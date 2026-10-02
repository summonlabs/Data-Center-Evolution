// The compatibility graph: structure, defects, and the path search that decides
// whether a component can actually move from one version to another.
#include <string>
#include <vector>

#include "dce/compatibility.hpp"
#include "harness.hpp"

namespace {

dce::ComponentId comp(const std::string& name) { return *dce::ComponentId::parse(name); }
dce::CapabilityId cap(const std::string& name) { return *dce::CapabilityId::parse(name); }
dce::EvidenceId ev(const std::string& name) { return *dce::EvidenceId::parse(name); }

dce::CompatEdge edge(const std::string& component, dce::Version from, dce::Version to,
                     const std::vector<std::string>& required,
                     const std::vector<std::string>& provides, const std::string& provenance) {
  dce::CompatEdge value;
  value.component = comp(component);
  value.from = from;
  value.to = to;
  value.kind = dce::TransitionKind::upgrade;
  for (const std::string& name : required) {
    DCE_CHECK_OK(value.requires_capabilities.insert(cap(name)));
  }
  for (const std::string& name : provides) {
    DCE_CHECK_OK(value.provides_capabilities.insert(cap(name)));
  }
  if (!provenance.empty()) {
    value.provenance = ev(provenance);
  }
  value.reversible = true;
  value.justification = "test edge";
  return value;
}

dce::EvidenceSet evidence_with(const std::vector<std::string>& ids) {
  dce::EvidenceSet set;
  for (const std::string& id : ids) {
    dce::EvidenceRecord record;
    record.id = ev(id);
    record.kind = dce::EvidenceKind::compatibility_certification;
    record.claim = "certified";
    record.producer = *dce::ActorId::parse("author");
    DCE_CHECK_OK(set.add(std::move(record)));
  }
  return set;
}

// v0 -> v1 -> v2, each upgrade requiring the capability the previous version
// established.
dce::CompatibilityGraph chain() {
  dce::CompatibilityGraph graph;
  const dce::Version v0{1, 0, 0};
  const dce::Version v1{1, 1, 0};
  const dce::Version v2{2, 0, 0};
  DCE_CHECK_OK(graph.add_node(dce::ComponentVersion{comp("dccp.power"), v0}));
  DCE_CHECK_OK(graph.add_node(dce::ComponentVersion{comp("dccp.power"), v1}));
  DCE_CHECK_OK(graph.add_node(dce::ComponentVersion{comp("dccp.power"), v2}));
  DCE_CHECK_OK(graph.add_edge(edge("dccp.power", v0, v1, {}, {"cap.power.1"}, "ev.0")));
  DCE_CHECK_OK(graph.add_edge(edge("dccp.power", v1, v2, {"cap.power.1"}, {"cap.power.2"}, "ev.1")));
  graph.canonicalize();
  return graph;
}

dce::CapabilitySet starting_capabilities() {
  dce::CapabilitySet set;
  DCE_CHECK_OK(set.insert(cap("cap.power.0")));
  return set;
}

}  // namespace

DCE_TEST(compatibility, nodes_and_edges_are_idempotent_and_bounded_in_identity) {
  dce::CompatibilityGraph graph;
  const dce::Version v0{1, 0, 0};
  const dce::Version v1{1, 1, 0};
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v0}));
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v0}));
  DCE_CHECK_EQ(graph.nodes().size(), static_cast<std::size_t>(1));

  DCE_REQUIRE_OK(graph.add_edge(edge("c", v0, v1, {}, {"cap.1"}, "ev.0")));
  DCE_REQUIRE_OK(graph.add_edge(edge("c", v0, v1, {}, {"cap.1"}, "ev.0")));
  DCE_CHECK_EQ(graph.edges().size(), static_cast<std::size_t>(1));

  dce::CompatEdge different = edge("c", v0, v1, {}, {"cap.other"}, "ev.0");
  DCE_CHECK_EQ(graph.add_edge(different).code(), dce::ErrorCode::conflict);
}

DCE_TEST(compatibility, a_clean_graph_reports_no_defects) {
  const dce::CompatibilityGraph graph = chain();
  const std::vector<dce::GraphDefect> defects = graph.analyse();
  DCE_CHECK_EQ(defects.size(), static_cast<std::size_t>(0));
  for (const dce::GraphDefect& defect : defects) {
    DCE_FAIL(std::string("unexpected defect ") + dce::to_string(defect.kind) + ": " +
             defect.explanation);
  }
}

