#include "compositors/river-classic/river-classic-workspace-backend.h"
#include "river-classic-control-unstable-v1-client-protocol.h"
#include "river-classic-status-unstable-v1-client-protocol.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

  struct FakeProxy {
    const wl_interface* interface;
    std::uint32_t version;
    void (**listener)(void) = nullptr;
    void* data = nullptr;
    bool alive = true;
  };

  struct Command {
    std::vector<std::string> arguments;
    wl_seat* seat;
    zriver_command_callback_v1* callback;
  };

  std::vector<std::unique_ptr<FakeProxy>> proxies;
  std::vector<std::string> arguments;
  std::vector<Command> commands;
  std::vector<std::pair<wl_output*, zriver_output_status_v1*>> outputStatuses;
  zriver_seat_status_v1* seatStatus = nullptr;

  FakeProxy& proxy(void* handle) { return *static_cast<FakeProxy*>(handle); }

  template <typename T> T* makeProxy(const wl_interface* interface, std::uint32_t version) {
    proxies.push_back(std::make_unique<FakeProxy>(FakeProxy{interface, version}));
    return reinterpret_cast<T*>(proxies.back().get());
  }

  template <typename T> const T& listener(void* handle) {
    TEST_CHECK(proxy(handle).alive);
    TEST_CHECK(proxy(handle).listener != nullptr);
    return *reinterpret_cast<const T*>(proxy(handle).listener);
  }

  zriver_output_status_v1* statusFor(wl_output* output) {
    for (auto it = outputStatuses.rbegin(); it != outputStatuses.rend(); ++it) {
      if (it->first == output && proxy(it->second).alive) {
        return it->second;
      }
    }
    TEST_CHECK(false);
    return nullptr;
  }

  void focus(wl_output* output) {
    listener<zriver_seat_status_v1_listener>(seatStatus).focused_output(proxy(seatStatus).data, seatStatus, output);
  }

  void focusedTags(wl_output* output, std::uint32_t tags) {
    auto* status = statusFor(output);
    listener<zriver_output_status_v1_listener>(status).focused_tags(proxy(status).data, status, tags);
  }

  void viewTags(wl_output* output, std::vector<std::uint32_t> tags) {
    auto* status = statusFor(output);
    wl_array array{tags.size() * sizeof(std::uint32_t), tags.size() * sizeof(std::uint32_t), tags.data()};
    listener<zriver_output_status_v1_listener>(status).view_tags(proxy(status).data, status, &array);
  }

  void urgentTags(wl_output* output, std::uint32_t tags) {
    auto* status = statusFor(output);
    listener<zriver_output_status_v1_listener>(status).urgent_tags(proxy(status).data, status, tags);
  }

  void finish(std::size_t index, bool success) {
    auto* callback = commands.at(index).callback;
    const auto& callbacks = listener<zriver_command_callback_v1_listener>(callback);
    if (success) {
      callbacks.success(proxy(callback).data, callback, "");
    } else {
      callbacks.failure(proxy(callback).data, callback, "Invalid output indicator");
    }
    TEST_CHECK(!proxy(callback).alive);
  }

} // namespace

extern "C" {

std::uint32_t wl_proxy_get_version(wl_proxy* handle) { return proxy(handle).version; }

int wl_proxy_add_listener(wl_proxy* handle, void (**implementation)(void), void* data) {
  TEST_CHECK(proxy(handle).alive);
  TEST_CHECK(proxy(handle).listener == nullptr);
  proxy(handle).listener = implementation;
  proxy(handle).data = data;
  return 0;
}

void wl_proxy_destroy(wl_proxy* handle) {
  TEST_CHECK(proxy(handle).alive);
  proxy(handle).alive = false;
}

wl_proxy* wl_proxy_marshal_flags(
    wl_proxy* handle, std::uint32_t opcode, const wl_interface* interface, std::uint32_t version, std::uint32_t flags,
    ...
) {
  TEST_CHECK(proxy(handle).alive);
  if ((flags & WL_MARSHAL_FLAG_DESTROY) != 0) {
    wl_proxy_destroy(handle);
    return nullptr;
  }

  va_list args;
  va_start(args, flags);
  wl_proxy* result = nullptr;
  if (proxy(handle).interface == &zriver_status_manager_v1_interface) {
    (void)va_arg(args, void*);
    result = makeProxy<wl_proxy>(interface, version);
    if (opcode == ZRIVER_STATUS_MANAGER_V1_GET_RIVER_OUTPUT_STATUS) {
      outputStatuses.emplace_back(va_arg(args, wl_output*), reinterpret_cast<zriver_output_status_v1*>(result));
    } else {
      TEST_CHECK(opcode == ZRIVER_STATUS_MANAGER_V1_GET_RIVER_SEAT_STATUS);
      TEST_CHECK(va_arg(args, wl_seat*) != nullptr);
      seatStatus = reinterpret_cast<zriver_seat_status_v1*>(result);
    }
  } else {
    TEST_CHECK(proxy(handle).interface == &zriver_control_v1_interface);
    if (opcode == ZRIVER_CONTROL_V1_ADD_ARGUMENT) {
      arguments.emplace_back(va_arg(args, const char*));
    } else {
      TEST_CHECK(opcode == ZRIVER_CONTROL_V1_RUN_COMMAND);
      auto* seat = va_arg(args, wl_seat*);
      (void)va_arg(args, void*);
      result = makeProxy<wl_proxy>(interface, version);
      commands.push_back({std::move(arguments), seat, reinterpret_cast<zriver_command_callback_v1*>(result)});
      arguments.clear();
    }
  }
  va_end(args);
  return result;
}

} // extern "C"

