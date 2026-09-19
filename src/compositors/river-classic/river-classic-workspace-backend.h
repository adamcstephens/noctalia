#pragma once

#include "compositors/output_backend.h"
#include "compositors/workspace_backend.h"

#include <cstdint>
#include <deque>
#include <unordered_map>

struct wl_seat;
struct zriver_status_manager_v1;
struct zriver_control_v1;
struct zriver_output_status_v1;
struct zriver_seat_status_v1;
struct zriver_command_callback_v1;

class RiverClassicWorkspaceBackend final : public WorkspaceBackend,
                                           public OutputLifecycleObserver,
                                           public WorkspaceOutputNameResolver {
public:
  ~RiverClassicWorkspaceBackend() override;

  void bindRiverClassicStatus(zriver_status_manager_v1* manager);
  void bindRiverClassicControl(zriver_control_v1* control);
  void setSeat(wl_seat* seat);
  void setOutputNameResolver(Resolver resolver) override;

  [[nodiscard]] const char* backendName() const override { return "river-classic"; }
  [[nodiscard]] bool isAvailable() const noexcept override;
  void setChangeCallback(ChangeCallback callback) override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;
  void cleanup() override;

  void onOutputAdded(wl_output* output) override;
  void onOutputRemoved(wl_output* output) override;
  [[nodiscard]] wl_output* focusedOutput() const { return m_focusedOutput; }

private:
  struct OutputState {
    zriver_output_status_v1* handle = nullptr;
    std::uint32_t focused = 0;
    std::uint32_t occupied = 0;
    std::uint32_t urgent = 0;
  };

  struct Activation {
    wl_output* output;
    std::uint32_t tags;
    bool focusOutput;
  };

  void ensureOutputBound(wl_output* output);
  void ensureSeatBound();
  void releaseStatus();
  void cancelCommands();
  void enqueueActivation(wl_output* output, const std::string& id, bool focusOutput);
  void startNextActivation();
  void runCommand(const char* command, const std::string& argument, bool focusing);
  void onCommandFinished(zriver_command_callback_v1* callback, bool success, const char* message);
  void updateTags(zriver_output_status_v1* handle, std::uint32_t OutputState::* field, std::uint32_t tags);
  void notifyChanged();

  zriver_status_manager_v1* m_statusManager = nullptr;
  zriver_control_v1* m_control = nullptr;
  wl_seat* m_seat = nullptr;
  zriver_seat_status_v1* m_seatStatus = nullptr;
  wl_output* m_focusedOutput = nullptr;
  std::unordered_map<wl_output*, OutputState> m_outputs;
  std::unordered_map<zriver_output_status_v1*, wl_output*> m_outputByHandle;
  std::deque<Activation> m_activations;
  zriver_command_callback_v1* m_commandCallback = nullptr;
  bool m_focusing = false;
  Resolver m_outputNameResolver;
  ChangeCallback m_changeCallback;
};
