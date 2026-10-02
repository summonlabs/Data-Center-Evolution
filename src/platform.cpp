// Platform adapter: the one translation unit in which the runtime touches the
// operating system.
//
// The public header declares every handle-owning class with a pointer to an
// implementation; this file is the only place where those implementations and
// the platform headers that describe them exist. Three rules are worth stating
// because the rest of the runtime is built on them:
//
//   * Durability is never implied. File::sync() is FlushFileBuffers on Windows
//     and fsync on POSIX and reports io_error whenever the call fails, and
//     sync_directory() flushes directory metadata on POSIX while being a
//     documented no-op on Windows.
//   * Authority is never blocked on. FileLock::acquire() fails with busy
//     instead of waiting, so a live stale writer cannot exist on one host.
//   * No exception leaves the API. Every failure that an operating-system call
//     can report is mapped onto the Status vocabulary.

#include "dce/platform.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#endif

#include "dce/text.hpp"

namespace dce::platform {
namespace {

// Bytes moved per operating-system call. Large enough to keep the call count
// low, small enough to stay comfortably inside a stack frame.
constexpr std::size_t kIoChunk = 64U * 1024U;

// Directory entries are ordered by the unsigned value of their bytes, so that
// both platforms produce the same order for names outside ASCII.
[[nodiscard]] bool byte_less(const std::string& left, const std::string& right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](char a, char b) { return static_cast<unsigned char>(a) < static_cast<unsigned char>(b); });
}

// Untrusted text is rendered before it reaches an error message, so a path can
// never smuggle a control sequence into a terminal.
[[nodiscard]] std::string quoted(std::string_view value) {
  return "\"" + text::escape_for_output(value) + "\"";
}

// The two path shapes no platform can act on: the empty path, and a path with
// an embedded NUL that every path-taking call would silently truncate.
[[nodiscard]] Status validate_path(std::string_view path) {
  if (path.empty()) {
    return Error{ErrorCode::invalid_argument, "path is empty"};
  }
  if (path.find('\0') != std::string_view::npos) {
    return Error{ErrorCode::invalid_argument, "path contains an embedded NUL byte"};
  }
  return Status{};
}

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Windows helpers
// ---------------------------------------------------------------------------

[[nodiscard]] bool to_utf8(std::wstring_view value, std::string& out) {
  out.clear();
  if (value.empty()) {
    return true;
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  const int length = static_cast<int>(value.size());
  const int needed = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr,
                                           nullptr);
  if (needed <= 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(needed));
  const int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, out.data(), needed,
                                            nullptr, nullptr);
  if (written != needed) {
    out.clear();
    return false;
  }
  return true;
}

[[nodiscard]] std::string win32_text(DWORD code) {
  const std::string fallback = "Windows error " + std::to_string(static_cast<unsigned long long>(code));
  LPWSTR buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return fallback;
  }
  const std::wstring message(buffer, static_cast<std::size_t>(length));
  ::LocalFree(buffer);
  std::wstring trimmed = message;
  while (!trimmed.empty() && (trimmed.back() == L'\r' || trimmed.back() == L'\n' || trimmed.back() == L' ')) {
    trimmed.pop_back();
  }
  std::string utf8;
  if (!to_utf8(trimmed, utf8) || utf8.empty()) {
    return fallback;
  }
  return utf8;
}

[[nodiscard]] Error win_error_code(ErrorCode code, std::string_view context, DWORD last) {
  return Error{code, std::string(context) + ": " + win32_text(last)};
}

[[nodiscard]] Error win_error(ErrorCode code, std::string_view context) {
  return win_error_code(code, context, ::GetLastError());
}

// The extended-length prefix removes the historical MAX_PATH ceiling. It is
// only applied to absolute paths that actually need it, because the prefix also
// disables path normalisation, which relative paths and "." components rely on.
constexpr std::size_t kExtendedPathThreshold = 240;

[[nodiscard]] bool path_is_absolute(std::wstring_view path) {
  if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  return path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/');
}

[[nodiscard]] std::wstring extended_path(std::wstring_view path) {
  std::wstring normalized;
  normalized.reserve(path.size() + 8);
  for (wchar_t character : path) {
    normalized.push_back(character == L'/' ? L'\\' : character);
  }
  if (normalized.size() >= 2 && normalized[0] == L'\\' && normalized[1] == L'\\') {
    return L"\\\\?\\UNC\\" + normalized.substr(2);
  }
  return L"\\\\?\\" + normalized;
}

// UTF-8 to UTF-16 with a hard failure on ill-formed input: a path that cannot
// be represented is reported, never silently replaced with question marks.
[[nodiscard]] bool to_wide(std::string_view value, std::wstring& out) {
  out.clear();
  if (value.empty()) {
    return true;
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  const int length = static_cast<int>(value.size());
  const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
  if (needed <= 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(needed));
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, out.data(), needed);
  if (written != needed) {
    out.clear();
    return false;
  }
  return true;
}

// Command lines are assembled with the quoting rules CreateProcessW parses: an
// argument that is empty or contains a blank, a tab, a quote or a newline is
// quoted, embedded quotes are escaped, and backslashes are doubled only where
// they precede a quote. That is what carries a path containing spaces intact.
void append_windows_argument(std::string& command_line, std::string_view argument) {
  const bool needs_quotes = argument.empty() || argument.find_first_of(" \t\n\v\"") != std::string_view::npos;
  if (!needs_quotes) {
    command_line.append(argument);
    return;
  }
  command_line.push_back('"');
  std::size_t backslashes = 0;
  for (char character : argument) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      command_line.append(backslashes * 2 + 1, '\\');
      backslashes = 0;
      command_line.push_back('"');
      continue;
    }
    command_line.append(backslashes, '\\');
    backslashes = 0;
    command_line.push_back(character);
  }
  command_line.append(backslashes * 2, '\\');
  command_line.push_back('"');
}

[[nodiscard]] Result<std::wstring> widen_path(std::string_view path) {
  DCE_TRY(validate_path(path));
  std::wstring wide;
  if (!to_wide(path, wide)) {
    return Error{ErrorCode::invalid_argument, "path is not valid UTF-8: " + quoted(path)};
  }
  if (wide.size() >= kExtendedPathThreshold && path_is_absolute(wide) && wide.rfind(L"\\\\?\\", 0) != 0) {
    return Result<std::wstring>(extended_path(wide));
  }
  return Result<std::wstring>(std::move(wide));
}

[[nodiscard]] bool is_dot_entry(const wchar_t* name) {
  if (name[0] != L'.') {
    return false;
  }
  if (name[1] == L'\0') {
    return true;
  }
  return name[1] == L'.' && name[2] == L'\0';
}

// A lock or a process handle that is closed on every path, including the ones
// taken when a later call in the same function fails.
class ScopedHandle {
 public:
  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE value) noexcept : value_(value) {}
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&& other) noexcept : value_(other.release()) {}
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  ~ScopedHandle() { reset(nullptr); }

  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] HANDLE release() noexcept {
    const HANDLE value = value_;
    value_ = nullptr;
    return value;
  }
  void reset(HANDLE value) noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(value_);
    }
    value_ = value;
  }

 private:
  HANDLE value_{nullptr};
};

// A standard handle the child may inherit. When the parent has none, which is
// the case for a process started without a console, the NUL device stands in so
// that CreateProcessW still receives a usable handle.
[[nodiscard]] HANDLE inherited_standard_handle(DWORD which, SECURITY_ATTRIBUTES& attributes,
                                               ScopedHandle& fallback) {
  const HANDLE handle = ::GetStdHandle(which);
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
    return handle;
  }
  fallback.reset(::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                               OPEN_EXISTING, 0, nullptr));
  return fallback.get();
}

[[nodiscard]] ErrorCode map_missing_file(DWORD last) noexcept {
  // ERROR_DIRECTORY is what the directory calls report when the name resolves
  // to something that is not a directory, which is not_found on POSIX too.
  if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND || last == ERROR_DIRECTORY) {
    return ErrorCode::not_found;
  }
  return ErrorCode::io_error;
}

[[nodiscard]] ErrorCode map_create_conflict(DWORD last) noexcept {
  if (last == ERROR_FILE_EXISTS || last == ERROR_ALREADY_EXISTS) {
    return ErrorCode::already_exists;
  }
  return ErrorCode::io_error;
}

#else

// ---------------------------------------------------------------------------
// POSIX helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string error_text(int error_number) {
  return std::error_code(error_number, std::generic_category()).message();
}

[[nodiscard]] Error os_error(ErrorCode code, std::string_view context) {
  return Error{code, std::string(context) + ": " + error_text(errno)};
}