int main() {
  RiverClassicWorkspaceBackend backend;
  auto* first = makeProxy<wl_output>(&wl_output_interface, 4);
  auto* second = makeProxy<wl_output>(&wl_output_interface, 4);
  auto* seat = makeProxy<wl_seat>(&wl_seat_interface, 7);
  auto* manager = makeProxy<zriver_status_manager_v1>(&zriver_status_manager_v1_interface, 4);
  auto* control = makeProxy<zriver_control_v1>(&zriver_control_v1_interface, 1);
  int changes = 0;
  backend.setChangeCallback([&changes]() { ++changes; });
  backend.setOutputNameResolver([=](wl_output* output) { return output == first ? "DP-1" : "DP-2"; });

  backend.onOutputAdded(first);
  backend.setSeat(seat);
  backend.bindRiverClassicControl(control);
  TEST_CHECK(!backend.isAvailable());
  backend.bindRiverClassicStatus(manager);
  TEST_CHECK(backend.isAvailable());
  backend.onOutputAdded(second);
  backend.onOutputAdded(second);
  TEST_CHECK(outputStatuses.size() == 2);
  focus(first);
  focusedTags(first, 0x80000001U);
  focusedTags(second, 2U);
  viewTags(first, {1U, 0x80000004U});
  urgentTags(first, 0x80000000U);
  const auto firstTags = backend.forOutput(first);
  TEST_CHECK(firstTags.size() == 32);
  for (std::uint32_t index = 0; index < 32; ++index) {
    TEST_CHECK(firstTags[index].id == std::to_string(index + 1));
    TEST_CHECK(firstTags[index].name == firstTags[index].id);
    TEST_CHECK(firstTags[index].index == index + 1);
    TEST_CHECK(firstTags[index].coordinates == std::vector<std::uint32_t>{index});
  }
  TEST_CHECK(firstTags[0].active && firstTags[31].active);
  TEST_CHECK(firstTags[0].occupied && firstTags[2].occupied && firstTags[31].occupied);
  TEST_CHECK(firstTags[31].urgent && !firstTags[0].urgent);
  TEST_CHECK(backend.forOutput(second)[1].active && !backend.forOutput(second)[31].occupied);
  TEST_CHECK(backend.focusedOutput() == first && backend.all()[31].active);
  const int previousChanges = changes;
  viewTags(first, {2U});
  urgentTags(first, 0);
  TEST_CHECK(changes > previousChanges);
  TEST_CHECK(!backend.forOutput(first)[0].occupied && !backend.forOutput(first)[31].occupied);
  TEST_CHECK(backend.forOutput(first)[1].occupied && !backend.forOutput(first)[31].urgent);
  viewTags(first, {});
  TEST_CHECK(!backend.forOutput(first)[1].occupied);
  TEST_CHECK(backend.workspaceWindows(first).empty() && backend.appIdsByWorkspace(first).empty());

  for (const auto* id : {"", "0", "33", "-1", "+1", "1suffix", " 1", "1 ", "4294967297"}) {
    backend.activateForOutput(first, id);
  }
  TEST_CHECK(commands.empty());
  backend.activate("32");
  TEST_CHECK(commands.size() == 1);
  TEST_CHECK(commands[0].arguments == std::vector<std::string>({"set-focused-tags", "2147483648"}));
  TEST_CHECK(commands[0].seat == seat);
  finish(0, true);

  backend.activateForOutput(second, "2");
  TEST_CHECK(commands.size() == 2);
  TEST_CHECK(commands[1].arguments == std::vector<std::string>({"focus-output", "DP-2"}));
  finish(1, false);
  TEST_CHECK(commands.size() == 2);
  backend.activateForOutput(second, firstTags[31]);
  TEST_CHECK(commands.size() == 3);
  finish(2, true);
  TEST_CHECK(commands.size() == 4);
  TEST_CHECK(commands[3].arguments == std::vector<std::string>({"set-focused-tags", "2147483648"}));
  finish(3, true);

  backend.activateForOutput(first, "1");
  backend.activateForOutput(second, "2");
  TEST_CHECK(commands.size() == 5);
  finish(4, true);
  TEST_CHECK(commands.size() == 6);
  TEST_CHECK(commands[5].arguments == std::vector<std::string>({"set-focused-tags", "1"}));
  finish(5, true);
  TEST_CHECK(commands.size() == 7);
  TEST_CHECK(commands[6].arguments == std::vector<std::string>({"focus-output", "DP-2"}));
  finish(6, true);
  TEST_CHECK(commands.size() == 8);
  finish(7, true);

  focus(second);
  listener<zriver_seat_status_v1_listener>(seatStatus).unfocused_output(proxy(seatStatus).data, seatStatus, first);
  TEST_CHECK(backend.focusedOutput() == second);
  backend.activateForOutput(second, "3");
  auto* removedStatus = statusFor(second);
  auto* canceled = commands.back().callback;
  const auto countBeforeRemoval = commands.size();
  backend.onOutputRemoved(second);
  TEST_CHECK(!proxy(removedStatus).alive && !proxy(canceled).alive);
  TEST_CHECK(backend.focusedOutput() == nullptr && backend.forOutput(second).empty());
  backend.activateForOutput(second, "1");
  backend.activate("1");
  TEST_CHECK(commands.size() == countBeforeRemoval);
  backend.onOutputAdded(second);
  TEST_CHECK(statusFor(second) != removedStatus);
  TEST_CHECK(!backend.forOutput(second)[1].active && !backend.forOutput(second)[1].occupied);
  backend.activateForOutput(second, "1");
  finish(commands.size() - 1, true);
  auto* removedTagsCallback = commands.back().callback;
  backend.activateForOutput(first, "4");
  backend.activateForOutput(second, "5");
  const auto countBeforeQueuedRemoval = commands.size();
  backend.onOutputRemoved(second);
  TEST_CHECK(!proxy(removedTagsCallback).alive);
  TEST_CHECK(commands.size() == countBeforeQueuedRemoval + 1);
  TEST_CHECK(commands.back().arguments == std::vector<std::string>({"focus-output", "DP-1"}));
  finish(commands.size() - 1, true);
  TEST_CHECK(commands.back().arguments == std::vector<std::string>({"set-focused-tags", "8"}));
  finish(commands.size() - 1, false);
  TEST_CHECK(commands.size() == countBeforeQueuedRemoval + 2);
  backend.onOutputAdded(second);
  backend.activateForOutput(second, "1");
  auto* cleanupCallback = commands.back().callback;
  backend.cleanup();
  backend.cleanup();
  TEST_CHECK(!backend.isAvailable() && backend.focusedOutput() == nullptr && backend.all().empty());
  TEST_CHECK(!proxy(cleanupCallback).alive && !proxy(manager).alive && !proxy(control).alive);
  TEST_CHECK(proxy(first).alive && proxy(second).alive && proxy(seat).alive);
  TEST_CHECK(std::all_of(proxies.begin(), proxies.end(), [](const auto& handle) {
    return handle->interface == &wl_output_interface || handle->interface == &wl_seat_interface || !handle->alive;
  }));

  RiverClassicWorkspaceBackend lateSeat;
  lateSeat.bindRiverClassicStatus(makeProxy<zriver_status_manager_v1>(&zriver_status_manager_v1_interface, 2));
  lateSeat.onOutputAdded(first);
  lateSeat.bindRiverClassicControl(makeProxy<zriver_control_v1>(&zriver_control_v1_interface, 1));
  TEST_CHECK(!lateSeat.isAvailable());
  lateSeat.setSeat(seat);
  TEST_CHECK(lateSeat.isAvailable());
  focus(first);
  lateSeat.activate("1");
  auto* oldCallback = commands.back().callback;
  auto* oldSeatStatus = seatStatus;
  lateSeat.setSeat(nullptr);
  TEST_CHECK(!lateSeat.isAvailable() && !proxy(oldCallback).alive && !proxy(oldSeatStatus).alive);
  lateSeat.cleanup();
  return 0;
}
