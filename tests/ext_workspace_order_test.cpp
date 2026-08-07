#include "compositors/ext_workspace/ext_workspace_backend.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  std::vector<std::string> names(const std::vector<Workspace>& workspaces) {
    std::vector<std::string> result;
    result.reserve(workspaces.size());
    for (const auto& workspace : workspaces) {
      result.push_back(workspace.name);
    }
    return result;
  }

  // Pinnacle sends no coordinates event and hands out tags in hash order with
  // zero-based ids, which is what produced the scrambled "1 1 2 3 6 4 7 5 8" bar.
  std::vector<Workspace> pinnacleTags() {
    return {
        Workspace{.id = "4", .name = "5"}, Workspace{.id = "0", .name = "1"}, Workspace{.id = "2", .name = "3"},
        Workspace{.id = "8", .name = "9"}, Workspace{.id = "1", .name = "2"}, Workspace{.id = "6", .name = "7"},
        Workspace{.id = "3", .name = "4"}, Workspace{.id = "7", .name = "8"}, Workspace{.id = "5", .name = "6"},
    };
  }

} // namespace

int main() {
  bool ok = true;

  auto tags = pinnacleTags();
  ext_workspace::orderForDisplay(tags);
  ok &= check(
      names(tags) == std::vector<std::string>{"1", "2", "3", "4", "5", "6", "7", "8", "9"},
      "coordinate-less workspaces sort by numeric name"
  );

  bool indexed = true;
  for (std::size_t i = 0; i < tags.size(); ++i) {
    indexed &= tags[i].index == i + 1;
  }
  ok &= check(indexed, "display index is 1-based and contiguous");

  // The zero-based id that the label fallback used to drop must not reappear.
  ok &= check(tags.front().index == 1 && tags.front().id == "0", "zero id still gets index 1");

  std::vector<Workspace> coordinated{
      Workspace{.id = "a", .name = "9", .coordinates = {1}},
      Workspace{.id = "b", .name = "1", .coordinates = {0}},
  };
  ext_workspace::orderForDisplay(coordinated);
  ok &= check(names(coordinated) == std::vector<std::string>{"1", "9"}, "coordinates outrank the name tiebreak");

  // Named tags carry no numeric name, so creation order (numeric id) decides.
  std::vector<Workspace> named{
      Workspace{.id = "2", .name = "chat"},
      Workspace{.id = "0", .name = "web"},
      Workspace{.id = "1", .name = "term"},
  };
  ext_workspace::orderForDisplay(named);
  ok &= check(
      names(named) == std::vector<std::string>{"web", "term", "chat"}, "non-numeric names fall back to numeric id"
  );

  // Ids past the numeric range must not collapse into one bucket.
  std::vector<Workspace> textual{
      Workspace{.id = "ws-b", .name = "beta"},
      Workspace{.id = "ws-a", .name = "alpha"},
  };
  ext_workspace::orderForDisplay(textual);
  ok &= check(names(textual) == std::vector<std::string>{"alpha", "beta"}, "non-numeric ids sort lexicographically");

  std::vector<Workspace> empty;
  ext_workspace::orderForDisplay(empty);
  ok &= check(empty.empty(), "empty input stays empty");

  return ok ? 0 : 1;
}