[[nodiscard]] Error os_error_code(ErrorCode code, std::string_view context, int error_number) {
  return Error{code, std::string(context) + ": " + error_text(error_number)};
}

#if defined(O_CLOEXEC)
constexpr int kCloseOnExec = O_CLOEXEC;
#else
constexpr int kCloseOnExec = 0;
#endif

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Writing to a child or a peer that has already gone away must report EPIPE
// rather than kill the host process with SIGPIPE. The disposition is installed
// once, and every spawned child is given the default disposition back before
// it executes, so the programs this runtime starts are unaffected.
bool g_sigpipe_ignored_by_us = false;

void ensure_sigpipe_ignored() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction current {};
    if (::sigaction(SIGPIPE, nullptr, &current) != 0 || current.sa_handler != SIG_DFL) {
      // A host that installed its own disposition keeps it: the adapter does
      // not overrule the process that loaded it.
      return;
    }
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (::sigaction(SIGPIPE, &action, nullptr) == 0) {
      g_sigpipe_ignored_by_us = true;
    }
  });
}

void restore_default_sigpipe() noexcept {
  struct sigaction action {};
  action.sa_handler = SIG_DFL;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  (void)::sigaction(SIGPIPE, &action, nullptr);
}

void clear_close_on_exec(int descriptor) noexcept {
  const int flags = ::fcntl(descriptor, F_GETFD);
  if (flags >= 0) {
    (void)::fcntl(descriptor, F_SETFD, flags & ~FD_CLOEXEC);
  }
}

// Reports a failure that happened between fork() and exec() to the parent
// through a close-on-exec pipe, then leaves immediately. Nothing here may
// allocate or take a lock: the forked child owns only this thread.
[[noreturn]] void child_fail(int descriptor, int error_number) noexcept {
  const int code = error_number;
  for (;;) {
    const ssize_t written = ::write(descriptor, &code, sizeof(code));
    if (written >= 0 || errno != EINTR) {
      break;
    }
  }
  ::_exit(127);
}

// A pipe with both ends close-on-exec. The ends the child needs are duplicated
// onto descriptors 0, 1 and 2, which drops the flag again.
struct PipeFds {
  int ends[2]{-1, -1};

  PipeFds() = default;
  PipeFds(const PipeFds&) = delete;
  PipeFds& operator=(const PipeFds&) = delete;
  ~PipeFds() { close_ends(); }

  [[nodiscard]] bool create() noexcept {
    if (::pipe(ends) != 0) {
      return false;
    }
    clear_close_on_exec(ends[0]);
    clear_close_on_exec(ends[1]);
    return true;
  }

  void close_ends() noexcept {
    for (int& end : ends) {
      if (end >= 0) {
        ::close(end);
        end = -1;
      }
    }
  }

  [[nodiscard]] int read_end() const noexcept { return ends[0]; }
  [[nodiscard]] int write_end() const noexcept { return ends[1]; }

  [[nodiscard]] int release_read() noexcept {
    const int value = ends[0];
    ends[0] = -1;
    return value;
  }
  [[nodiscard]] int release_write() noexcept {
    const int value = ends[1];
    ends[1] = -1;
    return value;
  }
};

[[nodiscard]] int decode_wait_status(int status) noexcept {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 128;
}

#endif  // defined(_WIN32)

}  // namespace

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::string join_path(std::string_view directory, std::string_view name) {
#if defined(_WIN32)
  constexpr char kSeparator = '\\';
  const auto is_separator = [](char character) { return character == '\\' || character == '/'; };
#else
  constexpr char kSeparator = '/';
  const auto is_separator = [](char character) { return character == '/'; };
#endif
  if (directory.empty()) {
    return std::string(name);
  }
  if (name.empty()) {
    return std::string(directory);
  }
  std::string joined(directory);
  if (!is_separator(joined.back())) {
    joined.push_back(kSeparator);
  }
  joined.append(name);
  return joined;
}

std::uint64_t process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(static_cast<long long>(::getpid()));
#endif
}

Result<FileStat> stat_path(std::string_view path) {
  DCE_TRY(validate_path(path));
#if defined(_WIN32)
  DCE_ASSIGN(wide, widen_path(path));
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!::GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
    const DWORD last = ::GetLastError();
    return win_error_code(map_missing_file(last), "stat " + quoted(path), last);
  }
  FileStat info;
  info.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  info.is_symlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  info.is_regular_file = !info.is_directory && !info.is_symlink;
  info.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) | static_cast<std::uint64_t>(data.nFileSizeLow);
  return info;
#else
  const std::string text(path);
  struct stat data {};
  if (::lstat(text.c_str(), &data) != 0) {
    const int error_number = errno;
    const ErrorCode code = (error_number == ENOENT || error_number == ENOTDIR) ? ErrorCode::not_found : ErrorCode::io_error;
    return os_error_code(code, "stat " + quoted(path), error_number);
  }
  FileStat info;
  info.is_directory = S_ISDIR(data.st_mode);
  info.is_symlink = S_ISLNK(data.st_mode);
  info.is_regular_file = S_ISREG(data.st_mode);
  if (data.st_size > 0) {
    info.size = static_cast<std::uint64_t>(data.st_size);
  }
  return info;
#endif
}

Result<bool> path_exists(std::string_view path) {
  DCE_TRY(validate_path(path));
#if defined(_WIN32)
  DCE_ASSIGN(wide, widen_path(path));
  const DWORD attributes = ::GetFileAttributesW(wide.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  const DWORD last = ::GetLastError();
  if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) {
    return false;
  }
  return win_error_code(ErrorCode::io_error, "inspect " + quoted(path), last);
#else
  const std::string text(path);
  struct stat data {};
  if (::lstat(text.c_str(), &data) == 0) {
    return true;
  }
  const int error_number = errno;
  if (error_number == ENOENT || error_number == ENOTDIR) {
    return false;
  }
  return os_error_code(ErrorCode::io_error, "inspect " + quoted(path), error_number);
#endif
}

Result<std::vector<std::string>> list_directory(std::string_view path) {
  DCE_TRY(validate_path(path));
  std::vector<std::string> names;
#if defined(_WIN32)
  DCE_ASSIGN(wide, widen_path(path));
  std::wstring pattern = wide;
  if (pattern.empty() || (pattern.back() != L'\\' && pattern.back() != L'/')) {
    pattern.push_back(L'\\');
  }
  pattern.push_back(L'*');
  WIN32_FIND_DATAW entry{};
  const HANDLE search = ::FindFirstFileW(pattern.c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD last = ::GetLastError();
    return win_error_code(map_missing_file(last), "list " + quoted(path), last);
  }
  for (;;) {
    if (!is_dot_entry(entry.cFileName)) {
      std::string name;
      if (!to_utf8(entry.cFileName, name)) {
        ::FindClose(search);
        return Error{ErrorCode::io_error, "directory entry of " + quoted(path) + " is not valid UTF-16"};
      }
      names.push_back(std::move(name));
    }
    if (!::FindNextFileW(search, &entry)) {
      const DWORD last = ::GetLastError();
      ::FindClose(search);
      if (last != ERROR_NO_MORE_FILES) {
        return win_error_code(ErrorCode::io_error, "list " + quoted(path), last);
      }
      break;
    }
  }
#else
  const std::string text(path);
  DIR* directory = ::opendir(text.c_str());
  if (directory == nullptr) {
    const int error_number = errno;
    const ErrorCode code = (error_number == ENOENT || error_number == ENOTDIR) ? ErrorCode::not_found : ErrorCode::io_error;
    return os_error_code(code, "list " + quoted(path), error_number);
  }
  for (;;) {
    errno = 0;
    struct dirent* entry = ::readdir(directory);
    if (entry == nullptr) {
      const int error_number = errno;
      (void)::closedir(directory);
      if (error_number != 0) {
        return os_error_code(ErrorCode::io_error, "list " + quoted(path), error_number);
      }
      break;
    }
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    names.emplace_back(name);
  }
#endif
  std::sort(names.begin(), names.end(), byte_less);
  return Result<std::vector<std::string>>(std::move(names));
}