DCE_TEST(compatibility, analysis_finds_every_structural_defect) {
  dce::CompatibilityGraph graph;
  const dce::Version v0{1, 0, 0};
  const dce::Version v1{1, 1, 0};
  const dce::Version orphan{9, 0, 0};
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v0}));
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v1}));
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), orphan}));
  DCE_REQUIRE_OK(graph.add_edge(edge("c", v0, v0, {}, {}, "ev.self")));
  DCE_REQUIRE_OK(graph.add_edge(edge("c", v0, v1, {}, {}, "ev.0")));
  DCE_REQUIRE_OK(graph.add_edge(edge("c", v1, v0, {}, {}, "")));
  dce::CompatEdge downgrade = edge("c", v1, v0, {}, {}, "ev.1");
  downgrade.kind = dce::TransitionKind::downgrade;
  downgrade.reversible = false;
  DCE_REQUIRE_OK(graph.add_edge(downgrade));
  graph.canonicalize();

  const std::vector<dce::GraphDefect> defects = graph.analyse();
  bool self_loop = false;
  bool cycle = false;
  bool orphan_found = false;
  bool provenance_missing = false;
  bool downgrade_incompatible = false;
  for (const dce::GraphDefect& defect : defects) {
    switch (defect.kind) {
      case dce::GraphDefectKind::self_loop: self_loop = true; break;
      case dce::GraphDefectKind::cycle: cycle = true; break;
      case dce::GraphDefectKind::orphan_node: orphan_found = true; break;
      case dce::GraphDefectKind::provenance_missing: provenance_missing = true; break;
      case dce::GraphDefectKind::downgrade_incompatible: downgrade_incompatible = true; break;
      case dce::GraphDefectKind::capability_regression: break;
    }
    DCE_CHECK_TRUE(!defect.explanation.empty());
  }
  DCE_CHECK_TRUE(self_loop);
  DCE_CHECK_TRUE(cycle);
  DCE_CHECK_TRUE(orphan_found);
  DCE_CHECK_TRUE(provenance_missing);
  DCE_CHECK_TRUE(downgrade_incompatible);
}

DCE_TEST(compatibility, a_capability_regression_is_reported) {
  dce::CompatibilityGraph graph;
  const dce::Version v0{1, 0, 0};
  const dce::Version v1{1, 1, 0};
  const dce::Version v2{2, 0, 0};
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v0}));
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v1}));
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v2}));
  // Arriving at v1 establishes nothing, but leaving v1 requires something.
  DCE_REQUIRE_OK(graph.add_edge(edge("c", v0, v1, {}, {}, "ev.0")));
  DCE_REQUIRE_OK(graph.add_edge(edge("c", v1, v2, {"cap.needed"}, {"cap.2"}, "ev.1")));
  graph.canonicalize();
  bool regression = false;
  for (const dce::GraphDefect& defect : graph.analyse()) {
    if (defect.kind == dce::GraphDefectKind::capability_regression) {
      regression = true;
      DCE_CHECK_TRUE(defect.capability == cap("cap.needed"));
    }
  }
  DCE_CHECK_TRUE(regression);
}

DCE_TEST(compatibility, a_multi_hop_path_accumulates_capabilities) {
  const dce::CompatibilityGraph graph = chain();
  const dce::EvidenceSet evidence = evidence_with({"ev.0", "ev.1"});
  const dce::Result<dce::PathOutcome> outcome =
      graph.find_path(comp("dccp.power"), dce::Version{1, 0, 0}, dce::Version{2, 0, 0},
                      starting_capabilities(), dce::TransitionKind::upgrade, evidence);
  DCE_REQUIRE_OK(outcome);
  DCE_CHECK_TRUE(outcome->found());
  DCE_REQUIRE(outcome->path.has_value());
  DCE_CHECK_EQ(outcome->path->edges.size(), static_cast<std::size_t>(2));
  DCE_CHECK_TRUE(outcome->path->resulting_capabilities.contains(cap("cap.power.2")));
  DCE_CHECK_TRUE((outcome->path->edges.front().from == dce::Version{1, 0, 0}));
  DCE_CHECK_TRUE((outcome->path->edges.back().to == dce::Version{2, 0, 0}));
}

