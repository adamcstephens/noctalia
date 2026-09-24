#pragma once

#include "wayland/wayland_connection.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"

#include <unordered_map>

namespace compositors::river_classic {

  using OutputPowerControls = std::unordered_map<wl_output*, zwlr_output_power_v1*>;

  [[nodiscard]] bool setOutputPower(
      zwlr_output_power_manager_v1* manager, const std::vector<WaylandOutput>& outputs, OutputPowerControls& controls,
      bool on
  );
  void removeOutputPower(OutputPowerControls& controls, wl_output* output);
  void destroyOutputPowers(OutputPowerControls& controls);

} // namespace compositors::river_classic