Status make_directories(std::string_view path) {
  DCE_TRY(validate_path(path));
#if defined(_WIN32)
  DCE_ASSIGN(wide, widen_path(path));
  std::wstring target = wide;
  for (wchar_t& character : target) {
    if (character == L'/') {
      character = L'\\';
    }
  }
  const std::size_t prefix = [&target]() -> std::size_t {
    const auto skip_unc = [&target](std::size_t start) -> std::size_t {
      const std::size_t server = target.find(L'\\', start);
      if (server == std::wstring::npos) {
        return target.size();
      }
      const std::size_t share = target.find(L'\\', server + 1);
      if (share == std::wstring::npos) {
        return target.size();
      }
      return share;
    };
    if (target.size() >= 4 && target[0] == L'\\' && target[1] == L'\\' && (target[2] == L'?' || target[2] == L'.') &&
        target[3] == L'\\') {
      const std::size_t device = 4;
      if (target.size() >= device + 4 && (target[device] == L'U' || target[device] == L'u') &&
          (target[device + 1] == L'N' || target[device + 1] == L'n') &&
          (target[device + 2] == L'C' || target[device + 2] == L'c') && target[device + 3] == L'\\') {
        return skip_unc(device + 4);
      }
      // The extended form of a drive path is \?\C:\..., and the drive letter
      // belongs to the prefix: creating "C:" as a component would be refused.
      if (target.size() > device + 1 && target[device + 1] == L':') {
        return device + 2;
      }
      return device;
    }
    if (target.size() >= 2 && target[0] == L'\\' && target[1] == L'\\') {
      return skip_unc(2);
    }
    if (target.size() >= 2 && target[1] == L':') {
      return 2;
    }
    if (!target.empty() && target[0] == L'\\') {
      return 1;
    }
    return 0;
  }();
  std::wstring current = target.substr(0, prefix);
  std::size_t cursor = prefix;
  while (cursor < target.size()) {
    std::size_t separator = target.find(L'\\', cursor);
    if (separator == std::wstring::npos) {
      separator = target.size();
    }
    if (separator > cursor) {
      if (!current.empty() && current.back() != L'\\') {
        current.push_back(L'\\');
      }
      current.append(target, cursor, separator - cursor);
      if (!::CreateDirectoryW(current.c_str(), nullptr)) {
        const DWORD last = ::GetLastError();
        std::string component;
        (void)to_utf8(current, component);
        if (last != ERROR_ALREADY_EXISTS) {
          return win_error_code(ErrorCode::io_error, "create directory " + quoted(component), last);
        }
        const DWORD attributes = ::GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
          return Error{ErrorCode::already_exists,
                       "create directory " + quoted(component) + ": it exists and is not a directory"};
        }
      }
    }
    cursor = separator + 1;
  }
  return Status{};
#else
  const std::string target(path);
  std::string current;
  std::size_t cursor = 0;
  if (target[0] == '/') {
    current = "/";
    cursor = 1;
  }
  while (cursor < target.size()) {
    std::size_t separator = target.find('/', cursor);
    if (separator == std::string::npos) {
      separator = target.size();
    }
    if (separator > cursor) {
      if (!current.empty() && current.back() != '/') {
        current.push_back('/');
      }
      current.append(target, cursor, separator - cursor);
      if (::mkdir(current.c_str(), static_cast<mode_t>(0777)) != 0) {
        const int error_number = errno;
        if (error_number != EEXIST) {
          return os_error_code(ErrorCode::io_error, "create directory " + quoted(current), error_number);
        }
        struct stat data {};
        if (::stat(current.c_str(), &data) != 0 || !S_ISDIR(data.st_mode)) {
          return Error{ErrorCode::already_exists,
                       "create directory " + quoted(current) + ": it exists and is not a directory"};
        }
      }
    }
    cursor = separator + 1;
  }
  return Status{};
#endif
}

Status remove_file(std::string_view path) {
  DCE_TRY(validate_path(path));
#if defined(_WIN32)
  DCE_ASSIGN(wide, widen_path(path));
  if (!::DeleteFileW(wide.c_str())) {
    const DWORD last = ::GetLastError();
    return win_error_code(map_missing_file(last), "remove " + quoted(path), last);
  }
  return Status{};
#else
  const std::string text(path);
  if (::unlink(text.c_str()) != 0) {
    const int error_number = errno;
    const ErrorCode code = (error_number == ENOENT || error_number == ENOTDIR) ? ErrorCode::not_found : ErrorCode::io_error;
    return os_error_code(code, "remove " + quoted(path), error_number);
  }
  return Status{};
#endif
}

namespace {

// The link itself is always removed, never its target: a symbolic link or a
// junction pointing out of the tree is unlinked, so removal cannot escape the
// subtree it was asked to delete.
[[nodiscard]] Status remove_directory(std::string_view path) {
#if defined(_WIN32)
  DCE_ASSIGN(wide, widen_path(path));
  if (!::RemoveDirectoryW(wide.c_str())) {
    const DWORD last = ::GetLastError();
    return win_error_code(map_missing_file(last), "remove directory " + quoted(path), last);
  }
  return Status{};
#else
  const std::string text(path);
  if (::rmdir(text.c_str()) != 0) {
    const int error_number = errno;
    const ErrorCode code = (error_number == ENOENT || error_number == ENOTDIR) ? ErrorCode::not_found : ErrorCode::io_error;
    return os_error_code(code, "remove directory " + quoted(path), error_number);
  }
  return Status{};
#endif
}

}  // namespace

Status remove_tree(std::string_view path) {
  DCE_TRY(validate_path(path));
  DCE_ASSIGN(info, stat_path(path));
  if (info.is_symlink) {
    return info.is_directory ? remove_directory(path) : remove_file(path);
  }
  if (!info.is_directory) {
    return remove_file(path);
  }
  DCE_ASSIGN(names, list_directory(path));
  Status first_failure{};
  for (const std::string& name : names) {
    const std::string child = join_path(path, name);
    const Status removed = remove_tree(child);
    if (!removed.ok() && first_failure.ok()) {
      first_failure = removed;
    }
  }
  if (!first_failure.ok()) {
    return first_failure;
  }
  return remove_directory(path);
}

// ---------------------------------------------------------------------------
// Durable files
// ---------------------------------------------------------------------------
struct File::Impl {
#if defined(_WIN32)
  HANDLE handle{INVALID_HANDLE_VALUE};

  [[nodiscard]] bool opened() const noexcept { return handle != INVALID_HANDLE_VALUE; }
  bool close_native() noexcept {
    if (handle == INVALID_HANDLE_VALUE) {
      return true;
    }
    const HANDLE closing = handle;
    handle = INVALID_HANDLE_VALUE;
    return ::CloseHandle(closing) != 0;
  }
#else
  int handle{-1};

  [[nodiscard]] bool opened() const noexcept { return handle >= 0; }
  bool close_native() noexcept {
    if (handle < 0) {
      return true;
    }
    const int closing = handle;
    handle = -1;
    return ::close(closing) == 0;
  }
#endif
  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() { close_native(); }
};

File::File(File&& other) noexcept : impl_(std::move(other.impl_)) {}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

File::File() noexcept = default;

File::~File() = default;

Result<File> File::open(std::string_view path, OpenMode mode) {
  DCE_TRY(validate_path(path));
  auto implementation = std::make_unique<Impl>();
#if defined(_WIN32)
  DCE_ASSIGN(wide, widen_path(path));
  DWORD access = GENERIC_READ;
  DWORD disposition = OPEN_EXISTING;
  switch (mode) {
    case OpenMode::read_only:
      access = GENERIC_READ;
      disposition = OPEN_EXISTING;
      break;
    case OpenMode::read_write:
      access = GENERIC_READ | GENERIC_WRITE;
      disposition = OPEN_ALWAYS;
      break;
    case OpenMode::create_new:
      access = GENERIC_READ | GENERIC_WRITE;
      disposition = CREATE_NEW;
      break;
  }
  const HANDLE handle = ::CreateFileW(wide.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD last = ::GetLastError();
    const ErrorCode code = disposition == CREATE_NEW ? map_create_conflict(last) : map_missing_file(last);
    return win_error_code(code, "open " + quoted(path), last);
  }
  implementation->handle = handle;
#else
  const std::string text(path);
  int flags = O_RDONLY;
  switch (mode) {
    case OpenMode::read_only:
      flags = O_RDONLY;
      break;
    case OpenMode::read_write:
      flags = O_RDWR | O_CREAT;
      break;
    case OpenMode::create_new:
      flags = O_RDWR | O_CREAT | O_EXCL;
      break;
  }
  flags |= kCloseOnExec;
  const int descriptor = ::open(text.c_str(), flags, static_cast<mode_t>(0644));
  if (descriptor < 0) {
    const int error_number = errno;
    ErrorCode code = ErrorCode::io_error;
    if (error_number == ENOENT || error_number == ENOTDIR) {
      code = ErrorCode::not_found;
    } else if (error_number == EEXIST) {
      code = ErrorCode::already_exists;
    }
    return os_error_code(code, "open " + quoted(path), error_number);
  }
  implementation->handle = descriptor;
#endif
  File file;
  file.impl_ = std::move(implementation);
  return Result<File>(std::move(file));
}

