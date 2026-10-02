#include "dce/status.hpp"

namespace dce {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ok: return "ok";
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::malformed: return "malformed";
    case ErrorCode::out_of_range: return "out_of_range";
    case ErrorCode::overflow: return "overflow";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::already_exists: return "already_exists";
    case ErrorCode::duplicate: return "duplicate";
    case ErrorCode::conflict: return "conflict";
    case ErrorCode::stale: return "stale";
    case ErrorCode::fenced: return "fenced";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::indeterminate: return "indeterminate";
    case ErrorCode::refused: return "refused";
    case ErrorCode::denied: return "denied";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::corruption: return "corruption";
    case ErrorCode::torn_tail: return "torn_tail";
    case ErrorCode::busy: return "busy";
    case ErrorCode::closed: return "closed";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::limit_exceeded: return "limit_exceeded";
    case ErrorCode::internal: return "internal";
  }
  return "unknown";
}

}  // namespace dce
