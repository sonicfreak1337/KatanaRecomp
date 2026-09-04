#include "host_build_tool.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace katana::cli {
namespace {

inline constexpr std::string_view event_header =
    "KATANA_HOST_BUILD_EVENT_V1";

[[nodiscard]] bool source_argument(
    std::string_view argument) noexcept {
    if (argument.size() >= 2u && argument.front() == '"' &&
        argument.back() == '"') {
        argument.remove_prefix(1u);
        argument.remove_suffix(1u);
    }
    const auto slash = argument.find_last_of("/\\");
    const auto dot = argument.find_last_of('.');
    if (dot == std::string_view::npos ||
        (slash != std::string_view::npos && dot < slash))
        return false;
    auto extension = std::string(argument.substr(dot));
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return extension == ".c" || extension == ".cc" ||
           extension == ".cpp" || extension == ".cxx" ||
           extension == ".c++";
}

[[nodiscard]] std::uint64_t invocation_units(
    const HostBuildToolKind kind,
    const std::span<const char* const> arguments) noexcept {
    if (kind != HostBuildToolKind::Compile) return 1u;
    const auto explicit_sources = static_cast<std::uint64_t>(
        std::count_if(
            arguments.begin(), arguments.end(),
            [](const auto* argument) {
                return argument != nullptr && source_argument(argument);
            }));
    return std::max<std::uint64_t>(1u, explicit_sources);
}

[[nodiscard]] std::filesystem::path unique_event_path(
    const std::filesystem::path& root,
    const HostBuildToolKind kind) {
    static std::atomic<std::uint64_t> sequence{0u};
#ifdef _WIN32
    const auto process =
        static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process = static_cast<std::uint64_t>(::getpid());
#endif
    const auto clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return root /
           ("event-" + std::to_string(process) + '-' +
            std::to_string(clock) + '-' +
            std::to_string(sequence.fetch_add(
                1u, std::memory_order_relaxed)) + '-' +
            std::string(host_build_tool_kind_name(kind)));
}

// Host-tool events are best-effort telemetry, not build authority. Closing
// the file publishes its bytes to the OS cache; forcing durable media for
// every compile edge caused hundreds of serialized storage barriers.
[[nodiscard]] bool write_new_event(
    const std::filesystem::path& path,
    const HostBuildToolKind kind,
    const std::uint64_t units) noexcept {
    try {
        const auto document =
            std::string(event_header) + "\nkind=" +
            std::string(host_build_tool_kind_name(kind)) +
            "\nunits=" + std::to_string(units) + "\n";
#ifdef _WIN32
        const auto handle = CreateFileW(
            path.c_str(),
            GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        BY_HANDLE_FILE_INFORMATION identity{};
        DWORD written = 0u;
        const auto valid =
            GetFileType(handle) == FILE_TYPE_DISK &&
            GetFileInformationByHandle(handle, &identity) &&
            GetFileInformationByHandleEx(
                handle, FileAttributeTagInfo, &attributes,
                sizeof(attributes)) &&
            (attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY |
              FILE_ATTRIBUTE_REPARSE_POINT)) == 0u &&
            identity.nNumberOfLinks == 1u &&
            document.size() <= std::numeric_limits<DWORD>::max() &&
            WriteFile(
                handle,
                document.data(),
                static_cast<DWORD>(document.size()),
                &written,
                nullptr) &&
            written == document.size();
        static_cast<void>(CloseHandle(handle));
        return valid;
#else
        auto flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const auto descriptor = ::open(path.c_str(), flags, 0600);
        if (descriptor < 0) return false;
        struct stat information {};
        auto valid =
            ::fstat(descriptor, &information) == 0 &&
            S_ISREG(information.st_mode) && information.st_nlink == 1;
        std::size_t offset = 0u;
        while (valid && offset < document.size()) {
            const auto written = ::write(
                descriptor,
                document.data() + offset,
                document.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                valid = false;
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
        valid = ::close(descriptor) == 0 && valid;
        return valid;
#endif
    } catch (...) {
        return false;
    }
}

[[nodiscard]] int run_tool(
    const std::string& tool,
    const std::span<const char* const> arguments) noexcept {
    try {
#ifdef _WIN32
        const auto quote_windows_command_line_argument =
            [](const std::string_view argument) {
                const auto needs_quotes = argument.empty() ||
                    std::any_of(
                        argument.begin(), argument.end(),
                        [](const char character) {
                            return std::isspace(
                                       static_cast<unsigned char>(
                                           character)) != 0 ||
                                   character == '"';
                        });
                if (!needs_quotes) return std::string(argument);

                std::string quoted;
                quoted.reserve(argument.size() + 2u);
                quoted.push_back('"');
                std::size_t backslashes = 0u;
                for (const char character : argument) {
                    if (character == '\\') {
                        ++backslashes;
                        continue;
                    }
                    if (character == '"') {
                        quoted.append(backslashes * 2u + 1u, '\\');
                        quoted.push_back('"');
                        backslashes = 0u;
                        continue;
                    }
                    quoted.append(backslashes, '\\');
                    quoted.push_back(character);
                    backslashes = 0u;
                }
                quoted.append(backslashes * 2u, '\\');
                quoted.push_back('"');
                return quoted;
            };
        std::vector<std::string> serialized_arguments;
        serialized_arguments.reserve(arguments.size() + 1u);
        serialized_arguments.push_back(
            quote_windows_command_line_argument(tool));
        for (const auto* argument : arguments) {
            if (argument == nullptr) return 127;
            serialized_arguments.push_back(
                quote_windows_command_line_argument(argument));
        }
        std::vector<const char*> child_arguments;
        child_arguments.reserve(serialized_arguments.size() + 1u);
        for (const auto& argument : serialized_arguments)
            child_arguments.push_back(argument.c_str());
        child_arguments.push_back(nullptr);
        const auto result = _spawnvp(
            _P_WAIT, tool.c_str(), child_arguments.data());
        return result < 0 ? 127 : static_cast<int>(result);
#else
        std::vector<const char*> child_arguments;
        child_arguments.reserve(arguments.size() + 2u);
        child_arguments.push_back(tool.c_str());
        child_arguments.insert(
            child_arguments.end(), arguments.begin(), arguments.end());
        child_arguments.push_back(nullptr);
        std::vector<char*> mutable_arguments;
        mutable_arguments.reserve(child_arguments.size());
        for (const auto* argument : child_arguments)
            mutable_arguments.push_back(const_cast<char*>(argument));
        pid_t child = -1;
        const auto spawn_result = ::posix_spawnp(
            &child, tool.c_str(), nullptr, nullptr,
            mutable_arguments.data(), environ);
        if (spawn_result != 0) return 127;
        int status = 0;
        while (::waitpid(child, &status, 0) < 0) {
            if (errno == EINTR) continue;
            return 127;
        }
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 127;
#endif
    } catch (...) {
        return 127;
    }
}

[[nodiscard]] bool safe_event_root(
    const std::filesystem::path& event_root) noexcept {
    try {
        if (event_root.empty()) return false;
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(event_root, status_error);
        if (status_error || !std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status))
            return false;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(event_root.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u;
#else
        return true;
#endif
    } catch (...) {
        return false;
    }
}

} // namespace

int run_host_build_tool_launcher(
    const HostBuildToolKind kind,
    const std::filesystem::path& event_root,
    std::string tool,
    const std::span<const char* const> arguments) noexcept {
    if (tool.empty()) return 127;

    std::optional<std::filesystem::path> started;
    std::optional<std::filesystem::path> base;
    if (safe_event_root(event_root)) {
        try {
            base = unique_event_path(event_root, kind);
            started = std::filesystem::path(base->string() + ".started");
            if (!write_new_event(
                    *started, kind, invocation_units(kind, arguments))) {
                started.reset();
                base.reset();
            }
        } catch (...) {
            started.reset();
            base.reset();
        }
    }

    const auto result = run_tool(tool, arguments);
    if (started && base) {
        try {
            const auto terminal = std::filesystem::path(
                base->string() +
                (result == 0 ? ".committed" : ".failed"));
            std::error_code ignored;
            std::filesystem::rename(*started, terminal, ignored);
        } catch (...) {
        }
    }
    return result;
}

std::string_view host_build_tool_kind_name(
    const HostBuildToolKind kind) noexcept {
    switch (kind) {
    case HostBuildToolKind::Compile:
        return "compile";
    case HostBuildToolKind::Archive:
        return "archive";
    case HostBuildToolKind::Link:
        return "link";
    }
    return "unknown";
}

} // namespace katana::cli
