// Platform adapter: files, durable file primitives, child processes and
// loopback sockets.
//
// Everything that depends on the operating system lives behind this header.
// The deterministic core never includes it, which is what keeps the model
// testable without a filesystem, a clock or a network. Durability claims made
// anywhere in this runtime bottom out in File::sync() and sync_directory().
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dce/status.hpp"

namespace dce::platform {

// ---------------------------------------------------------------------------
// Filesystem
// ---------------------------------------------------------------------------
struct FileStat {
  std::uint64_t size{0};
  bool is_directory{false};
  bool is_regular_file{false};
  bool is_symlink{false};
};

[[nodiscard]] Result<FileStat> stat_path(std::string_view path);
[[nodiscard]] Result<bool> path_exists(std::string_view path);

// Directory entries in ascending byte order; "." and ".." are never returned.
[[nodiscard]] Result<std::vector<std::string>> list_directory(std::string_view path);

// Creates the directory and every missing parent.
[[nodiscard]] Status make_directories(std::string_view path);

// Removes a single file. Removing an absent file reports not_found.
[[nodiscard]] Status remove_file(std::string_view path);

// Removes a file or a whole directory tree. Best effort across entries: the
// first failure is reported, and removal never follows a symbolic link out of
// the tree.
[[nodiscard]] Status remove_tree(std::string_view path);

// Joins a directory and a child name with the platform separator.
[[nodiscard]] std::string join_path(std::string_view directory, std::string_view name);

[[nodiscard]] std::uint64_t process_id() noexcept;

// ---------------------------------------------------------------------------
// Durable files
// ---------------------------------------------------------------------------
enum class OpenMode {
  read_only,    // existing file only
  read_write,   // create when absent, never truncate
  create_new,   // fail with already_exists when the path exists
};

class File {
 public:
  File() noexcept;
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File();

  [[nodiscard]] static Result<File> open(std::string_view path, OpenMode mode);

  // Positional write. Short writes are completed; a failure is reported.
  [[nodiscard]] Status write_at(std::uint64_t offset, std::span<const std::byte> data);

  // Positional read. Returns the number of bytes read, which is fewer than
  // requested only at end of file.
  [[nodiscard]] Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> buffer);

  // The durability boundary: FlushFileBuffers on Windows, fsync on POSIX.
  // When this returns ok the bytes are on stable storage.
  [[nodiscard]] Status sync();

  [[nodiscard]] Status truncate(std::uint64_t size);
  [[nodiscard]] Result<std::uint64_t> size();
  [[nodiscard]] Status close();

  [[nodiscard]] bool is_open() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Flushes directory metadata so that a completed rename survives a crash.
// On Windows this is a documented no-op: NTFS metadata ordering is handled by
// the filesystem and there is no portable directory handle to flush. The
// limitation is recorded in docs/PERSISTENCE.md.
[[nodiscard]] Status sync_directory(std::string_view path);

// Atomically replaces final_path with temp_path.
[[nodiscard]] Status atomic_replace(std::string_view temp_path, std::string_view final_path);

// An advisory whole-file lock. A second exclusive holder is refused with
// busy rather than blocked on, which is what makes a live stale writer
// impossible on one host.
class FileLock {
 public:
  FileLock() noexcept;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  ~FileLock();

  [[nodiscard]] static Result<FileLock> acquire(std::string_view path, bool exclusive);

  [[nodiscard]] Status release();
  [[nodiscard]] bool held() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Child processes
// ---------------------------------------------------------------------------
struct ProcessOptions {
  std::string program;
  std::vector<std::string> arguments;
  std::string working_directory;  // empty inherits the parent's directory
  bool capture_stdout{true};
  bool capture_stderr{false};  // when false the child inherits the parent's
};

// A child process that is really a separate operating-system process. The
// suites use it to prove restart, crash and partition behaviour rather than
// asserting it in a single address space.
class ChildProcess {
 public:
  ChildProcess() noexcept;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // Destruction does not wait: an unwaited child keeps running, so a suite
  // that leaks a process is visibly wrong rather than silently cleaned up.
  ~ChildProcess();

  [[nodiscard]] static Result<ChildProcess> spawn(const ProcessOptions& options);

  [[nodiscard]] Result<std::uint64_t> id();
  [[nodiscard]] Result<bool> running();

  // Blocking, bounded line read from the child's standard output. A line
  // longer than max_bytes is reported as limit_exceeded rather than truncated.
  // Returns std::nullopt once the child's output reaches end of file.
  [[nodiscard]] Result<std::optional<std::string>> read_line(std::size_t max_bytes);

  [[nodiscard]] Status write_line(std::string_view text);

  // Blocking wait. Returns the child's exit code.
  [[nodiscard]] Result<int> wait();

  // Immediate hard termination, used to model a crash at an awkward boundary.
  [[nodiscard]] Status terminate();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Loopback sockets
// ---------------------------------------------------------------------------
// Winsock must be started before any socket call; the implementation does this
// idempotently and every socket entry point calls it.
[[nodiscard]] Status initialize_networking();

class Socket {
 public:
  Socket() noexcept;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket();

  // Binds and listens on 127.0.0.1. Port 0 asks the operating system for an
  // ephemeral port, which local_port() then reports.
  [[nodiscard]] static Result<Socket> listen_loopback(std::uint16_t port);
  [[nodiscard]] static Result<Socket> connect_loopback(std::uint16_t port);

  [[nodiscard]] Result<std::uint16_t> local_port();
  [[nodiscard]] Result<Socket> accept();

  [[nodiscard]] Status send_all(std::span<const std::byte> data);

  // Reads at least one byte when the peer is still connected, and reports
  // closed when the peer shut the connection down.
  [[nodiscard]] Result<std::size_t> recv_some(std::span<std::byte> buffer);

  [[nodiscard]] Status recv_exact(std::span<std::byte> buffer);

  [[nodiscard]] Status set_no_delay(bool enabled);
  [[nodiscard]] Status shutdown_both();
  [[nodiscard]] Status close();
  [[nodiscard]] bool is_open() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Monotonic nanoseconds from an unspecified origin. Used for measurement only;
// no correctness decision in this runtime depends on it.
[[nodiscard]] std::uint64_t monotonic_nanos() noexcept;

}  // namespace dce::platform
