#include "compositors/river-classic/river-classic-output-power-backend.h"

namespace compositors::river_classic {

  namespace {

    void outputPowerMode(void*, zwlr_output_power_v1*, std::uint32_t) {}

    void outputPowerFailed(void*, zwlr_output_power_v1*) {}

    const zwlr_output_power_v1_listener kOutputPowerListener = {
        .mode = outputPowerMode,
        .failed = outputPowerFailed,
    };

  } // namespace

  bool setOutputPower(
      zwlr_output_power_manager_v1* manager, const std::vector<WaylandOutput>& outputs, OutputPowerControls& controls,
      bool on
  ) {
    if (manager == nullptr) {
      return false;
    }

    bool requested = false;
    for (const auto& output : outputs) {
      if (output.output == nullptr) {
        continue;
      }
      auto [it, inserted] = controls.try_emplace(output.output, nullptr);
      if (inserted) {
        it->second = zwlr_output_power_manager_v1_get_output_power(manager, output.output);
        if (it->second == nullptr) {
          controls.erase(it);
          continue;
        }
        zwlr_output_power_v1_add_listener(it->second, &kOutputPowerListener, nullptr);
      }

      zwlr_output_power_v1_set_mode(it->second, on ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
      requested = true;
    }
    return requested;
  }

  void removeOutputPower(OutputPowerControls& controls, wl_output* output) {
    const auto it = controls.find(output);
    if (it == controls.end()) {
      return;
    }

    zwlr_output_power_v1_destroy(it->second);
    controls.erase(it);
  }

  void destroyOutputPowers(OutputPowerControls& controls) {
    for (const auto& control : controls) {
      zwlr_output_power_v1_destroy(control.second);
    }
    controls.clear();
  }

} // namespace compositors::river_classic
