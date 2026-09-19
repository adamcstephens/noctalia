#include "compositors/river-classic/river-classic-workspace-backend.h"

#include "core/log.h"
#include "river-classic-control-unstable-v1-client-protocol.h"
#include "river-classic-status-unstable-v1-client-protocol.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <utility>

namespace {

  constexpr Logger kLog("river-classic-workspace");

} // namespace

RiverClassicWorkspaceBackend::~RiverClassicWorkspaceBackend() { cleanup(); }

void RiverClassicWorkspaceBackend::bindRiverClassicStatus(zriver_status_manager_v1* manager) {
  if (manager == nullptr || manager == m_statusManager) {
    return;
  }
  cancelCommands();
  releaseStatus();
  m_statusManager = manager;
  for (const auto& [output, _] : m_outputs) {
    ensureOutputBound(output);
  }
  ensureSeatBound();
}

void RiverClassicWorkspaceBackend::bindRiverClassicControl(zriver_control_v1* control) {
  if (control == nullptr || control == m_control) {
    return;
  }
  cancelCommands();
  if (m_control != nullptr) {
    zriver_control_v1_destroy(m_control);
  }
  m_control = control;
}

void RiverClassicWorkspaceBackend::setSeat(wl_seat* seat) {
  if (seat == m_seat) {
    return;
  }
  cancelCommands();
  if (m_seatStatus != nullptr) {
    zriver_seat_status_v1_destroy(m_seatStatus);
    m_seatStatus = nullptr;
  }
  m_seat = seat;
  m_focusedOutput = nullptr;
  ensureSeatBound();
  notifyChanged();
}

void RiverClassicWorkspaceBackend::setOutputNameResolver(Resolver resolver) {
  m_outputNameResolver = std::move(resolver);
}

bool RiverClassicWorkspaceBackend::isAvailable() const noexcept {
  return m_statusManager != nullptr && m_control != nullptr && m_seat != nullptr;
}

void RiverClassicWorkspaceBackend::setChangeCallback(ChangeCallback callback) {
  m_changeCallback = std::move(callback);
}

void RiverClassicWorkspaceBackend::activate(const std::string& id) { enqueueActivation(m_focusedOutput, id, false); }

void RiverClassicWorkspaceBackend::activateForOutput(wl_output* output, const std::string& id) {
  if (output == nullptr) {
    activate(id);
    return;
  }
  enqueueActivation(output, id, true);
}

void RiverClassicWorkspaceBackend::activateForOutput(wl_output* output, const Workspace& workspace) {
  activateForOutput(output, workspace.id);
}

std::vector<Workspace> RiverClassicWorkspaceBackend::all() const { return forOutput(m_focusedOutput); }

std::vector<Workspace> RiverClassicWorkspaceBackend::forOutput(wl_output* output) const {
  const auto it = m_outputs.find(output != nullptr ? output : m_focusedOutput);
  if (it == m_outputs.end() || it->second.handle == nullptr) {
    return {};
  }
  const auto& state = it->second;
  std::vector<Workspace> result;
  result.reserve(32);
  for (std::uint32_t index = 0; index < 32; ++index) {
    const auto mask = std::uint32_t{1} << index;
    const auto id = std::to_string(index + 1);
    result.push_back(
        Workspace{
            .id = id,
            .name = id,
            .coordinates = {index},
            .index = index + 1,
            .active = (state.focused & mask) != 0,
            .urgent = (state.urgent & mask) != 0,
            .occupied = (state.occupied & mask) != 0,
        }
    );
  }
  return result;
}

void RiverClassicWorkspaceBackend::cleanup() {
  cancelCommands();
  releaseStatus();
  if (m_control != nullptr) {
    zriver_control_v1_destroy(m_control);
    m_control = nullptr;
  }
  m_outputs.clear();
  m_seat = nullptr;
}

void RiverClassicWorkspaceBackend::onOutputAdded(wl_output* output) {
  if (output == nullptr) {
    return;
  }
  m_outputs.try_emplace(output);
  ensureOutputBound(output);
}

