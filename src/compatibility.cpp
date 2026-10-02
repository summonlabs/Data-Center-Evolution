#include "dce/compatibility.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>

#include "dce/text.hpp"

namespace dce {
namespace {

constexpr const char* kNoPathText =
    "no sequence of evidenced edges connects the source version to the destination version";

}  // namespace

const char* to_string(TransitionKind kind) noexcept {
  switch (kind) {
    case TransitionKind::upgrade: return "upgrade";
    case TransitionKind::downgrade: return "downgrade";
    case TransitionKind::reinstall: return "reinstall";
  }
  return "unknown";
}

const char* to_string(GraphDefectKind kind) noexcept {
  switch (kind) {
    case GraphDefectKind::self_loop: return "self_loop";
    case GraphDefectKind::cycle: return "cycle";
    case GraphDefectKind::downgrade_incompatible: return "downgrade_incompatible";
    case GraphDefectKind::orphan_node: return "orphan_node";
    case GraphDefectKind::provenance_missing: return "provenance_missing";
    case GraphDefectKind::capability_regression: return "capability_regression";
  }
  return "unknown";
}

const char* to_string(PathRefusalKind kind) noexcept {
  switch (kind) {
    case PathRefusalKind::source_unknown: return "source_unknown";
    case PathRefusalKind::target_unknown: return "target_unknown";
    case PathRefusalKind::no_path: return "no_path";
    case PathRefusalKind::missing_intermediate_capability: return "missing_intermediate_capability";
    case PathRefusalKind::downgrade_incompatible: return "downgrade_incompatible";
    case PathRefusalKind::provenance_missing: return "provenance_missing";
  }
  return "unknown";
}

Status CompatibilityGraph::add_node(const ComponentVersion& node) {
  if (canonical_) {
    return Error{ErrorCode::internal, "node added after the graph was canonicalised"};
  }
  for (const ComponentVersion& existing : nodes_) {
    if (existing == node) {
      return Status{};  // the same node declared twice is the same node
    }
  }
  if (nodes_.size() >= kMaxGraphNodes) {
    return Error{ErrorCode::limit_exceeded, "compatibility graph exceeds its node bound"};
  }
  nodes_.push_back(node);
  return Status{};
}

Status CompatibilityGraph::add_edge(const CompatEdge& edge) {
  if (canonical_) {
    return Error{ErrorCode::internal, "edge added after the graph was canonicalised"};
  }
  if (edges_.size() >= kMaxGraphEdges) {
    return Error{ErrorCode::limit_exceeded, "compatibility graph exceeds its edge bound"};
  }
  for (const CompatEdge& existing : edges_) {
    if (existing.component == edge.component && existing.from == edge.from &&
        existing.to == edge.to && existing.kind == edge.kind) {
      if (existing == edge) {
        return Status{};
      }
      return Error{ErrorCode::conflict,
                   "two different compatibility edges claim the same transition"};
    }
  }
  edges_.push_back(edge);
  return Status{};
}

void CompatibilityGraph::canonicalize() {
  std::sort(nodes_.begin(), nodes_.end());
  std::sort(edges_.begin(), edges_.end(),
            [](const CompatEdge& left, const CompatEdge& right) {
              if (left.component != right.component) {
                return left.component < right.component;
              }
              if (left.from != right.from) {
                return left.from < right.from;
              }
              if (left.to != right.to) {
                return left.to < right.to;
              }
              return static_cast<int>(left.kind) < static_cast<int>(right.kind);
            });
  canonical_ = true;
}

bool CompatibilityGraph::has_node(const ComponentId& component, const Version& version) const {
  return std::binary_search(nodes_.begin(), nodes_.end(), ComponentVersion{component, version});
}

const CompatEdge* CompatibilityGraph::find_edge(const ComponentId& component, const Version& from,
                                                const Version& to) const {
  for (const CompatEdge& edge : edges_) {
    if (edge.component == component && edge.from == from && edge.to == to) {
      return &edge;
    }
  }
  return nullptr;
}

namespace {

// Index of a node in the canonical node vector, or nothing when absent.
std::optional<std::size_t> node_index(const std::vector<ComponentVersion>& nodes,
                                      const ComponentId& component, const Version& version) {
  const auto position =
      std::lower_bound(nodes.begin(), nodes.end(), ComponentVersion{component, version});
  if (position == nodes.end() || !(*position == ComponentVersion{component, version})) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(position - nodes.begin());
}

}  // namespace

std::vector<GraphDefect> CompatibilityGraph::analyse() const {
  std::vector<GraphDefect> defects;

  for (const CompatEdge& edge : edges_) {
    if (edge.from == edge.to) {
      defects.push_back(GraphDefect{GraphDefectKind::self_loop, edge.component, edge.from, edge.to, {},
                                    "an edge that does not change the version is not a transition"});
    }
    if (!edge.provenance.valid()) {
      defects.push_back(GraphDefect{GraphDefectKind::provenance_missing, edge.component, edge.from,
                                    edge.to, {},
                                    "the edge carries no evidence identity, so it is a claim "
                                    "without provenance"});
    }
    if (edge.kind == TransitionKind::downgrade && !edge.reversible) {
      defects.push_back(GraphDefect{GraphDefectKind::downgrade_incompatible, edge.component,
                                    edge.from, edge.to, {},
                                    "a downgrade edge that is not marked reversible cannot be "
                                    "used to recover a failed rollout"});
    }
  }

  // A capability regression is an edge that lands on a version from which a
  // further transition expects a capability this edge never established.
  for (const CompatEdge& arrival : edges_) {
    for (const CompatEdge& departure : edges_) {
      if (departure.component != arrival.component || !(departure.from == arrival.to)) {
        continue;
      }
      const std::vector<CapabilityId> missing =
          departure.requires_capabilities.missing_from(arrival.provides_capabilities);
      if (!missing.empty()) {
        defects.push_back(GraphDefect{GraphDefectKind::capability_regression, arrival.component,
                                      arrival.from, arrival.to, missing.front(),
                                      "arriving at this version does not establish a capability "
                                      "the next transition requires"});
      }
    }
  }

  // Nodes that no edge touches cannot participate in any evolution.
  for (const ComponentVersion& node : nodes_) {
    bool touched = false;
    for (const CompatEdge& edge : edges_) {
      if (edge.component != node.component) {
        continue;
      }
      if (edge.from == node.version || edge.to == node.version) {
        touched = true;
        break;
      }
    }
    if (!touched) {
      defects.push_back(GraphDefect{GraphDefectKind::orphan_node, node.component, node.version, {},
                                    {}, "no compatibility edge refers to this component version"});
    }
  }

  // Cycles between distinct versions of one component are reported once per
  // component, at the lowest version that participates in one.
  std::vector<ComponentId> components;
  for (const ComponentVersion& node : nodes_) {
    if (std::find(components.begin(), components.end(), node.component) == components.end()) {
      components.push_back(node.component);
    }
  }
  for (const ComponentId& component : components) {
    std::vector<Version> versions;
    for (const ComponentVersion& node : nodes_) {
      if (node.component == component) {
        versions.push_back(node.version);
      }
    }
    std::sort(versions.begin(), versions.end());
    versions.erase(std::unique(versions.begin(), versions.end()), versions.end());

    std::vector<std::vector<std::size_t>> adjacency(versions.size());
    for (const CompatEdge& edge : edges_) {
      if (edge.component != component) {
        continue;
      }
      const auto from =
          std::lower_bound(versions.begin(), versions.end(), edge.from);
      const auto to = std::lower_bound(versions.begin(), versions.end(), edge.to);
      if (from == versions.end() || to == versions.end()) {
        continue;
      }
      const std::size_t from_index = static_cast<std::size_t>(from - versions.begin());
      const std::size_t to_index = static_cast<std::size_t>(to - versions.begin());
      adjacency[from_index].push_back(to_index);
    }
    for (std::vector<std::size_t>& list : adjacency) {
      std::sort(list.begin(), list.end());
      list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    // Iterative depth-first search with the three-colour scheme.
    std::vector<int> colour(versions.size(), 0);
    bool found = false;
    Version cycle_start;
    for (std::size_t root = 0; root < versions.size() && !found; ++root) {
      if (colour[root] != 0) {
        continue;
      }
      std::vector<std::pair<std::size_t, std::size_t>> stack;  // node, next child
      stack.emplace_back(root, 0);
      colour[root] = 1;
      while (!stack.empty() && !found) {
        auto& frame = stack.back();
        if (frame.second >= adjacency[frame.first].size()) {
          colour[frame.first] = 2;
          stack.pop_back();
          continue;
        }
        const std::size_t child = adjacency[frame.first][frame.second];
        ++frame.second;
        if (colour[child] == 1) {
          found = true;
          cycle_start = versions[child];
          break;
        }
        if (colour[child] == 0) {
          colour[child] = 1;
          stack.emplace_back(child, 0);
        }
      }
    }
    if (found) {
      defects.push_back(GraphDefect{GraphDefectKind::cycle, component, cycle_start, cycle_start, {},
                                    "this component's transitions contain a cycle, so the graph "
                                    "cannot order a rollout by version alone"});
    }
  }

  std::sort(defects.begin(), defects.end());
  return defects;
}

Result<PathOutcome> CompatibilityGraph::find_path(const ComponentId& component, const Version& from,
                                                  const Version& to,
                                                  const CapabilitySet& initial_capabilities,
                                                  TransitionKind kind, const EvidenceSet& evidence,
                                                  std::size_t max_depth) const {
  PathOutcome outcome;

  const std::optional<std::size_t> start = node_index(nodes_, component, from);
  if (!start.has_value()) {
    outcome.refusal = PathRefusal{PathRefusalKind::source_unknown, {}, from, {},
                                  "the source version is not a node in the compatibility graph"};
    return outcome;
  }
  const std::optional<std::size_t> goal = node_index(nodes_, component, to);
  if (!goal.has_value()) {
    outcome.refusal = PathRefusal{PathRefusalKind::target_unknown, {}, to, {},
                                  "the destination version is not a node in the compatibility graph"};
    return outcome;
  }

  if (from == to) {
    CompatibilityPath path;
    path.resulting_capabilities = initial_capabilities;
    path.target = to;
    outcome.path = path;
    return outcome;
  }

  if (kind == TransitionKind::upgrade && to < from) {
    outcome.refusal = PathRefusal{PathRefusalKind::downgrade_incompatible, {}, from, {},
                                  "an upgrade transition cannot move a component backwards"};
    return outcome;
  }
  if (kind == TransitionKind::downgrade && from < to) {
    outcome.refusal = PathRefusal{PathRefusalKind::downgrade_incompatible, {}, from, {},
                                  "a downgrade transition cannot move a component forwards"};
    return outcome;
  }
  if (kind == TransitionKind::reinstall) {
    outcome.refusal = PathRefusal{PathRefusalKind::no_path, {}, from, {},
                                  "a reinstall changes no version, so it has no path to a "
                                  "different version"};
    return outcome;
  }

  // Outgoing edge index per node, in canonical edge order, so that the search
  // is a deterministic function of the graph alone.
  std::vector<std::vector<std::size_t>> outgoing(nodes_.size());
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    const CompatEdge& edge = edges_[index];
    if (edge.component != component || edge.kind != kind) {
      continue;
    }
    const std::optional<std::size_t> source = node_index(nodes_, component, edge.from);
    if (!source.has_value()) {
      continue;
    }
    outgoing[*source].push_back(index);
  }

  struct Item {
    std::size_t node;
    CapabilitySet capabilities;
    std::size_t depth;
  };

  std::vector<std::optional<CapabilitySet>> best(nodes_.size());
  std::vector<std::optional<CompatEdge>> entry_edge(nodes_.size());
  std::deque<Item> queue;
  best[*start] = initial_capabilities;
  queue.push_back(Item{*start, initial_capabilities, 0});

  std::optional<PathRefusal> capability_block;
  std::optional<PathRefusal> provenance_block;
  std::size_t budget = kMaxGraphNodes * kMaxGraphHops;

  while (!queue.empty() && budget > 0) {
    Item item = std::move(queue.front());
    queue.pop_front();
    if (item.depth >= max_depth) {
      continue;
    }
    for (const std::size_t edge_index : outgoing[item.node]) {
      if (budget == 0) {
        break;
      }
      --budget;
      const CompatEdge& edge = edges_[edge_index];
      const std::optional<std::size_t> target = node_index(nodes_, component, edge.to);
      if (!target.has_value()) {
        continue;
      }
      if (!edge.provenance.valid() || evidence.find(edge.provenance) == nullptr) {
        if (!provenance_block.has_value()) {
          PathRefusal refusal;
          refusal.kind = PathRefusalKind::provenance_missing;
          refusal.at_version = edge.from;
          refusal.explanation =
              "the transition from this version has no evidence in the plan, so it is not a "
              "permission";
          provenance_block = refusal;
        }
        continue;
      }
      const std::vector<CapabilityId> missing =
          edge.requires_capabilities.missing_from(item.capabilities);
      if (!missing.empty()) {
        if (!capability_block.has_value()) {
          PathRefusal refusal;
          refusal.kind = PathRefusalKind::missing_intermediate_capability;
          refusal.missing_capability = missing.front();
          refusal.at_version = edge.from;
          refusal.explanation =
              "the transition from this version requires a capability the path has not "
              "established";
          capability_block = refusal;
        }
        continue;
      }

      CapabilitySet accumulated = item.capabilities;
      for (const CapabilityId& capability : edge.provides_capabilities) {
        DCE_TRY(accumulated.insert(capability));
      }

      std::optional<CapabilitySet>& slot = best[*target];
      if (slot.has_value() && accumulated.missing_from(*slot).empty()) {
        continue;  // nothing new is reachable through this node
      }
      if (!slot.has_value()) {
        slot = accumulated;
      } else {
        for (const CapabilityId& capability : accumulated) {
          DCE_TRY(slot->insert(capability));
        }
      }
      entry_edge[*target] = edge;
      queue.push_back(Item{*target, accumulated, item.depth + 1});
    }
  }

  if (best[*goal].has_value()) {
    std::vector<CompatEdge> reversed;
    std::size_t cursor = *goal;
    std::size_t guard = 0;
    while (cursor != *start && guard < nodes_.size()) {
      if (!entry_edge[cursor].has_value()) {
        break;
      }
      const CompatEdge& edge = *entry_edge[cursor];
      reversed.push_back(edge);
      const std::optional<std::size_t> previous = node_index(nodes_, component, edge.from);
      if (!previous.has_value()) {
        break;
      }
      cursor = *previous;
      ++guard;
    }
    if (cursor == *start) {
      CompatibilityPath path;
      path.edges.assign(reversed.rbegin(), reversed.rend());
      path.target = to;
      path.resulting_capabilities = initial_capabilities;
      for (const CompatEdge& edge : path.edges) {
        for (const CapabilityId& capability : edge.provides_capabilities) {
          DCE_TRY(path.resulting_capabilities.insert(capability));
        }
      }
      outcome.path = std::move(path);
      return outcome;
    }
    outcome.refusal = PathRefusal{PathRefusalKind::no_path, {}, from, {}, kNoPathText};
    return outcome;
  }

  std::vector<ComponentVersion> frontier;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    if (best[index].has_value()) {
      frontier.push_back(nodes_[index]);
    }
  }

  if (capability_block.has_value()) {
    capability_block->frontier = std::move(frontier);
    outcome.refusal = capability_block;
    return outcome;
  }
  if (provenance_block.has_value()) {
    provenance_block->frontier = std::move(frontier);
    outcome.refusal = provenance_block;
    return outcome;
  }
  outcome.refusal = PathRefusal{PathRefusalKind::no_path, {}, from, std::move(frontier), kNoPathText};
  return outcome;
}

}  // namespace dce