DCE_TEST(compatibility, the_same_version_needs_no_path) {
  const dce::CompatibilityGraph graph = chain();
  const dce::EvidenceSet evidence = evidence_with({"ev.0", "ev.1"});
  const dce::Result<dce::PathOutcome> outcome =
      graph.find_path(comp("dccp.power"), dce::Version{1, 1, 0}, dce::Version{1, 1, 0}, {},
                      dce::TransitionKind::upgrade, evidence);
  DCE_REQUIRE_OK(outcome);
  DCE_CHECK_TRUE(outcome->found());
  DCE_REQUIRE(outcome->path.has_value());
  DCE_CHECK_EQ(outcome->path->edges.size(), static_cast<std::size_t>(0));
}

DCE_TEST(compatibility, refusals_name_the_problem) {
  const dce::CompatibilityGraph graph = chain();
  const dce::EvidenceSet evidence = evidence_with({"ev.0", "ev.1"});

  {
    const dce::Result<dce::PathOutcome> outcome =
        graph.find_path(comp("dccp.power"), dce::Version{5, 0, 0}, dce::Version{2, 0, 0}, {},
                        dce::TransitionKind::upgrade, evidence);
    DCE_REQUIRE_OK(outcome);
    DCE_CHECK_TRUE(!outcome->found());
    DCE_REQUIRE(outcome->refusal.has_value());
    DCE_CHECK_EQ(outcome->refusal->kind, dce::PathRefusalKind::source_unknown);
  }
  {
    const dce::Result<dce::PathOutcome> outcome =
        graph.find_path(comp("dccp.power"), dce::Version{1, 0, 0}, dce::Version{5, 0, 0}, {},
                        dce::TransitionKind::upgrade, evidence);
    DCE_REQUIRE_OK(outcome);
    DCE_CHECK_TRUE(!outcome->found());
    DCE_REQUIRE(outcome->refusal.has_value());
    DCE_CHECK_EQ(outcome->refusal->kind, dce::PathRefusalKind::target_unknown);
  }
  {
    // The only route to the target needs a capability that nothing on the path
    // establishes, so the refusal must name that capability and the version it
    // was needed at.
    dce::CompatibilityGraph gated;
    const dce::Version v0{1, 0, 0};
    const dce::Version v1{1, 1, 0};
    const dce::Version v2{2, 0, 0};
    DCE_REQUIRE_OK(gated.add_node(dce::ComponentVersion{comp("dccp.power"), v0}));
    DCE_REQUIRE_OK(gated.add_node(dce::ComponentVersion{comp("dccp.power"), v1}));
    DCE_REQUIRE_OK(gated.add_node(dce::ComponentVersion{comp("dccp.power"), v2}));
    DCE_REQUIRE_OK(gated.add_edge(edge("dccp.power", v0, v1, {"cap.gate"}, {"cap.power.1"}, "ev.0")));
    DCE_REQUIRE_OK(gated.add_edge(edge("dccp.power", v1, v2, {"cap.power.1"}, {"cap.power.2"}, "ev.1")));
    gated.canonicalize();
    const dce::Result<dce::PathOutcome> outcome =
        gated.find_path(comp("dccp.power"), v0, v2, starting_capabilities(),
                        dce::TransitionKind::upgrade, evidence);
    DCE_REQUIRE_OK(outcome);
    DCE_CHECK_TRUE(!outcome->found());
    DCE_REQUIRE(outcome->refusal.has_value());
    DCE_CHECK_EQ(outcome->refusal->kind, dce::PathRefusalKind::missing_intermediate_capability);
    DCE_CHECK_TRUE(outcome->refusal->missing_capability == cap("cap.gate"));
    DCE_CHECK_TRUE((outcome->refusal->at_version == v0));
    DCE_CHECK_TRUE(!outcome->refusal->frontier.empty());
    DCE_CHECK_TRUE(!outcome->refusal->explanation.empty());
  }
  {
    // No evidence is held for the edges, so the only route has no provenance.
    const dce::EvidenceSet empty;
    const dce::Result<dce::PathOutcome> outcome =
        graph.find_path(comp("dccp.power"), dce::Version{1, 0, 0}, dce::Version{2, 0, 0},
                        starting_capabilities(), dce::TransitionKind::upgrade, empty);
    DCE_REQUIRE_OK(outcome);
    DCE_CHECK_TRUE(!outcome->found());
    DCE_REQUIRE(outcome->refusal.has_value());
    DCE_CHECK_EQ(outcome->refusal->kind, dce::PathRefusalKind::provenance_missing);
  }
  {
    // An upgrade that would move backwards is refused before any search.
    const dce::Result<dce::PathOutcome> outcome =
        graph.find_path(comp("dccp.power"), dce::Version{2, 0, 0}, dce::Version{1, 0, 0},
                        starting_capabilities(), dce::TransitionKind::upgrade, evidence);
    DCE_REQUIRE_OK(outcome);
    DCE_CHECK_TRUE(!outcome->found());
    DCE_REQUIRE(outcome->refusal.has_value());
    DCE_CHECK_EQ(outcome->refusal->kind, dce::PathRefusalKind::downgrade_incompatible);
  }
  {
    // A downgrade that would move forwards is refused for the same reason.
    const dce::Result<dce::PathOutcome> outcome =
        graph.find_path(comp("dccp.power"), dce::Version{1, 0, 0}, dce::Version{2, 0, 0},
                        starting_capabilities(), dce::TransitionKind::downgrade, evidence);
    DCE_REQUIRE_OK(outcome);
    DCE_CHECK_TRUE(!outcome->found());
    DCE_REQUIRE(outcome->refusal.has_value());
    DCE_CHECK_EQ(outcome->refusal->kind, dce::PathRefusalKind::downgrade_incompatible);
  }
}