void RiverClassicWorkspaceBackend::onOutputRemoved(wl_output* output) {
  const auto it = m_outputs.find(output);
  if (it == m_outputs.end()) {
    return;
  }
  if (it->second.handle != nullptr) {
    m_outputByHandle.erase(it->second.handle);
    zriver_output_status_v1_destroy(it->second.handle);
  }
  m_outputs.erase(it);
  if (m_focusedOutput == output) {
    m_focusedOutput = nullptr;
  }
  if (!m_activations.empty() && m_activations.front().output == output && m_commandCallback != nullptr) {
    zriver_command_callback_v1_destroy(m_commandCallback);
    m_commandCallback = nullptr;
  }
  std::erase_if(m_activations, [output](const Activation& activation) { return activation.output == output; });
  startNextActivation();
  notifyChanged();
}

void RiverClassicWorkspaceBackend::ensureOutputBound(wl_output* output) {
  auto it = m_outputs.find(output);
  if (m_statusManager == nullptr || it == m_outputs.end() || it->second.handle != nullptr) {
    return;
  }
  static const zriver_output_status_v1_listener listener = {
      .focused_tags =
          [](void* data, zriver_output_status_v1* handle, std::uint32_t tags) {
            static_cast<RiverClassicWorkspaceBackend*>(data)->updateTags(handle, &OutputState::focused, tags);
          },
      .view_tags =
          [](void* data, zriver_output_status_v1* handle, wl_array* tags) {
            std::uint32_t occupied = 0;
            const auto* bytes = static_cast<const unsigned char*>(tags->data);
            for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= tags->size;
                 offset += sizeof(std::uint32_t)) {
              std::uint32_t viewTags;
              std::memcpy(&viewTags, bytes + offset, sizeof(viewTags));
              occupied |= viewTags;
            }
            static_cast<RiverClassicWorkspaceBackend*>(data)->updateTags(handle, &OutputState::occupied, occupied);
          },
      .urgent_tags =
          [](void* data, zriver_output_status_v1* handle, std::uint32_t tags) {
            static_cast<RiverClassicWorkspaceBackend*>(data)->updateTags(handle, &OutputState::urgent, tags);
          },
      .layout_name = [](void*, zriver_output_status_v1*, const char*) {},
      .layout_name_clear = [](void*, zriver_output_status_v1*) {},
  };
  auto* handle = zriver_status_manager_v1_get_river_output_status(m_statusManager, output);
  if (handle == nullptr) {
    return;
  }
  it->second.handle = handle;
  m_outputByHandle.emplace(handle, output);
  zriver_output_status_v1_add_listener(handle, &listener, this);
}

void RiverClassicWorkspaceBackend::ensureSeatBound() {
  if (m_statusManager == nullptr || m_seat == nullptr || m_seatStatus != nullptr) {
    return;
  }
  static const zriver_seat_status_v1_listener listener = {
      .focused_output =
          [](void* data, zriver_seat_status_v1*, wl_output* output) {
            auto& backend = *static_cast<RiverClassicWorkspaceBackend*>(data);
            if (backend.m_outputs.contains(output) && backend.m_focusedOutput != output) {
              backend.m_focusedOutput = output;
              backend.notifyChanged();
            }
          },
      .unfocused_output =
          [](void* data, zriver_seat_status_v1*, wl_output* output) {
            auto& backend = *static_cast<RiverClassicWorkspaceBackend*>(data);
            if (backend.m_focusedOutput == output) {
              backend.m_focusedOutput = nullptr;
              backend.notifyChanged();
            }
          },
      .focused_view = [](void*, zriver_seat_status_v1*, const char*) {},
      .mode = [](void*, zriver_seat_status_v1*, const char*) {},
  };
  m_seatStatus = zriver_status_manager_v1_get_river_seat_status(m_statusManager, m_seat);
  if (m_seatStatus != nullptr) {
    zriver_seat_status_v1_add_listener(m_seatStatus, &listener, this);
  }
}

void RiverClassicWorkspaceBackend::releaseStatus() {
  if (m_seatStatus != nullptr) {
    zriver_seat_status_v1_destroy(m_seatStatus);
    m_seatStatus = nullptr;
  }
  m_focusedOutput = nullptr;
  for (auto& [_, state] : m_outputs) {
    if (state.handle != nullptr) {
      zriver_output_status_v1_destroy(state.handle);
    }
    state = {};
  }
  m_outputByHandle.clear();
  if (m_statusManager != nullptr) {
    zriver_status_manager_v1_destroy(m_statusManager);
    m_statusManager = nullptr;
  }
}

