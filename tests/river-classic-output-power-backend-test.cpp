#include "compositors/river-classic/river-classic-output-power-backend.h"
#include "tests/test_check.h"

#include <cstdarg>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

  struct FakeProxy {
    const wl_interface* interface;
    std::uint32_t version;
    bool alive = true;
  };

  struct Request {
    wl_output* output;
    std::uint32_t mode;
  };

  std::vector<std::unique_ptr<FakeProxy>> proxies;
  std::vector<Request> requests;
  std::vector<std::pair<zwlr_output_power_v1*, wl_output*>> outputPowers;

  FakeProxy& proxy(void* handle) { return *static_cast<FakeProxy*>(handle); }

  template <typename T> T* makeProxy(const wl_interface* interface, std::uint32_t version) {
    proxies.push_back(std::make_unique<FakeProxy>(FakeProxy{interface, version}));
    return reinterpret_cast<T*>(proxies.back().get());
  }

  wl_output* outputFor(zwlr_output_power_v1* power) {
    for (const auto& [candidate, output] : outputPowers) {
      if (candidate == power) {
        return output;
      }
    }
    TEST_CHECK(false);
    return nullptr;
  }

} // namespace

extern "C" {

std::uint32_t wl_proxy_get_version(wl_proxy* handle) { return proxy(handle).version; }

void wl_proxy_destroy(wl_proxy* handle) {
  TEST_CHECK(proxy(handle).alive);
  proxy(handle).alive = false;
}

int wl_proxy_add_listener(wl_proxy* handle, void (**implementation)(void), void* data) {
  TEST_CHECK(proxy(handle).interface == &zwlr_output_power_v1_interface);
  TEST_CHECK(implementation != nullptr);
  TEST_CHECK(data == nullptr);
  return 0;
}

wl_proxy* wl_proxy_marshal_flags(
    wl_proxy* handle, std::uint32_t opcode, const wl_interface* interface, std::uint32_t version, std::uint32_t flags,
    ...
) {
  TEST_CHECK(proxy(handle).alive);

  va_list args;
  va_start(args, flags);
  if (proxy(handle).interface == &zwlr_output_power_manager_v1_interface) {
    TEST_CHECK(opcode == ZWLR_OUTPUT_POWER_MANAGER_V1_GET_OUTPUT_POWER);
    (void)va_arg(args, void*);
    auto* output = va_arg(args, wl_output*);
    auto* power = makeProxy<zwlr_output_power_v1>(interface, version);
    outputPowers.emplace_back(power, output);
    va_end(args);
    return reinterpret_cast<wl_proxy*>(power);
  }

  TEST_CHECK(proxy(handle).interface == &zwlr_output_power_v1_interface);
  if (opcode == ZWLR_OUTPUT_POWER_V1_SET_MODE) {
    requests.push_back({outputFor(reinterpret_cast<zwlr_output_power_v1*>(handle)), va_arg(args, std::uint32_t)});
  } else {
    TEST_CHECK(opcode == ZWLR_OUTPUT_POWER_V1_DESTROY);
    TEST_CHECK((flags & WL_MARSHAL_FLAG_DESTROY) != 0);
    proxy(handle).alive = false;
  }
  va_end(args);
  return nullptr;
}

} // extern "C"

int main() {
  auto* manager = makeProxy<zwlr_output_power_manager_v1>(&zwlr_output_power_manager_v1_interface, 1);
  auto* first = makeProxy<wl_output>(&wl_output_interface, 4);
  auto* second = makeProxy<wl_output>(&wl_output_interface, 4);
  const std::vector<WaylandOutput> outputs = {{.output = first}, {.output = nullptr}, {.output = second}};
  compositors::river_classic::OutputPowerControls controls;

  TEST_CHECK(!compositors::river_classic::setOutputPower(nullptr, outputs, controls, false));
  TEST_CHECK(compositors::river_classic::setOutputPower(manager, outputs, controls, false));
  TEST_CHECK(requests.size() == 2);
  TEST_CHECK(outputPowers.size() == 2);
  TEST_CHECK(requests[0].output == first);
  TEST_CHECK(requests[0].mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF);
  TEST_CHECK(requests[1].output == second);
  TEST_CHECK(requests[1].mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF);

  TEST_CHECK(compositors::river_classic::setOutputPower(manager, outputs, controls, true));
  TEST_CHECK(requests.size() == 4);
  TEST_CHECK(outputPowers.size() == 2);
  TEST_CHECK(requests[2].output == first);
  TEST_CHECK(requests[2].mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
  TEST_CHECK(requests[3].output == second);
  TEST_CHECK(requests[3].mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);

  compositors::river_classic::removeOutputPower(controls, first);
  TEST_CHECK(controls.size() == 1);
  TEST_CHECK(!proxy(outputPowers[0].first).alive);
  TEST_CHECK(proxy(outputPowers[1].first).alive);
  compositors::river_classic::destroyOutputPowers(controls);
  TEST_CHECK(controls.empty());
  TEST_CHECK(!proxy(outputPowers[1].first).alive);
}