Status File::write_at(std::uint64_t offset, std::span<const std::byte> data) {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "write: the file is not open"};
  }
  if (data.empty()) {
    return Status{};
  }
  if (data.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
    return Error{ErrorCode::overflow, "write: the offset and the buffer length do not fit in a file position"};
  }
#if defined(_WIN32)
  std::size_t written = 0;
  while (written < data.size()) {
    const std::size_t remaining = data.size() - written;
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    const std::uint64_t position = offset + static_cast<std::uint64_t>(written);
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(position >> 32U);
    DWORD moved = 0;
    if (!::WriteFile(impl_->handle, data.data() + written, chunk, &moved, &overlapped)) {
      return win_error(ErrorCode::io_error, "write " + std::to_string(position));
    }
    if (moved == 0) {
      return Error{ErrorCode::io_error, "write reported no progress"};
    }
    written += static_cast<std::size_t>(moved);
  }
#else
  std::size_t written = 0;
  while (written < data.size()) {
    const std::size_t chunk = std::min<std::size_t>(data.size() - written, kIoChunk);
    const std::uint64_t position = offset + static_cast<std::uint64_t>(written);
    const ssize_t moved = ::pwrite(impl_->handle, data.data() + written, chunk, static_cast<off_t>(position));
    if (moved < 0) {
      const int error_number = errno;
      if (error_number == EINTR) {
        continue;
      }
      return os_error_code(ErrorCode::io_error, "write " + std::to_string(position), error_number);
    }
    if (moved == 0) {
      return Error{ErrorCode::io_error, "write reported no progress"};
    }
    written += static_cast<std::size_t>(moved);
  }
#endif
  return Status{};
}

Result<std::size_t> File::read_at(std::uint64_t offset, std::span<std::byte> buffer) {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "read: the file is not open"};
  }
  std::size_t total = 0;
  if (buffer.empty()) {
    return total;
  }
  if (buffer.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
    return Error{ErrorCode::overflow, "read: the offset and the buffer length do not fit in a file position"};
  }
#if defined(_WIN32)
  while (total < buffer.size()) {
    const std::size_t remaining = buffer.size() - total;
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    const std::uint64_t position = offset + static_cast<std::uint64_t>(total);
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(position >> 32U);
    DWORD moved = 0;
    if (!::ReadFile(impl_->handle, buffer.data() + total, chunk, &moved, &overlapped)) {
      const DWORD last = ::GetLastError();
      if (last == ERROR_HANDLE_EOF) {
        break;
      }
      return win_error_code(ErrorCode::io_error, "read " + std::to_string(position), last);
    }
    if (moved == 0) {
      break;
    }
    total += static_cast<std::size_t>(moved);
  }
#else
  while (total < buffer.size()) {
    const std::size_t chunk = std::min<std::size_t>(buffer.size() - total, kIoChunk);
    const std::uint64_t position = offset + static_cast<std::uint64_t>(total);
    const ssize_t moved = ::pread(impl_->handle, buffer.data() + total, chunk, static_cast<off_t>(position));
    if (moved < 0) {
      const int error_number = errno;
      if (error_number == EINTR) {
        continue;
      }
      return os_error_code(ErrorCode::io_error, "read " + std::to_string(position), error_number);
    }
    if (moved == 0) {
      break;
    }
    total += static_cast<std::size_t>(moved);
  }
#endif
  return total;
}

Status File::sync() {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "sync: the file is not open"};
  }
#if defined(_WIN32)
  if (!::FlushFileBuffers(impl_->handle)) {
    return win_error(ErrorCode::io_error, "flush the file to stable storage");
  }
#else
  if (::fsync(impl_->handle) != 0) {
    return os_error(ErrorCode::io_error, "flush the file to stable storage");
  }
#endif
  return Status{};
}

Status File::truncate(std::uint64_t size) {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "truncate: the file is not open"};
  }
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(size);
  if (!::SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN)) {
    return win_error(ErrorCode::io_error, "truncate: seek to " + std::to_string(size));
  }
  if (!::SetEndOfFile(impl_->handle)) {
    return win_error(ErrorCode::io_error, "truncate to " + std::to_string(size));
  }
#else
  if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return Error{ErrorCode::out_of_range, "truncate: the size does not fit in a file offset"};
  }
  if (::ftruncate(impl_->handle, static_cast<off_t>(size)) != 0) {
    return os_error(ErrorCode::io_error, "truncate to " + std::to_string(size));
  }
#endif
  return Status{};
}

Result<std::uint64_t> File::size() {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "size: the file is not open"};
  }
#if defined(_WIN32)
  LARGE_INTEGER length{};
  if (!::GetFileSizeEx(impl_->handle, &length)) {
    return win_error(ErrorCode::io_error, "read the file size");
  }
  return static_cast<std::uint64_t>(length.QuadPart);
#else
  struct stat data {};
  if (::fstat(impl_->handle, &data) != 0) {
    return os_error(ErrorCode::io_error, "read the file size");
  }
  return static_cast<std::uint64_t>(data.st_size);
#endif
}

Status File::close() {
  if (!impl_) {
    return Status{};
  }
  if (!impl_->close_native()) {
    return Error{ErrorCode::io_error, "close the file"};
  }
  return Status{};
}

bool File::is_open() const noexcept { return impl_ != nullptr && impl_->opened(); }

// Windows has no portable handle for a directory that can be flushed: NTFS
// orders directory metadata against the rename that created it, so a
// MoveFileExW carrying MOVEFILE_WRITE_THROUGH is already durable and there is
// nothing left to flush here. The limitation is recorded in docs/PERSISTENCE.md.
Status sync_directory(std::string_view path) {
#if defined(_WIN32)
  DCE_TRY(validate_path(path));
  return Status{};
#else
  DCE_TRY(validate_path(path));
  const std::string text(path);
  const int descriptor = ::open(text.c_str(), O_RDONLY | kCloseOnExec);
  if (descriptor < 0) {
    const int error_number = errno;
    const ErrorCode code = (error_number == ENOENT || error_number == ENOTDIR) ? ErrorCode::not_found : ErrorCode::io_error;
    return os_error_code(code, "open directory " + quoted(path), error_number);
  }
  const int flushed = ::fsync(descriptor);
  const int flush_error = errno;
  const int closed = ::close(descriptor);
  const int close_error = errno;
  if (flushed != 0) {
    return os_error_code(ErrorCode::io_error, "flush directory " + quoted(path), flush_error);
  }
  if (closed != 0) {
    return os_error_code(ErrorCode::io_error, "close directory " + quoted(path), close_error);
  }
  return Status{};
#endif
}

