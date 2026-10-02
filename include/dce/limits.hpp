// Documented bounds for authoritative collections and payloads.
//
// Every collection that can arrive from a peer, from disk or from an operator
// is bounded here, and the bound is enforced before memory is reserved for it.
#pragma once

#include <cstddef>

namespace dce {

inline constexpr std::size_t kMaxSites = 4096;
inline constexpr std::size_t kMaxComponents = 1024;
inline constexpr std::size_t kMaxSteps = 2048;
inline constexpr std::size_t kMaxCohorts = 512;
inline constexpr std::size_t kMaxGates = 2048;
inline constexpr std::size_t kMaxEvidence = 4096;
inline constexpr std::size_t kMaxExceptions = 256;
inline constexpr std::size_t kMaxDeprecations = 512;
inline constexpr std::size_t kMaxDependencies = 256;
inline constexpr std::size_t kMaxPreconditions = 512;
inline constexpr std::size_t kMaxGraphNodes = 4096;
inline constexpr std::size_t kMaxGraphEdges = 16384;
inline constexpr std::size_t kMaxGraphHops = 64;
inline constexpr std::size_t kMaxReceipts = 65536;
inline constexpr std::size_t kMaxCheckpoints = 65536;
inline constexpr std::size_t kMaxReconciliations = 65536;
inline constexpr std::size_t kMaxStepDependencies = 64;
inline constexpr std::size_t kMaxCohortSites = 4096;
inline constexpr std::size_t kMaxGateEvidence = 32;
inline constexpr std::size_t kMaxTextBytes = 512;
inline constexpr std::size_t kMaxClaimBytes = 1024;

}  // namespace dce
