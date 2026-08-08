#include "dbus/logind/logind_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <cstdlib>
#include <fcntl.h>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace {
  constexpr Logger kLog("logind");

  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLogindObjectPath{"/org/freedesktop/login1"};
  // Alias for "the caller's session, or this user's display session if it has none".
  const sdbus::ObjectPath kLogindAutoSessionPath{"/org/freedesktop/login1/session/auto"};
  constexpr auto kLogindManagerInterface = "org.freedesktop.login1.Manager";
  constexpr auto kLogindSessionInterface = "org.freedesktop.login1.Session";

  // Inhibit locks stay alive until every duplicate of the fd is closed. Our detached
  // process launcher double-forks without closing fds, so without CLOEXEC `systemctl
  // suspend` inherits the sleep-delay inhibit and logind waits the full InhibitDelayMaxSec.
  [[nodiscard]] bool setCloexec(int fd) {
    if (fd < 0) {
      return false;
    }
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
      return false;
    }
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
  }

  [[nodiscard]] std::optional<sdbus::ObjectPath> resolveSessionPath(sdbus::IConnection& connection) {
    try {
      auto managerProxy = sdbus::createProxy(connection, kLogindBusName, kLogindObjectPath);

      if (const char* sessionId = std::getenv("XDG_SESSION_ID"); sessionId != nullptr && sessionId[0] != '\0') {
        try {
          sdbus::ObjectPath sessionPath;
          managerProxy->callMethod("GetSession")
              .onInterface(kLogindManagerInterface)
              .withArguments(std::string(sessionId))
              .storeResultsTo(sessionPath);
          return sessionPath;
        } catch (const sdbus::Error& e) {
          kLog.debug("failed to resolve logind session via XDG_SESSION_ID={}: {}", sessionId, e.what());
        }
      }

      try {
        sdbus::ObjectPath sessionPath;
        managerProxy->callMethod("GetSessionByPID")
            .onInterface(kLogindManagerInterface)
            .withArguments(static_cast<std::uint32_t>(::getpid()))
            .storeResultsTo(sessionPath);
        return sessionPath;
      } catch (const sdbus::Error& e) {
        // Running under systemd --user puts us in user@.service instead of a session
        // scope, so we own no session to look up.
        kLog.debug("failed to resolve logind session via pid: {}", e.what());
      }

      // Resolve the alias to its canonical path: logind emits session signals (Lock/Unlock)
      // on the real object path, so a proxy bound to the alias would never see them.
      auto autoProxy = sdbus::createProxy(connection, kLogindBusName, kLogindAutoSessionPath);
      const auto displaySessionId =
          autoProxy->getProperty("Id").onInterface(kLogindSessionInterface).get<std::string>();
      sdbus::ObjectPath sessionPath;
      managerProxy->callMethod("GetSession")
          .onInterface(kLogindManagerInterface)
          .withArguments(displaySessionId)
          .storeResultsTo(sessionPath);
      kLog.info("resolved logind display session {}", displaySessionId);
      return sessionPath;
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to resolve logind session: {}", e.what());
      return std::nullopt;
    }
  }
} // namespace

LogindService::LogindService(SystemBus& bus) : m_bus(bus) {
  m_managerProxy = sdbus::createProxy(m_bus.connection(), kLogindBusName, kLogindObjectPath);
  m_managerProxy->uponSignal("PrepareForSleep").onInterface(kLogindManagerInterface).call([this](bool sleeping) {
    if (m_prepareForSleepCallback) {
      m_prepareForSleepCallback(sleeping);
    }
  });
}

LogindService::~LogindService() {
  if (m_idleHint.value_or(false)) {
    setIdleHint(false);
  }
  releaseSleepDelayInhibit();
  releaseIdleInhibit();
}

void LogindService::ensureSessionProxy() {
  if (m_sessionProxy != nullptr) {
    return;
  }

  const auto sessionPath = resolveSessionPath(m_bus.connection());
  if (!sessionPath.has_value()) {
    kLog.warn("logind session features disabled: session path unavailable");
    return;
  }

  m_sessionProxy = sdbus::createProxy(m_bus.connection(), kLogindBusName, *sessionPath);
  m_sessionPath = sessionPath->c_str();
  m_sessionProxy->uponSignal("Lock").onInterface(kLogindSessionInterface).call([this]() {
    if (m_sessionLockIntegrationEnabled && m_lockCallback) {
      m_lockCallback();
    }
  });
  m_sessionProxy->uponSignal("Unlock").onInterface(kLogindSessionInterface).call([this]() {
    if (m_sessionLockIntegrationEnabled && m_unlockCallback) {
      m_unlockCallback();
    }
  });
  kLog.info("logind session resolved ({})", m_sessionPath);
}

void LogindService::setSessionLockIntegrationEnabled(bool enabled) {
  if (m_sessionLockIntegrationEnabled == enabled) {
    return;
  }
  m_sessionLockIntegrationEnabled = enabled;
  if (!enabled) {
    releaseSleepDelayInhibit();
    kLog.info("logind session lock monitor disabled");
    return;
  }
  ensureSessionProxy();
  if (m_sessionProxy != nullptr) {
    kLog.info("logind session lock monitor active ({})", m_sessionPath);
  }
}