Status atomic_replace(std::string_view temp_path, std::string_view final_path) {
  DCE_TRY(validate_path(temp_path));
  DCE_TRY(validate_path(final_path));
#if defined(_WIN32)
  DCE_ASSIGN(temp_wide, widen_path(temp_path));
  DCE_ASSIGN(final_wide, widen_path(final_path));
  if (!::MoveFileExW(temp_wide.c_str(), final_wide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD last = ::GetLastError();
    return win_error_code(map_missing_file(last), "replace " + quoted(final_path) + " with " + quoted(temp_path), last);
  }
  return Status{};
#else
  const std::string temp_text(temp_path);
  const std::string final_text(final_path);
  if (::rename(temp_text.c_str(), final_text.c_str()) != 0) {
    const int error_number = errno;
    const ErrorCode code = (error_number == ENOENT || error_number == ENOTDIR) ? ErrorCode::not_found : ErrorCode::io_error;
    return os_error_code(code, "replace " + quoted(final_path) + " with " + quoted(temp_path), error_number);
  }
  return Status{};
#endif
}

// ---------------------------------------------------------------------------
// Advisory whole-file locks
// ---------------------------------------------------------------------------
struct FileLock::Impl {
#if defined(_WIN32)
  HANDLE handle{INVALID_HANDLE_VALUE};
  bool locked{false};

  [[nodiscard]] bool held() const noexcept { return locked && handle != INVALID_HANDLE_VALUE; }
  bool release_native() noexcept {
    bool released = true;
    if (handle != INVALID_HANDLE_VALUE) {
      if (locked) {
        OVERLAPPED overlapped{};
        released = ::UnlockFileEx(handle, 0, 1, 0, &overlapped) != 0;
      }
      const HANDLE closing = handle;
      handle = INVALID_HANDLE_VALUE;
      locked = false;
      if (::CloseHandle(closing) == 0) {
        released = false;
      }
    }
    return released;
  }
#else
  int handle{-1};
  bool locked{false};

  [[nodiscard]] bool held() const noexcept { return locked && handle >= 0; }
  bool release_native() noexcept {
    bool released = true;
    if (handle >= 0) {
      if (locked && ::flock(handle, LOCK_UN) != 0) {
        released = false;
      }
      const int closing = handle;
      handle = -1;
      locked = false;
      // The close releases the lock as well, so it happens on every path.
      if (::close(closing) != 0) {
        released = false;
      }
    }
    return released;
  }
#endif
  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() { release_native(); }
};

FileLock::FileLock(FileLock&& other) noexcept : impl_(std::move(other.impl_)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

FileLock::FileLock() noexcept = default;

FileLock::~FileLock() = default;

Result<FileLock> FileLock::acquire(std::string_view path, bool exclusive) {
  DCE_TRY(validate_path(path));
  auto implementation = std::make_unique<Impl>();
#if defined(_WIN32)
  DCE_ASSIGN(wide, widen_path(path));
  // The file is created when it is absent and shared with everyone, because
  // exclusion is expressed by the byte-range lock and not by the share mode:
  // a second acquirer must be able to open the file and then be refused.
  const HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win_error(ErrorCode::io_error, "open lock file " + quoted(path));
  }
  OVERLAPPED overlapped{};
  DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
  if (exclusive) {
    flags |= LOCKFILE_EXCLUSIVE_LOCK;
  }
  if (!::LockFileEx(handle, flags, 0, 1, 0, &overlapped)) {
    const DWORD last = ::GetLastError();
    (void)::CloseHandle(handle);
    if (last == ERROR_LOCK_VIOLATION || last == ERROR_SHARING_VIOLATION || last == ERROR_IO_PENDING ||
        last == ERROR_OPERATION_ABORTED) {
      return win_error_code(ErrorCode::busy, "lock " + quoted(path), last);
    }
    return win_error_code(ErrorCode::io_error, "lock " + quoted(path), last);
  }
  implementation->handle = handle;
  implementation->locked = true;
#else
  const std::string text(path);
  const int descriptor = ::open(text.c_str(), O_RDWR | O_CREAT | kCloseOnExec, static_cast<mode_t>(0644));
  if (descriptor < 0) {
    return os_error(ErrorCode::io_error, "open lock file " + quoted(path));
  }
  const int operation = (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
  if (::flock(descriptor, operation) != 0) {
    const int error_number = errno;
    (void)::close(descriptor);
    if (error_number == EWOULDBLOCK || error_number == EAGAIN) {
      return os_error_code(ErrorCode::busy, "lock " + quoted(path), error_number);
    }
    return os_error_code(ErrorCode::io_error, "lock " + quoted(path), error_number);
  }
  implementation->handle = descriptor;
  implementation->locked = true;
#endif
  FileLock lock;
  lock.impl_ = std::move(implementation);
  return Result<FileLock>(std::move(lock));
}

Status FileLock::release() {
  if (!impl_) {
    return Status{};
  }
  if (!impl_->release_native()) {
    return Error{ErrorCode::io_error, "release the file lock"};
  }
  return Status{};
}

bool FileLock::held() const noexcept { return impl_ != nullptr && impl_->held(); }

// ---------------------------------------------------------------------------
// Child processes
// ---------------------------------------------------------------------------
struct ChildProcess::Impl {
#if defined(_WIN32)
  HANDLE process{nullptr};
  HANDLE stdin_write{nullptr};
  HANDLE stdout_read{nullptr};

  [[nodiscard]] bool started() const noexcept { return process != nullptr; }
  [[nodiscard]] bool has_input() const noexcept { return stdin_write != nullptr; }
  [[nodiscard]] bool has_output() const noexcept { return stdout_read != nullptr; }

  void close_native() noexcept {
    if (stdin_write != nullptr) {
      (void)::CloseHandle(stdin_write);
      stdin_write = nullptr;
    }
    if (stdout_read != nullptr) {
      (void)::CloseHandle(stdout_read);
      stdout_read = nullptr;
    }
    if (process != nullptr) {
      (void)::CloseHandle(process);
      process = nullptr;
    }
  }
#else
  pid_t pid{-1};
  int stdin_write{-1};
  int stdout_read{-1};

  [[nodiscard]] bool started() const noexcept { return pid > 0; }
  [[nodiscard]] bool has_input() const noexcept { return stdin_write >= 0; }
  [[nodiscard]] bool has_output() const noexcept { return stdout_read >= 0; }

  void close_native() noexcept {
    if (stdin_write >= 0) {
      (void)::close(stdin_write);
      stdin_write = -1;
    }
    if (stdout_read >= 0) {
      (void)::close(stdout_read);
      stdout_read = -1;
    }
    if (pid > 0) {
      if (!exited) {
        // Destruction never blocks. A child that has already exited is reaped
        // so that it does not linger as a zombie; a child that is still running
        // is deliberately left alone.
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
          exited = true;
          exit_code = decode_wait_status(status);
        }
      }
      pid = -1;
    }
  }
#endif
  std::string pending;
  bool output_ended{false};
  bool exited{false};
  int exit_code{0};

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() { close_native(); }

  [[nodiscard]] Status read_chunk(std::size_t room);
  [[nodiscard]] Status write_all(std::string_view text);
};

Status ChildProcess::Impl::read_chunk(std::size_t room) {
  if (room == 0) {
    return Status{};
  }
  std::array<char, kIoChunk> chunk;
#if defined(_WIN32)
  const DWORD want = static_cast<DWORD>(std::min<std::size_t>(room, chunk.size()));
  DWORD received = 0;
  if (!::ReadFile(stdout_read, chunk.data(), want, &received, nullptr)) {
    const DWORD last = ::GetLastError();
    if (last == ERROR_BROKEN_PIPE || last == ERROR_HANDLE_EOF) {
      output_ended = true;
      return Status{};
    }
    return win_error_code(ErrorCode::io_error, "read the child's standard output", last);
  }
  if (received == 0) {
    output_ended = true;
    return Status{};
  }
  pending.append(chunk.data(), static_cast<std::size_t>(received));
#else
  const std::size_t want = std::min(room, chunk.size());
  ssize_t received = -1;
  for (;;) {
    received = ::read(stdout_read, chunk.data(), want);
    if (received >= 0 || errno != EINTR) {
      break;
    }
  }
  if (received < 0) {
    return os_error(ErrorCode::io_error, "read the child's standard output");
  }
  if (received == 0) {
    output_ended = true;
    return Status{};
  }
  pending.append(chunk.data(), static_cast<std::size_t>(received));
#endif
  return Status{};
}

Status ChildProcess::Impl::write_all(std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const std::size_t remaining = text.size() - written;
#if defined(_WIN32)
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    DWORD moved = 0;
    if (!::WriteFile(stdin_write, text.data() + written, chunk, &moved, nullptr)) {
      const DWORD last = ::GetLastError();
      if (last == ERROR_BROKEN_PIPE || last == ERROR_NO_DATA || last == ERROR_INVALID_HANDLE) {
        return win_error_code(ErrorCode::closed, "write the child's standard input", last);
      }
      return win_error_code(ErrorCode::io_error, "write the child's standard input", last);
    }
    if (moved == 0) {
      return Error{ErrorCode::io_error, "writing to the child's standard input reported no progress"};
    }
    written += static_cast<std::size_t>(moved);
#else
    const std::size_t chunk = std::min<std::size_t>(remaining, kIoChunk);
    const ssize_t moved = ::write(stdin_write, text.data() + written, chunk);
    if (moved < 0) {
      const int error_number = errno;
      if (error_number == EINTR) {
        continue;
      }
      const ErrorCode code = error_number == EPIPE ? ErrorCode::closed : ErrorCode::io_error;
      return os_error_code(code, "write the child's standard input", error_number);
    }
    if (moved == 0) {
      return Error{ErrorCode::io_error, "writing to the child's standard input reported no progress"};
    }
    written += static_cast<std::size_t>(moved);
#endif
  }
  return Status{};
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : impl_(std::move(other.impl_)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

ChildProcess::ChildProcess() noexcept = default;

ChildProcess::~ChildProcess() = default;

Result<ChildProcess> ChildProcess::spawn(const ProcessOptions& options) {
  if (options.program.empty()) {
    return Error{ErrorCode::invalid_argument, "spawn: the program path is empty"};
  }
  if (options.program.find('\0') != std::string::npos) {
    return Error{ErrorCode::invalid_argument, "spawn: the program path contains an embedded NUL byte"};
  }
  auto implementation = std::make_unique<Impl>();
#if defined(_WIN32)
  std::string command_line;
  append_windows_argument(command_line, options.program);
  for (const std::string& argument : options.arguments) {
    command_line.push_back(' ');
    append_windows_argument(command_line, argument);
  }
  std::wstring command_wide;
  if (!to_wide(command_line, command_wide)) {
    return Error{ErrorCode::invalid_argument, "spawn: the command line is not valid UTF-8"};
  }
  std::vector<wchar_t> command_buffer(command_wide.begin(), command_wide.end());
  command_buffer.push_back(L'\0');

  std::wstring working_directory;
  if (!options.working_directory.empty()) {
    DCE_ASSIGN(directory_wide, widen_path(options.working_directory));
    working_directory = std::move(directory_wide);
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  attributes.lpSecurityDescriptor = nullptr;

  HANDLE raw_read = nullptr;
  HANDLE raw_write = nullptr;
  if (!::CreatePipe(&raw_read, &raw_write, &attributes, 0)) {
    return win_error(ErrorCode::io_error, "create the child's standard input pipe");
  }
  ScopedHandle child_stdin(raw_read);
  ScopedHandle parent_stdin(raw_write);
  if (!::SetHandleInformation(parent_stdin.get(), HANDLE_FLAG_INHERIT, 0)) {
    return win_error(ErrorCode::io_error, "protect the child's standard input pipe");
  }

  ScopedHandle child_stdout;
  ScopedHandle parent_stdout;
  if (options.capture_stdout) {
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!::CreatePipe(&output_read, &output_write, &attributes, 0)) {
      return win_error(ErrorCode::io_error, "create the child's standard output pipe");
    }
    parent_stdout.reset(output_read);
    child_stdout.reset(output_write);
    if (!::SetHandleInformation(parent_stdout.get(), HANDLE_FLAG_INHERIT, 0)) {
      return win_error(ErrorCode::io_error, "protect the child's standard output pipe");
    }
  }

  ScopedHandle device_fallback;
  const HANDLE output_target = child_stdout.get() != nullptr
                                   ? child_stdout.get()
                                   : inherited_standard_handle(STD_OUTPUT_HANDLE, attributes, device_fallback);
  if (output_target == nullptr) {
    return win_error(ErrorCode::io_error, "obtain the parent's standard output handle");
  }
  const HANDLE error_target = (options.capture_stderr && child_stdout.get() != nullptr)
                                  ? child_stdout.get()
                                  : inherited_standard_handle(STD_ERROR_HANDLE, attributes, device_fallback);
  if (error_target == nullptr) {
    return win_error(ErrorCode::io_error, "obtain the parent's standard error handle");
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = child_stdin.get();
  startup.hStdOutput = output_target;
  startup.hStdError = error_target;

  // The image is named by the command line alone, so that a program is looked
  // up exactly as the caller spelled it, including through PATH.
  PROCESS_INFORMATION information{};
  const BOOL created = ::CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        working_directory.empty() ? nullptr : working_directory.c_str(), &startup,
                                        &information);
  if (!created) {
    const DWORD last = ::GetLastError();
    const ErrorCode code = map_missing_file(last);
    return win_error_code(code, "start " + quoted(options.program), last);
  }
  ScopedHandle process(information.hProcess);
  ScopedHandle thread(information.hThread);
  implementation->process = process.release();
  implementation->stdin_write = parent_stdin.release();
  implementation->stdout_read = parent_stdout.release();
#else
  ensure_sigpipe_ignored();

  PipeFds child_stdin;
  PipeFds child_stdout;
  PipeFds exec_status;
  if (!child_stdin.create()) {
    return os_error(ErrorCode::io_error, "create the child's standard input pipe");
  }
  if (options.capture_stdout && !child_stdout.create()) {
    return os_error(ErrorCode::io_error, "create the child's standard output pipe");
  }
  if (!exec_status.create()) {
    return os_error(ErrorCode::io_error, "create the exec status pipe");
  }

  // Everything the child needs is built before the fork: between fork() and
  // exec() only async-signal-safe calls are made, and none of them allocate.
  std::vector<char*> argv;
  argv.reserve(options.arguments.size() + 2);
  argv.push_back(const_cast<char*>(options.program.c_str()));
  for (const std::string& argument : options.arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  const std::string working_directory = options.working_directory;

  const pid_t child_pid = ::fork();
  if (child_pid < 0) {
    return os_error(ErrorCode::io_error, "fork");
  }
  if (child_pid == 0) {
    if (!working_directory.empty() && ::chdir(working_directory.c_str()) != 0) {
      child_fail(exec_status.write_end(), errno);
    }
    if (::dup2(child_stdin.read_end(), STDIN_FILENO) < 0) {
      child_fail(exec_status.write_end(), errno);
    }
    clear_close_on_exec(STDIN_FILENO);
    if (child_stdout.read_end() >= 0) {
      if (::dup2(child_stdout.write_end(), STDOUT_FILENO) < 0) {
        child_fail(exec_status.write_end(), errno);
      }
      clear_close_on_exec(STDOUT_FILENO);
      if (options.capture_stderr) {
        if (::dup2(child_stdout.write_end(), STDERR_FILENO) < 0) {
          child_fail(exec_status.write_end(), errno);
        }
        clear_close_on_exec(STDERR_FILENO);
      }
    }
    if (g_sigpipe_ignored_by_us) {
      restore_default_sigpipe();
    }
    ::execv(options.program.c_str(), argv.data());
    child_fail(exec_status.write_end(), errno);
  }

  const int parent_stdin = child_stdin.release_write();
  child_stdin.close_ends();
  const int parent_stdout = child_stdout.read_end() >= 0 ? child_stdout.release_read() : -1;
  child_stdout.close_ends();
  const int status_read = exec_status.release_read();
  exec_status.close_ends();

  // The exec status pipe is close-on-exec, so it reaches end of file exactly
  // when the image has been replaced. Anything else is a failure between fork
  // and exec, which is reported with the errno the child saw.
  int child_errno = 0;
  ssize_t received = -1;
  for (;;) {
    received = ::read(status_read, &child_errno, sizeof(child_errno));
    if (received >= 0 || errno != EINTR) {
      break;
    }
  }
  const int status_error = received < 0 ? errno : 0;
  (void)::close(status_read);
  if (received != 0) {
    int status = 0;
    while (::waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {
    }
    (void)::close(parent_stdin);
    (void)::close(parent_stdout);
    if (received > 0) {
      const ErrorCode code = child_errno == ENOENT ? ErrorCode::not_found : ErrorCode::io_error;
      return os_error_code(code, "start " + quoted(options.program), child_errno);
    }
    return os_error_code(ErrorCode::io_error, "start " + quoted(options.program), status_error);
  }
  implementation->pid = child_pid;
  implementation->stdin_write = parent_stdin;
  implementation->stdout_read = parent_stdout;
#endif
  ChildProcess child;
  child.impl_ = std::move(implementation);
  return Result<ChildProcess>(std::move(child));
}

Result<std::uint64_t> ChildProcess::id() {
  if (!impl_ || !impl_->started()) {
    return Error{ErrorCode::closed, "the child process is not running"};
  }
#if defined(_WIN32)
  const DWORD process = ::GetProcessId(impl_->process);
  if (process == 0) {
    return win_error(ErrorCode::io_error, "read the child process identifier");
  }
  return static_cast<std::uint64_t>(process);
#else
  return static_cast<std::uint64_t>(static_cast<long long>(impl_->pid));
#endif
}

Result<bool> ChildProcess::running() {
  if (!impl_ || !impl_->started()) {
    return Error{ErrorCode::closed, "the child process is not running"};
  }
  if (impl_->exited) {
    return false;
  }
#if defined(_WIN32)
  DWORD code = 0;
  if (!::GetExitCodeProcess(impl_->process, &code)) {
    return win_error(ErrorCode::io_error, "read the child process state");
  }
  if (code == STILL_ACTIVE) {
    return true;
  }
  impl_->exited = true;
  impl_->exit_code = static_cast<int>(code);
  return false;
#else
  int status = 0;
  const pid_t reaped = ::waitpid(impl_->pid, &status, WNOHANG);
  if (reaped == 0) {
    return true;
  }
  if (reaped == impl_->pid) {
    impl_->exited = true;
    impl_->exit_code = decode_wait_status(status);
    return false;
  }
  if (errno == EINTR) {
    return true;
  }
  return Error{ErrorCode::indeterminate, "the child process state could not be read"};
#endif
}

Result<std::optional<std::string>> ChildProcess::read_line(std::size_t max_bytes) {
  if (!impl_ || !impl_->started()) {
    return Error{ErrorCode::closed, "read: the child process is not running"};
  }
  if (!impl_->has_output()) {
    return Error{ErrorCode::unsupported, "read: the child's standard output was not captured"};
  }
  // Two bytes of slack cover the carriage return and the terminator of a
  // CRLF line ending, which is a terminator and not part of the line's text.
  const std::size_t hard_cap = max_bytes > std::numeric_limits<std::size_t>::max() - 2
                                   ? std::numeric_limits<std::size_t>::max()
                                   : max_bytes + 2;
  for (;;) {
    const std::size_t newline = impl_->pending.find('\n');
    if (newline != std::string::npos) {
      if (newline > max_bytes && !(newline == max_bytes + 1 && impl_->pending[max_bytes] == '\r')) {
        return Error{ErrorCode::limit_exceeded, "read: the line is longer than the accepted bound"};
      }
      std::string line = impl_->pending.substr(0, newline);
      impl_->pending.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return std::optional<std::string>(std::move(line));
    }
    const std::size_t observed = (!impl_->pending.empty() && impl_->pending.back() == '\r')
                                     ? impl_->pending.size() - 1
                                     : impl_->pending.size();
    if (observed > max_bytes) {
      return Error{ErrorCode::limit_exceeded, "read: the line is longer than the accepted bound"};
    }
    if (impl_->output_ended) {
      if (impl_->pending.empty()) {
        return std::optional<std::string>{};
      }
      std::string line = std::move(impl_->pending);
      impl_->pending.clear();
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return std::optional<std::string>(std::move(line));
    }
    DCE_TRY(impl_->read_chunk(hard_cap - impl_->pending.size()));
  }
}

Status ChildProcess::write_line(std::string_view text) {
  if (!impl_ || !impl_->started()) {
    return Error{ErrorCode::closed, "write: the child process is not running"};
  }
  if (!impl_->has_input()) {
    return Error{ErrorCode::closed, "write: the child's standard input was not connected"};
  }
  std::string line(text);
  line.push_back('\n');
  return impl_->write_all(line);
}

Result<int> ChildProcess::wait() {
  if (!impl_ || !impl_->started()) {
    return Error{ErrorCode::closed, "wait: the child process is not running"};
  }
  if (impl_->exited) {
    return impl_->exit_code;
  }
#if defined(_WIN32)
  const DWORD waited = ::WaitForSingleObject(impl_->process, INFINITE);
  if (waited != WAIT_OBJECT_0) {
    return win_error(ErrorCode::io_error, "wait for the child process");
  }
  DWORD code = 0;
  if (!::GetExitCodeProcess(impl_->process, &code)) {
    return win_error(ErrorCode::io_error, "read the child process exit code");
  }
  impl_->exited = true;
  impl_->exit_code = static_cast<int>(code);
  return impl_->exit_code;
#else
  int status = 0;
  pid_t reaped = -1;
  for (;;) {
    reaped = ::waitpid(impl_->pid, &status, 0);
    if (reaped >= 0 || errno != EINTR) {
      break;
    }
  }
  if (reaped < 0) {
    return os_error(ErrorCode::io_error, "wait for the child process");
  }
  impl_->exited = true;
  impl_->exit_code = decode_wait_status(status);
  return impl_->exit_code;
#endif
}

Status ChildProcess::terminate() {
  if (!impl_ || !impl_->started()) {
    return Error{ErrorCode::closed, "terminate: the child process is not running"};
  }
  if (impl_->exited) {
    return Status{};
  }
#if defined(_WIN32)
  DWORD code = 0;
  if (::GetExitCodeProcess(impl_->process, &code) && code != STILL_ACTIVE) {
    impl_->exited = true;
    impl_->exit_code = static_cast<int>(code);
    return Status{};
  }
  if (!::TerminateProcess(impl_->process, 1)) {
    // The child may have exited between the two calls; that is the outcome the
    // caller asked for, so it is not reported as a failure.
    if (::GetExitCodeProcess(impl_->process, &code) && code != STILL_ACTIVE) {
      impl_->exited = true;
      impl_->exit_code = static_cast<int>(code);
      return Status{};
    }
    return win_error(ErrorCode::io_error, "terminate the child process");
  }
  return Status{};
#else
  int status = 0;
  if (::waitpid(impl_->pid, &status, WNOHANG) == impl_->pid) {
    impl_->exited = true;
    impl_->exit_code = decode_wait_status(status);
    return Status{};
  }
  if (::kill(impl_->pid, SIGKILL) != 0 && errno != ESRCH) {
    return os_error(ErrorCode::io_error, "terminate the child process");
  }
  return Status{};
#endif
}

// ---------------------------------------------------------------------------
// Loopback sockets
// ---------------------------------------------------------------------------
namespace {

std::once_flag g_networking_once;
Status g_networking_status;

#if defined(_WIN32)
[[nodiscard]] int last_socket_error() noexcept { return ::WSAGetLastError(); }

void close_socket(SOCKET handle) noexcept {
  if (handle != INVALID_SOCKET) {
    (void)::closesocket(handle);
  }
}

[[nodiscard]] Error socket_error(ErrorCode code, std::string_view context, int error_number) {
  return win_error_code(code, context, static_cast<DWORD>(error_number));
}

[[nodiscard]] bool is_interrupted(int error_number) noexcept { return error_number == WSAEINTR; }

[[nodiscard]] bool is_address_in_use(int error_number) noexcept { return error_number == WSAEADDRINUSE; }

[[nodiscard]] bool is_peer_gone(int error_number) noexcept {
  return error_number == WSAECONNRESET || error_number == WSAECONNABORTED || error_number == WSAENOTCONN ||
         error_number == WSAESHUTDOWN;
}

using socket_length = int;
#else
[[nodiscard]] int last_socket_error() noexcept { return errno; }

void close_socket(int handle) noexcept {
  if (handle >= 0) {
    (void)::close(handle);
  }
}

[[nodiscard]] Error socket_error(ErrorCode code, std::string_view context, int error_number) {
  return os_error_code(code, context, error_number);
}

[[nodiscard]] bool is_interrupted(int error_number) noexcept { return error_number == EINTR; }

[[nodiscard]] bool is_address_in_use(int error_number) noexcept { return error_number == EADDRINUSE; }

[[nodiscard]] bool is_peer_gone(int error_number) noexcept {
  return error_number == ECONNRESET || error_number == ECONNABORTED || error_number == ENOTCONN ||
         error_number == EPIPE;
}

using socket_length = socklen_t;
#endif

}  // namespace

Status initialize_networking() {
  std::call_once(g_networking_once, [] {
#if defined(_WIN32)
    // 2.2 is the Winsock version every supported Windows release provides.
    WSADATA data{};
    if (::WSAStartup(0x0202, &data) != 0) {
      g_networking_status = Error{ErrorCode::io_error, "WSAStartup failed"};
    }
#endif
  });
  return g_networking_status;
}

struct Socket::Impl {
#if defined(_WIN32)
  SOCKET handle{INVALID_SOCKET};

  [[nodiscard]] bool opened() const noexcept { return handle != INVALID_SOCKET; }
  bool close_native() noexcept {
    if (handle == INVALID_SOCKET) {
      return true;
    }
    const SOCKET closing = handle;
    handle = INVALID_SOCKET;
    return ::closesocket(closing) == 0;
  }
#else
  int handle{-1};

  [[nodiscard]] bool opened() const noexcept { return handle >= 0; }
  bool close_native() noexcept {
    if (handle < 0) {
      return true;
    }
    const int closing = handle;
    handle = -1;
    return ::close(closing) == 0;
  }
#endif
  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() { close_native(); }
};

Socket::Socket(Socket&& other) noexcept : impl_(std::move(other.impl_)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

Socket::Socket() noexcept = default;

Socket::~Socket() = default;

Result<Socket> Socket::listen_loopback(std::uint16_t port) {
  DCE_TRY(initialize_networking());
  auto implementation = std::make_unique<Impl>();
#if defined(_WIN32)
  const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == INVALID_SOCKET) {
    return socket_error(ErrorCode::io_error, "create a listening socket", last_socket_error());
  }
#else
  const int handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle < 0) {
    return socket_error(ErrorCode::io_error, "create a listening socket", errno);
  }
  // Releasing a loopback port and binding it again must not depend on the
  // peer's TIME_WAIT state. Windows deliberately does not get this option:
  // SO_REUSEADDR there also lets a second listener take a port that is already
  // bound, which would hide exactly the conflict a caller wants to observe.
  const int reuse = 1;
  (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, static_cast<socket_length>(sizeof(reuse)));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  // Loopback only: a listener that accepted traffic from anywhere would make
  // the isolation the suites assert unobservable.
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), static_cast<socket_length>(sizeof(address))) != 0) {
    const int error_number = last_socket_error();
    close_socket(handle);
    const ErrorCode code = is_address_in_use(error_number) ? ErrorCode::busy : ErrorCode::io_error;
    return socket_error(code, "bind loopback port " + std::to_string(port), error_number);
  }
  if (::listen(handle, SOMAXCONN) != 0) {
    const int error_number = last_socket_error();
    close_socket(handle);
    return socket_error(ErrorCode::io_error, "listen on loopback port " + std::to_string(port), error_number);
  }
  implementation->handle = handle;
  Socket listener;
  listener.impl_ = std::move(implementation);
  return Result<Socket>(std::move(listener));
}