DCE_TEST(compatibility, an_unreachable_target_reports_the_frontier_it_did_reach) {
  dce::CompatibilityGraph graph;
  const dce::Version v0{1, 0, 0};
  const dce::Version v1{1, 1, 0};
  const dce::Version island{5, 0, 0};
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v0}));
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), v1}));
  DCE_REQUIRE_OK(graph.add_node(dce::ComponentVersion{comp("c"), island}));
  DCE_REQUIRE_OK(graph.add_edge(edge("c", v0, v1, {}, {}, "ev.0")));
  graph.canonicalize();
  const dce::EvidenceSet evidence = evidence_with({"ev.0"});
  const dce::Result<dce::PathOutcome> outcome =
      graph.find_path(comp("c"), v0, island, {}, dce::TransitionKind::upgrade, evidence);
  DCE_REQUIRE_OK(outcome);
  DCE_CHECK_TRUE(!outcome->found());
  DCE_REQUIRE(outcome->refusal.has_value());
  DCE_CHECK_EQ(outcome->refusal->kind, dce::PathRefusalKind::no_path);
  // The frontier explains what was actually reachable.
  DCE_CHECK_TRUE(outcome->refusal->frontier.size() >= 2);
}

DCE_TEST(compatibility, the_search_is_deterministic_under_insertion_order) {
  dce::CompatibilityGraph first = chain();
  dce::CompatibilityGraph second;
  const dce::Version v0{1, 0, 0};
  const dce::Version v1{1, 1, 0};
  const dce::Version v2{2, 0, 0};
  // Deliberately the reverse insertion order.
  DCE_REQUIRE_OK(second.add_node(dce::ComponentVersion{comp("dccp.power"), v2}));
  DCE_REQUIRE_OK(second.add_edge(edge("dccp.power", v1, v2, {"cap.power.1"}, {"cap.power.2"}, "ev.1")));
  DCE_REQUIRE_OK(second.add_node(dce::ComponentVersion{comp("dccp.power"), v0}));
  DCE_REQUIRE_OK(second.add_edge(edge("dccp.power", v0, v1, {}, {"cap.power.1"}, "ev.0")));
  DCE_REQUIRE_OK(second.add_node(dce::ComponentVersion{comp("dccp.power"), v1}));
  second.canonicalize();

  const dce::EvidenceSet evidence = evidence_with({"ev.0", "ev.1"});
  const dce::Result<dce::PathOutcome> a =
      first.find_path(comp("dccp.power"), v0, v2, starting_capabilities(),
                      dce::TransitionKind::upgrade, evidence);
  const dce::Result<dce::PathOutcome> b =
      second.find_path(comp("dccp.power"), v0, v2, starting_capabilities(),
                       dce::TransitionKind::upgrade, evidence);
  DCE_REQUIRE_OK(a);
  DCE_REQUIRE_OK(b);
  DCE_CHECK_TRUE(a->found());
  DCE_CHECK_TRUE(b->found());
  DCE_REQUIRE(a->path.has_value());
  DCE_REQUIRE(b->path.has_value());
  DCE_CHECK_EQ(a->path->edges.size(), b->path->edges.size());
  for (std::size_t index = 0; index < a->path->edges.size(); ++index) {
    DCE_CHECK_TRUE(a->path->edges[index].from == b->path->edges[index].from);
    DCE_CHECK_TRUE(a->path->edges[index].to == b->path->edges[index].to);
  }
}
