// A read-only plan inspector.
//
// Usage: inspect_plan <plan-file>
//
// The file is exactly the canonical encoding of an EvolutionPlan: the bytes a
// CanonicalWriter holds after codec::put(writer, plan), which is the payload a
// coordinator commits under RecordType::plan_revision and the payload
// dce::codec::get<EvolutionPlan> decodes.
//
// The codec deliberately does not store EvolutionPlan::digest: it is derived
// from the content so that a stored digest can never disagree with the bytes it
// claims to summarise. This tool therefore recomputes the digest with
// dce::compute_plan_digest and compares it against the SHA-256 of the bytes on
// disk. The two agree exactly when the file is the canonical encoding of the
// plan it decodes to, and differ when the file was truncated, edited, or
// written in a non-canonical order: in that case the tool reports the mismatch
// and exits non-zero instead of printing a listing that looks authoritative.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dce/canonical.hpp"
#include "dce/codec.hpp"
#include "dce/digest.hpp"
#include "dce/evidence.hpp"
#include "dce/limits.hpp"
#include "dce/plan.hpp"
#include "dce/platform.hpp"
#include "dce/status.hpp"
#include "dce/text.hpp"
#include "dce/version.hpp"

namespace {

[[nodiscard]] int fail(const std::string& message) {
  std::cerr << "inspect_plan: " << message << '\n';
  return EXIT_FAILURE;
}

[[nodiscard]] std::string show(std::string_view value) {
  return dce::text::escape_for_output(value);
}

[[nodiscard]] std::string number(std::uint64_t value) { return dce::text::u64_to_string(value); }

// Reads the whole file, bounded by the largest canonical payload the codec
// accepts, and reports every operating-system level failure explicitly.
[[nodiscard]] dce::Result<std::vector<std::byte>> read_whole_file(const std::string& path) {
  auto opened = dce::platform::File::open(path, dce::platform::OpenMode::read_only);
  if (!opened.ok()) {
    return opened.error();
  }
  dce::platform::File file = std::move(opened).value();
  auto size = file.size();
  if (!size.ok()) {
    return size.error();
  }
  if (size.value() > static_cast<std::uint64_t>(dce::kMaxCanonicalBytes)) {
    return dce::Error{dce::ErrorCode::limit_exceeded,
                      "the file is larger than any canonical plan could be"};
  }
  const std::size_t total = static_cast<std::size_t>(size.value());
  std::vector<std::byte> bytes(total);
  std::size_t filled = 0;
  while (filled < total) {
    auto read = file.read_at(static_cast<std::uint64_t>(filled),
                             std::span<std::byte>(bytes.data() + filled, total - filled));
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return dce::Error{dce::ErrorCode::malformed, "the file ended before its recorded length"};
    }
    filled += read.value();
  }
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: inspect_plan <plan-file>\n";
    return EXIT_FAILURE;
  }
  const std::string path = argv[1];

  auto bytes_result = read_whole_file(path);
  if (!bytes_result.ok()) {
    return fail("cannot read '" + path + "': " + dce::to_string(bytes_result.code()) + ": " +
                bytes_result.message());
  }
  const std::vector<std::byte>& bytes = bytes_result.value();
  const std::span<const std::byte> stored_bytes(bytes.data(), bytes.size());

  dce::CanonicalReader reader(stored_bytes);
  auto plan_result = dce::codec::get<dce::EvolutionPlan>(reader);
  if (!plan_result.ok()) {
    return fail("cannot decode '" + path + "': " + dce::to_string(plan_result.code()) + ": " +
                plan_result.message());
  }
  if (!reader.at_end()) {
    return fail("'" + path + "' carries " + number(static_cast<std::uint64_t>(reader.remaining())) +
                " trailing byte(s) after the plan payload");
  }
  const dce::EvolutionPlan& plan = plan_result.value();

  auto digest_result = dce::compute_plan_digest(plan);
  if (!digest_result.ok()) {
    return fail(std::string("cannot digest the decoded plan: ") +
                dce::to_string(digest_result.code()) + ": " + digest_result.message());
  }
  const dce::Digest256& digest = digest_result.value();
  const dce::Digest256 stored = dce::Digest256::of(stored_bytes);
  const bool canonical = digest == stored;

  std::cout << "plan:        " << show(plan.identity.id.view()) << '\n';
  std::cout << "title:       " << show(plan.title) << '\n';
  std::cout << "revision:    " << number(plan.identity.revision.value()) << '\n';
  std::cout << "generation:  " << number(plan.identity.generation.value()) << '\n';
  std::cout << "epoch:       " << number(plan.identity.epoch.value()) << '\n';
  std::cout << "source:      " << plan.source_version.to_string() << '\n';
  std::cout << "target:      " << plan.target_version.to_string() << '\n';
  std::cout << "digest:      " << digest.to_hex() << '\n';
  std::cout << "stored:      " << stored.to_hex() << '\n';
  std::cout << "canonical:   " << (canonical ? "match" : "MISMATCH") << '\n';

  std::cout << "sites:       " << number(static_cast<std::uint64_t>(plan.membership.sites.size()))
            << '\n';
  for (const dce::SiteRecord& site : plan.membership.sites) {
    std::cout << "  " << show(site.id.view()) << " dccp=" << site.dccp_version.to_string();
    for (const dce::ComponentVersion& component : site.components) {
      std::cout << ' ' << show(component.component.view()) << '=' << component.version.to_string();
    }
    std::cout << '\n';
  }

  std::cout << "steps:       " << number(static_cast<std::uint64_t>(plan.steps.size())) << '\n';
  for (const dce::MigrationStep& step : plan.steps) {
    std::cout << "  " << show(step.id.view()) << " " << show(step.component.view()) << " "
              << step.from.to_string() << " -> " << step.to.to_string() << " "
              << dce::to_string(step.kind) << '\n';
  }

  std::cout << "cohorts:     " << number(static_cast<std::uint64_t>(plan.cohorts.size())) << '\n';
  for (const dce::RolloutCohort& cohort : plan.cohorts) {
    std::cout << "  " << show(cohort.id.view())
              << " wave=" << dce::text::u32_to_string(cohort.wave.value())
              << " strategy=" << dce::to_string(cohort.strategy)
              << " max_parallel=" << dce::text::u32_to_string(cohort.max_parallel)
              << " sites=" << number(static_cast<std::uint64_t>(cohort.sites.size())) << ':';
    for (const dce::SiteId& member : cohort.sites) {
      std::cout << ' ' << show(member.view());
    }
    std::cout << '\n';
  }

  std::cout << "gates:       " << number(static_cast<std::uint64_t>(plan.gates.size())) << '\n';
  for (const dce::Gate& gate : plan.gates.entries()) {
    std::cout << "  " << show(gate.id.view()) << " kind=" << dce::to_string(gate.kind)
              << " scope=" << dce::to_string(gate.scope)
              << " blocking=" << (gate.blocking ? "yes" : "no") << '\n';
  }

  std::cout << "evidence:    " << number(static_cast<std::uint64_t>(plan.evidence.size())) << '\n';
  constexpr std::array<dce::EvidenceKind, 10> kEvidenceKinds = {
      dce::EvidenceKind::compatibility_certification, dce::EvidenceKind::migration_drill,
      dce::EvidenceKind::rollback_drill,              dce::EvidenceKind::readiness_gate,
      dce::EvidenceKind::health_observation,          dce::EvidenceKind::dependency_attestation,
      dce::EvidenceKind::capability_dependency_proof, dce::EvidenceKind::authority_grant,
      dce::EvidenceKind::exception_grant,             dce::EvidenceKind::provenance_statement};
  for (const dce::EvidenceKind kind : kEvidenceKinds) {
    const std::size_t found = plan.evidence.of_kind(kind).size();
    if (found != 0) {
      std::cout << "  " << dce::to_string(kind) << " "
                << number(static_cast<std::uint64_t>(found)) << '\n';
    }
  }

  if (!canonical) {
    return fail("'" + path + "' is not the canonical encoding of the plan it decodes to: the " +
                number(static_cast<std::uint64_t>(bytes.size())) +
                " stored bytes hash to " + stored.to_hex() + " while the recomputed plan digest is " +
                digest.to_hex());
  }
  return EXIT_SUCCESS;
}