Result<Socket> Socket::connect_loopback(std::uint16_t port) {
  DCE_TRY(initialize_networking());
  auto implementation = std::make_unique<Impl>();
#if defined(_WIN32)
  const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == INVALID_SOCKET) {
    return socket_error(ErrorCode::io_error, "create a socket", last_socket_error());
  }
#else
  ensure_sigpipe_ignored();
  const int handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle < 0) {
    return socket_error(ErrorCode::io_error, "create a socket", errno);
  }
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(handle, reinterpret_cast<const sockaddr*>(&address), static_cast<socket_length>(sizeof(address))) !=
      0) {
    const int error_number = last_socket_error();
    close_socket(handle);
    return socket_error(ErrorCode::io_error, "connect to loopback port " + std::to_string(port), error_number);
  }
  implementation->handle = handle;
  Socket peer;
  peer.impl_ = std::move(implementation);
  return Result<Socket>(std::move(peer));
}

Result<std::uint16_t> Socket::local_port() {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "the socket is closed"};
  }
  DCE_TRY(initialize_networking());
  sockaddr_in address{};
  socket_length length = static_cast<socket_length>(sizeof(address));
  if (::getsockname(impl_->handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return socket_error(ErrorCode::io_error, "read the local port", last_socket_error());
  }
  return static_cast<std::uint16_t>(ntohs(address.sin_port));
}