void LogindService::setLockBeforeSuspendEnabled(bool enabled) {
  if (!m_sessionLockIntegrationEnabled || !enabled) {
    releaseSleepDelayInhibit();
    return;
  }
  (void)acquireSleepDelayInhibit();
}

void LogindService::setIdleHint(bool idle) {
  if (m_idleHintUnsupported || m_idleHint == idle) {
    return;
  }
  ensureSessionProxy();
  if (m_sessionProxy == nullptr) {
    return;
  }
  try {
    m_sessionProxy->callMethod("SetIdleHint").onInterface(kLogindSessionInterface).withArguments(idle);
    m_idleHint = idle;
    kLog.debug("logind idle hint set to {}", idle);
  } catch (const sdbus::Error& e) {
    // logind only accepts the hint on graphical sessions, so a shell started straight from a
    // tty (Type=tty) is refused every time. Latch instead of warning on every idle transition.
    if (e.getName() == "org.freedesktop.DBus.Error.NotSupported") {
      m_idleHintUnsupported = true;
      kLog.info("logind idle hint unsupported for this session (needs Type=x11/wayland): {}", e.getMessage());
      return;
    }
    kLog.warn("failed to set logind idle hint: {}", e.what());
  }
}

void LogindService::setPrepareForSleepCallback(PrepareForSleepCallback callback) {
  m_prepareForSleepCallback = std::move(callback);
}

void LogindService::setLockCallback(SessionLockCallback callback) { m_lockCallback = std::move(callback); }

void LogindService::setUnlockCallback(SessionLockCallback callback) { m_unlockCallback = std::move(callback); }

void LogindService::setSessionLockedHint(bool locked) {
  if (!m_sessionLockIntegrationEnabled || m_sessionProxy == nullptr) {
    return;
  }
  try {
    m_sessionProxy->callMethod("SetLockedHint").onInterface(kLogindSessionInterface).withArguments(locked);
    kLog.debug("logind session locked hint set to {}", locked);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to set logind session locked hint to {}: {}", locked, e.what());
  }
}

bool LogindService::supportsIdleInhibit() const noexcept { return m_managerProxy != nullptr; }

bool LogindService::hasIdleInhibit() const noexcept { return m_idleInhibitFd >= 0; }

bool LogindService::acquireIdleInhibit() {
  if (m_idleInhibitFd >= 0) {
    return true;
  }
  if (m_managerProxy == nullptr) {
    return false;
  }

  try {
    sdbus::UnixFd fd;
    m_managerProxy->callMethod("Inhibit")
        .onInterface(kLogindManagerInterface)
        .withArguments(std::string("idle"), std::string("noctalia"), std::string("Caffeine"), std::string("block"))
        .storeResultsTo(fd);
    m_idleInhibitFd = fd.release();
    if (m_idleInhibitFd < 0) {
      kLog.warn("logind idle inhibit returned invalid fd");
      return false;
    }
    if (!setCloexec(m_idleInhibitFd)) {
      kLog.warn("failed to set CLOEXEC on logind idle inhibit fd");
      ::close(m_idleInhibitFd);
      m_idleInhibitFd = -1;
      return false;
    }
    kLog.info("logind idle inhibit acquired");
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to acquire logind idle inhibit: {}", e.what());
    return false;
  }
}

void LogindService::releaseIdleInhibit() {
  if (m_idleInhibitFd < 0) {
    return;
  }
  ::close(m_idleInhibitFd);
  m_idleInhibitFd = -1;
  kLog.debug("logind idle inhibit released");
}

bool LogindService::hasSleepDelayInhibit() const noexcept { return m_sleepDelayInhibitFd >= 0; }

bool LogindService::acquireSleepDelayInhibit() {
  if (m_sleepDelayInhibitFd >= 0) {
    return true;
  }
  if (m_managerProxy == nullptr) {
    return false;
  }

  try {
    sdbus::UnixFd fd;
    m_managerProxy->callMethod("Inhibit")
        .onInterface(kLogindManagerInterface)
        .withArguments(
            std::string("sleep"), std::string("noctalia"), std::string("Lock before sleep"), std::string("delay")
        )
        .storeResultsTo(fd);
    m_sleepDelayInhibitFd = fd.release();
    if (m_sleepDelayInhibitFd < 0) {
      kLog.warn("logind sleep delay inhibit returned invalid fd");
      return false;
    }
    if (!setCloexec(m_sleepDelayInhibitFd)) {
      kLog.warn("failed to set CLOEXEC on logind sleep delay inhibit fd");
      ::close(m_sleepDelayInhibitFd);
      m_sleepDelayInhibitFd = -1;
      return false;
    }
    kLog.info("logind sleep delay inhibit acquired");
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to acquire logind sleep delay inhibit: {}", e.what());
    return false;
  }
}

void LogindService::releaseSleepDelayInhibit() {
  if (m_sleepDelayInhibitFd < 0) {
    return;
  }
  ::close(m_sleepDelayInhibitFd);
  m_sleepDelayInhibitFd = -1;
  kLog.info("logind sleep delay inhibit released");
}
