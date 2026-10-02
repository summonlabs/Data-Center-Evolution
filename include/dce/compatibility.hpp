// The compatibility graph.
//
// Nodes are (component, version) pairs. Edges are evidence-backed statements
// that a component may move from one version to another, with the capabilities
// that must already be present and the capabilities that appear afterwards.
// An edge without provenance is not a permission, and the graph analysis says
// so rather than silently trusting it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/capability.hpp"
#include "dce/evidence.hpp"
#include "dce/ids.hpp"
#include "dce/limits.hpp"
#include "dce/status.hpp"
#include "dce/version.hpp"

namespace dce {

enum class TransitionKind : std::uint8_t {
  upgrade,
  downgrade,
  reinstall,
};

[[nodiscard]] const char* to_string(TransitionKind kind) noexcept;

struct ComponentVersion {
  ComponentId component;
  Version version;

  auto operator<=>(const ComponentVersion&) const = default;
};

struct CompatEdge {
  ComponentId component;
  Version from;
  Version to;
  TransitionKind kind{TransitionKind::upgrade};
  CapabilitySet requires_capabilities;
  CapabilitySet provides_capabilities;
  bool reversible{false};
  EvidenceId provenance;
  std::string justification;

  auto operator<=>(const CompatEdge&) const = default;
};

enum class GraphDefectKind : std::uint8_t {
  self_loop,
  cycle,
  downgrade_incompatible,
  orphan_node,
  provenance_missing,
  capability_regression,
};

[[nodiscard]] const char* to_string(GraphDefectKind kind) noexcept;

struct GraphDefect {
  GraphDefectKind kind{GraphDefectKind::orphan_node};
  ComponentId component;
  Version from;
  Version to;
  CapabilityId capability;
  std::string explanation;

  auto operator<=>(const GraphDefect&) const = default;
};

enum class PathRefusalKind : std::uint8_t {
  source_unknown,                 // the starting version is not a node in the graph
  target_unknown,                 // the destination version is not a node in the graph
  no_path,                        // no sequence of edges reaches the destination
  missing_intermediate_capability,// a path exists on paper but a step needs a capability that is absent
  downgrade_incompatible,         // the only route would move a component backwards
  provenance_missing,             // every candidate route relies on an edge with no evidence
};

[[nodiscard]] const char* to_string(PathRefusalKind kind) noexcept;

struct PathRefusal {
  PathRefusalKind kind{PathRefusalKind::no_path};
  CapabilityId missing_capability;
  Version at_version;
  std::vector<ComponentVersion> frontier;
  std::string explanation;
};

struct CompatibilityPath {
  std::vector<CompatEdge> edges;
  CapabilitySet resulting_capabilities;
  Version target;
};

struct PathOutcome {
  std::optional<CompatibilityPath> path;
  std::optional<PathRefusal> refusal;

  [[nodiscard]] bool found() const noexcept { return path.has_value(); }
};

class CompatibilityGraph {
 public:
  [[nodiscard]] Status add_node(const ComponentVersion& node);
  [[nodiscard]] Status add_edge(const CompatEdge& edge);

  // Canonical order: nodes by (component, version), edges by
  // (component, from, to, kind). Digest and iteration order depend on it.
  void canonicalize();

  [[nodiscard]] bool has_node(const ComponentId& component, const Version& version) const;
  [[nodiscard]] const std::vector<ComponentVersion>& nodes() const noexcept { return nodes_; }
  [[nodiscard]] const std::vector<CompatEdge>& edges() const noexcept { return edges_; }
  [[nodiscard]] const CompatEdge* find_edge(const ComponentId& component, const Version& from,
                                            const Version& to) const;

  // Structural analysis: cycles between distinct versions of one component,
  // self loops, orphaned nodes, unsupported downgrades and edges that carry no
  // provenance.
  [[nodiscard]] std::vector<GraphDefect> analyse() const;

  // Deterministic breadth-first search in canonical edge order. The traversal
  // honours capability preconditions, so a route that would step through a
  // version whose prerequisites are not met is refused with the capability
  // that was missing and the frontier that was actually reachable, which is
  // what makes an impossible plan explainable.
  [[nodiscard]] Result<PathOutcome> find_path(const ComponentId& component, const Version& from,
                                              const Version& to,
                                              const CapabilitySet& initial_capabilities,
                                              TransitionKind kind, const EvidenceSet& evidence,
                                              std::size_t max_depth = kMaxGraphHops) const;

 private:
  std::vector<ComponentVersion> nodes_;
  std::vector<CompatEdge> edges_;
  bool canonical_{false};
};

}  // namespace dce