void RiverClassicWorkspaceBackend::cancelCommands() {
  if (m_commandCallback != nullptr) {
    zriver_command_callback_v1_destroy(m_commandCallback);
    m_commandCallback = nullptr;
  }
  m_activations.clear();
}

void RiverClassicWorkspaceBackend::enqueueActivation(wl_output* output, const std::string& id, bool focusOutput) {
  if (!isAvailable() || output == nullptr || !m_outputs.contains(output)) {
    return;
  }
  std::uint32_t tag = 0;
  const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), tag);
  if (error != std::errc{} || end != id.data() + id.size() || tag == 0 || tag > 32) {
    return;
  }
  m_activations.push_back({output, std::uint32_t{1} << (tag - 1), focusOutput});
  startNextActivation();
}

void RiverClassicWorkspaceBackend::startNextActivation() {
  if (m_commandCallback != nullptr || !isAvailable()) {
    return;
  }
  while (!m_activations.empty()) {
    const auto& activation = m_activations.front();
    if (!m_outputs.contains(activation.output)) {
      m_activations.pop_front();
      continue;
    }
    if (activation.focusOutput || activation.output != m_focusedOutput) {
      const auto name = m_outputNameResolver ? m_outputNameResolver(activation.output) : std::string{};
      if (name.empty()) {
        kLog.warn("cannot focus output without a connector name");
        m_activations.pop_front();
        continue;
      }
      runCommand("focus-output", name, true);
    } else {
      runCommand("set-focused-tags", std::to_string(activation.tags), false);
    }
    if (m_commandCallback != nullptr) {
      return;
    }
    m_activations.pop_front();
  }
}

void RiverClassicWorkspaceBackend::runCommand(const char* command, const std::string& argument, bool focusing) {
  static const zriver_command_callback_v1_listener listener = {
      .success = [](
                     void* data, zriver_command_callback_v1* callback, const char* message
                 ) { static_cast<RiverClassicWorkspaceBackend*>(data)->onCommandFinished(callback, true, message); },
      .failure = [](
                     void* data, zriver_command_callback_v1* callback, const char* message
                 ) { static_cast<RiverClassicWorkspaceBackend*>(data)->onCommandFinished(callback, false, message); },
  };
  zriver_control_v1_add_argument(m_control, command);
  zriver_control_v1_add_argument(m_control, argument.c_str());
  m_commandCallback = zriver_control_v1_run_command(m_control, m_seat);
  m_focusing = focusing;
  if (m_commandCallback != nullptr) {
    zriver_command_callback_v1_add_listener(m_commandCallback, &listener, this);
  } else {
    kLog.warn("failed to allocate {} callback", command);
  }
}

void RiverClassicWorkspaceBackend::onCommandFinished(
    zriver_command_callback_v1* callback, bool success, const char* message
) {
  if (callback != m_commandCallback) {
    return;
  }
  zriver_command_callback_v1_destroy(callback);
  m_commandCallback = nullptr;
  if (!success) {
    kLog.warn("{} failed: {}", m_focusing ? "focus-output" : "set-focused-tags", message != nullptr ? message : "");
  }
  if (m_activations.empty()) {
    return;
  }
  const auto& activation = m_activations.front();
  if (success && m_focusing && isAvailable() && m_outputs.contains(activation.output)) {
    runCommand("set-focused-tags", std::to_string(activation.tags), false);
    if (m_commandCallback != nullptr) {
      return;
    }
  }
  m_activations.pop_front();
  startNextActivation();
}

void RiverClassicWorkspaceBackend::updateTags(
    zriver_output_status_v1* handle, std::uint32_t OutputState::* field, std::uint32_t tags
) {
  const auto it = m_outputByHandle.find(handle);
  if (it == m_outputByHandle.end()) {
    return;
  }
  auto& current = m_outputs.at(it->second).*field;
  if (current != tags) {
    current = tags;
    notifyChanged();
  }
}

void RiverClassicWorkspaceBackend::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