Result<Socket> Socket::accept() {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "the socket is closed"};
  }
  DCE_TRY(initialize_networking());
  auto implementation = std::make_unique<Impl>();
#if defined(_WIN32)
  const SOCKET handle = ::accept(impl_->handle, nullptr, nullptr);
  if (handle == INVALID_SOCKET) {
    return socket_error(ErrorCode::io_error, "accept a loopback connection", last_socket_error());
  }
#else
  const int handle = ::accept(impl_->handle, nullptr, nullptr);
  if (handle < 0) {
    return socket_error(ErrorCode::io_error, "accept a loopback connection", errno);
  }
  // accept() does not inherit the close-on-exec flag of the listener.
  clear_close_on_exec(handle);
#endif
  implementation->handle = handle;
  Socket connection;
  connection.impl_ = std::move(implementation);
  return Result<Socket>(std::move(connection));
}

Status Socket::send_all(std::span<const std::byte> data) {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "send: the socket is closed"};
  }
  DCE_TRY(initialize_networking());
#if !defined(_WIN32)
  ensure_sigpipe_ignored();
#endif
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
#if defined(_WIN32)
    const int chunk = static_cast<int>(
        std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int moved = ::send(impl_->handle, reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
#else
    const std::size_t chunk = std::min<std::size_t>(remaining, kIoChunk);
    const ssize_t moved = ::send(impl_->handle, data.data() + sent, chunk, kSendFlags);
#endif
    if (moved < 0) {
      const int error_number = last_socket_error();
      if (is_interrupted(error_number)) {
        continue;
      }
      const ErrorCode code = is_peer_gone(error_number) ? ErrorCode::closed : ErrorCode::io_error;
      return socket_error(code, "send on the socket", error_number);
    }
    if (moved == 0) {
      return Error{ErrorCode::closed, "send: the peer closed the connection"};
    }
    sent += static_cast<std::size_t>(moved);
  }
  return Status{};
}

