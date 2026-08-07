#pragma once

#include "compositors/workspace_backend.h"

#include <unordered_map>
#include <vector>

struct wl_array;
struct ext_workspace_group_handle_v1;
struct ext_workspace_handle_v1;

namespace ext_workspace {

  // Establishes the display order and fills in the 1-based Workspace::index.
  //
  // Coordinates are the only order the protocol defines, and the event is
  // optional: compositors that don't send it (Pinnacle) leave every workspace
  // comparing equal. Fall back to the numeric name, then the numeric id, which
  // tracks creation order where ids are monotonic.
  void orderForDisplay(std::vector<Workspace>& workspaces);

} // namespace ext_workspace

class ExtWorkspaceBackend final : public WorkspaceBackend, public ExtWorkspaceProtocolBinder {
public:
  ExtWorkspaceBackend();

  void bindExtWorkspace(ext_workspace_manager_v1* manager) override;

  [[nodiscard]] const char* backendName() const override { return "ext-workspace"; }
  [[nodiscard]] bool isAvailable() const noexcept override { return m_manager != nullptr; }
  void setChangeCallback(ChangeCallback callback) override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;
  void cleanup() override;

  void onGroupCreated(ext_workspace_group_handle_v1* group);
  void onGroupRemoved(ext_workspace_group_handle_v1* group);
  void onGroupOutputEnter(ext_workspace_group_handle_v1* group, wl_output* output);
  void onGroupOutputLeave(ext_workspace_group_handle_v1* group, wl_output* output);
  void onGroupWorkspaceEnter(ext_workspace_group_handle_v1* group, ext_workspace_handle_v1* workspace);
  void onGroupWorkspaceLeave(ext_workspace_group_handle_v1* group, ext_workspace_handle_v1* workspace);
  void onWorkspaceCreated(ext_workspace_handle_v1* workspace);
  void onWorkspaceIdChanged(ext_workspace_handle_v1* workspace, const char* id);
  void onWorkspaceNameChanged(ext_workspace_handle_v1* workspace, const char* name);
  void onWorkspaceCoordinatesChanged(ext_workspace_handle_v1* workspace, wl_array* coordinates);
  void onWorkspaceStateChanged(ext_workspace_handle_v1* workspace, std::uint32_t state);
  void onWorkspaceRemoved(ext_workspace_handle_v1* workspace);
  void onManagerDone();
  void onManagerFinished();

private:
  struct WorkspaceGroup {
    ext_workspace_group_handle_v1* handle = nullptr;
    std::vector<wl_output*> outputs;
    std::vector<ext_workspace_handle_v1*> workspaces;
  };

  [[nodiscard]] const WorkspaceGroup* groupForWorkspace(ext_workspace_handle_v1* workspace) const;
  void commitActivation(const WorkspaceGroup* group, ext_workspace_handle_v1* workspace);

  ext_workspace_manager_v1* m_manager = nullptr;
  std::vector<WorkspaceGroup> m_groups;
  std::unordered_map<ext_workspace_handle_v1*, Workspace> m_workspaces;
  ChangeCallback m_changeCallback;
  // activate() alone is unspecified: the protocol says it may or may not
  // deactivate the rest of the group. Pinnacle's tags are additive, so
  // deactivate the others explicitly.
  bool m_exclusiveActivation = false;
};