Result<std::size_t> Socket::recv_some(std::span<std::byte> buffer) {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "receive: the socket is closed"};
  }
  if (buffer.empty()) {
    return std::size_t{0};
  }
  DCE_TRY(initialize_networking());
  for (;;) {
#if defined(_WIN32)
    const int chunk = static_cast<int>(
        std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int moved = ::recv(impl_->handle, reinterpret_cast<char*>(buffer.data()), chunk, 0);
#else
    const std::size_t chunk = std::min<std::size_t>(buffer.size(), kIoChunk);
    const ssize_t moved = ::recv(impl_->handle, buffer.data(), chunk, 0);
#endif
    if (moved > 0) {
      return static_cast<std::size_t>(moved);
    }
    if (moved == 0) {
      return Error{ErrorCode::closed, "the peer closed the connection"};
    }
    const int error_number = last_socket_error();
    if (is_interrupted(error_number)) {
      continue;
    }
    if (is_peer_gone(error_number)) {
      return socket_error(ErrorCode::closed, "the peer closed the connection", error_number);
    }
    return socket_error(ErrorCode::io_error, "receive on the socket", error_number);
  }
}

Status Socket::recv_exact(std::span<std::byte> buffer) {
  std::size_t received = 0;
  while (received < buffer.size()) {
    DCE_ASSIGN(moved, recv_some(buffer.subspan(received)));
    if (moved == 0) {
      return Error{ErrorCode::closed, "the peer closed the connection before the message was complete"};
    }
    received += moved;
  }
  return Status{};
}

Status Socket::set_no_delay(bool enabled) {
  if (!impl_ || !impl_->opened()) {
    return Error{ErrorCode::closed, "the socket is closed"};
  }
  DCE_TRY(initialize_networking());
  const int value = enabled ? 1 : 0;
  if (::setsockopt(impl_->handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value),
                   static_cast<socket_length>(sizeof(value))) != 0) {
    return socket_error(ErrorCode::io_error, "set TCP_NODELAY", last_socket_error());
  }
  return Status{};
}

Status Socket::shutdown_both() {
  if (!impl_ || !impl_->opened()) {
    return Status{};
  }
  DCE_TRY(initialize_networking());
#if defined(_WIN32)
  if (::shutdown(impl_->handle, SD_BOTH) != 0) {
    const int error_number = last_socket_error();
    if (error_number == WSAENOTCONN || error_number == WSAECONNRESET || error_number == WSAECONNABORTED) {
      return Status{};
    }
    return socket_error(ErrorCode::io_error, "shut the socket down", error_number);
  }
#else
  if (::shutdown(impl_->handle, SHUT_RDWR) != 0) {
    const int error_number = errno;
    if (error_number == ENOTCONN || error_number == ECONNRESET || error_number == ECONNABORTED) {
      return Status{};
    }
    return socket_error(ErrorCode::io_error, "shut the socket down", error_number);
  }
#endif
  return Status{};
}

Status Socket::close() {
  if (!impl_) {
    return Status{};
  }
  if (!impl_->close_native()) {
    return Error{ErrorCode::io_error, "close the socket"};
  }
  return Status{};
}

bool Socket::is_open() const noexcept { return impl_ != nullptr && impl_->opened(); }

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
std::uint64_t monotonic_nanos() noexcept {
#if defined(_WIN32)
  static const std::uint64_t frequency = [] {
    LARGE_INTEGER value{};
    if (!::QueryPerformanceFrequency(&value) || value.QuadPart <= 0) {
      return std::uint64_t{0};
    }
    return static_cast<std::uint64_t>(value.QuadPart);
  }();
  LARGE_INTEGER counter{};
  if (frequency == 0 || !::QueryPerformanceCounter(&counter) || counter.QuadPart < 0) {
    return 0;
  }
  const std::uint64_t ticks = static_cast<std::uint64_t>(counter.QuadPart);
  const std::uint64_t seconds = ticks / frequency;
  const std::uint64_t remainder = ticks % frequency;
  return seconds * 1000000000ULL + (remainder * 1000000000ULL) / frequency;
#else
  timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return 0;
  }
  const std::uint64_t seconds = static_cast<std::uint64_t>(value.tv_sec);
  const std::uint64_t nanos = static_cast<std::uint64_t>(value.tv_nsec);
  return seconds * 1000000000ULL + nanos;
#endif
}

}  // namespace dce::platform
