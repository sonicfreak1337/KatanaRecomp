#include "port_build_telemetry.hpp"

#include "katana/cli/exit_code.hpp"
#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <intrin.h>
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#endif
#endif

namespace katana::cli {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t minimum_pending_records = 8u;
constexpr std::size_t maximum_pending_records = 65'536u;
constexpr std::size_t minimum_record_bytes = 4u * 1'024u;
constexpr std::size_t maximum_record_bytes = 1024u * 1'024u;
constexpr std::size_t maximum_progress_label_bytes = 2u * 1'024u;

[[nodiscard]] std::uint64_t saturating_add(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

void append_unsigned(std::string& output, const std::uint64_t value) {
    std::array<char, 32u> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error == std::errc{}) output.append(buffer.data(), end);
}

void append_signed(std::string& output, const int value) {
    std::array<char, 32u> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error == std::errc{}) output.append(buffer.data(), end);
}

void append_json_string(
    std::string& output,
    const std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    output.push_back('"');
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20u) {
                output += "\\u00";
                output.push_back(hex[byte >> 4u]);
                output.push_back(hex[byte & 0x0fu]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_optional_unsigned(
    std::string& output,
    const std::optional<std::uint64_t> value) {
    if (value)
        append_unsigned(output, *value);
    else
        output += "null";
}

[[nodiscard]] bool path_like(const std::string_view value) noexcept {
    if (value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos)
        return true;
    return value.size() >= 2u &&
           ((value[0] >= 'A' && value[0] <= 'Z') ||
            (value[0] >= 'a' && value[0] <= 'z')) &&
           value[1] == ':';
}

[[nodiscard]] bool guest_address_like(
    const std::string_view value) noexcept {
    for (std::size_t index = 0u;
         index + 2u < value.size();
         ++index) {
        if (value[index] != '0' ||
            (value[index + 1u] != 'x' &&
             value[index + 1u] != 'X'))
            continue;
        std::size_t hexadecimal_digits = 0u;
        for (auto cursor = index + 2u;
             cursor < value.size();
             ++cursor) {
            const auto byte =
                static_cast<unsigned char>(value[cursor]);
            if (!((byte >= '0' && byte <= '9') ||
                  (byte >= 'a' && byte <= 'f') ||
                  (byte >= 'A' && byte <= 'F')))
                break;
            ++hexadecimal_digits;
        }
        if (hexadecimal_digits >= 8u) return true;
    }
    return false;
}

[[nodiscard]] std::string safe_display_label(
    const std::string_view value,
    const std::string_view fallback,
    const std::size_t maximum_size = 160u) {
    if (value.empty()) return std::string(fallback);
    if (value.size() > maximum_size || path_like(value))
        return std::string(fallback);
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20u || byte == 0x7fu)
            return std::string(fallback);
    }
    return std::string(value);
}

#ifdef _WIN32
[[nodiscard]] bool windows_reserved_disk_filename(
    const std::filesystem::path& path) noexcept {
    try {
        auto filename = path.filename().native();
        if (filename.empty() ||
            filename.find(L':') != std::wstring::npos)
            return true;
        while (!filename.empty() &&
               (filename.back() == L'.' ||
                filename.back() == L' '))
            filename.pop_back();
        if (filename.empty()) return true;
        const auto extension = filename.find(L'.');
        auto base = filename.substr(0u, extension);
        while (!base.empty() && base.back() == L' ')
            base.pop_back();
        std::transform(
            base.begin(),
            base.end(),
            base.begin(),
            [](const wchar_t character) {
                return character >= L'a' &&
                               character <= L'z'
                           ? static_cast<wchar_t>(
                                 character -
                                 (L'a' - L'A'))
                           : character;
            });
        if (base == L"CON" || base == L"PRN" ||
            base == L"AUX" || base == L"NUL" ||
            base == L"CONIN$" || base == L"CONOUT$")
            return true;
        return base.size() == 4u &&
               ((base.starts_with(L"COM") ||
                 base.starts_with(L"LPT")) &&
                base[3] >= L'1' && base[3] <= L'9');
    } catch (...) {
        return true;
    }
}

enum class WindowsRelativeOpenResult : std::uint8_t {
    Opened,
    NameCollision,
    NotFound,
    Failed,
};

[[nodiscard]] WindowsRelativeOpenResult
open_windows_directory_child(
    const HANDLE parent,
    const std::filesystem::path& name,
    const ACCESS_MASK desired_access,
    const ULONG share_access,
    const ULONG disposition,
    HANDLE& result) noexcept {
    result = INVALID_HANDLE_VALUE;
    try {
        if (parent == INVALID_HANDLE_VALUE ||
            name.empty() || name.has_parent_path() ||
            name == L"." || name == L"..")
            return WindowsRelativeOpenResult::Failed;
        const auto& native_name = name.native();
        const auto byte_count =
            native_name.size() * sizeof(wchar_t);
        if (byte_count == 0u ||
            byte_count >
                std::numeric_limits<USHORT>::max())
            return WindowsRelativeOpenResult::Failed;

        const auto module = GetModuleHandleW(L"ntdll.dll");
        if (module == nullptr)
            return WindowsRelativeOpenResult::Failed;
        const auto address =
            GetProcAddress(module, "NtCreateFile");
        if (address == nullptr)
            return WindowsRelativeOpenResult::Failed;
        using NtCreateFileFunction = decltype(&NtCreateFile);
        NtCreateFileFunction nt_create_file = nullptr;
        static_assert(
            sizeof(nt_create_file) == sizeof(address));
        std::memcpy(
            &nt_create_file, &address, sizeof(address));

        UNICODE_STRING unicode_name{};
        unicode_name.Length =
            static_cast<USHORT>(byte_count);
        unicode_name.MaximumLength = unicode_name.Length;
        unicode_name.Buffer = const_cast<PWSTR>(
            native_name.data());
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = parent;
        attributes.ObjectName = &unicode_name;
        attributes.Attributes = OBJ_CASE_INSENSITIVE;
        IO_STATUS_BLOCK status_block{};
        HANDLE opened = INVALID_HANDLE_VALUE;
        const auto status = nt_create_file(
            &opened,
            desired_access,
            &attributes,
            &status_block,
            nullptr,
            FILE_ATTRIBUTE_NORMAL,
            share_access,
            disposition,
            FILE_NON_DIRECTORY_FILE |
                FILE_SYNCHRONOUS_IO_NONALERT |
                FILE_OPEN_REPARSE_POINT,
            nullptr,
            0u);
        constexpr NTSTATUS name_collision =
            static_cast<NTSTATUS>(0xC0000035L);
        constexpr NTSTATUS name_not_found =
            static_cast<NTSTATUS>(0xC0000034L);
        constexpr NTSTATUS path_not_found =
            static_cast<NTSTATUS>(0xC000003AL);
        if (status == name_collision)
            return WindowsRelativeOpenResult::NameCollision;
        if (status == name_not_found || status == path_not_found)
            return WindowsRelativeOpenResult::NotFound;
        if (status < 0 || opened == INVALID_HANDLE_VALUE) {
            if (opened != INVALID_HANDLE_VALUE)
                static_cast<void>(CloseHandle(opened));
            return WindowsRelativeOpenResult::Failed;
        }
        result = opened;
        return WindowsRelativeOpenResult::Opened;
    } catch (...) {
        return WindowsRelativeOpenResult::Failed;
    }
}

[[nodiscard]] bool same_windows_file_identity(
    const BY_HANDLE_FILE_INFORMATION& left,
    const BY_HANDLE_FILE_INFORMATION& right) noexcept {
    return left.dwVolumeSerialNumber ==
               right.dwVolumeSerialNumber &&
           left.nFileIndexHigh == right.nFileIndexHigh &&
           left.nFileIndexLow == right.nFileIndexLow;
}
#endif

[[nodiscard]] std::string safe_identifier(
    const std::string_view value,
    const std::string_view fallback) {
    if (value.empty() || value.size() > 64u) return std::string(fallback);
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9') || byte == '-' ||
              byte == '_' || byte == '.'))
            return std::string(fallback);
    }
    return std::string(value);
}

[[nodiscard]] std::optional<std::string>
environment_value(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0u;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        return std::nullopt;
    std::string result(value);
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

[[nodiscard]] std::optional<std::uint64_t>
parse_unsigned_decimal(const std::string_view value) noexcept {
    if (value.empty()) return std::nullopt;
    std::uint64_t result = 0u;
    const auto conversion = std::from_chars(
        value.data(), value.data() + value.size(), result, 10);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != value.data() + value.size())
        return std::nullopt;
    return result;
}

struct HostInformation final {
    std::string os_family = "unknown";
    std::string os_version = "unknown";
    std::string architecture = "unknown";
    std::string cpu_model = "unknown";
    std::uint64_t physical_cores = 0u;
    std::uint64_t logical_processors = 0u;
    std::uint64_t ram_bytes = 0u;
};

[[nodiscard]] std::string compiled_toolchain() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER) + "." +
           std::to_string(_MSC_FULL_VER);
#elif defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." +
           std::to_string(__clang_minor__) + "." +
           std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." +
           std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string compiled_build_profile() {
#ifdef NDEBUG
    return "release-like";
#else
    return "debug-like";
#endif
}

#ifdef _WIN32

[[nodiscard]] std::string windows_architecture(
    const WORD architecture) {
    switch (architecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x86_64";
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
    default:
        return "unknown";
    }
}

[[nodiscard]] std::string windows_cpu_brand() {
#if defined(_M_IX86) || defined(_M_X64)
    std::array<int, 4u> registers{};
    __cpuid(registers.data(), static_cast<int>(0x80000000u));
    const auto maximum_leaf = static_cast<std::uint32_t>(registers[0]);
    if (maximum_leaf < 0x80000004u) return "unknown";
    std::array<char, 49u> brand{};
    for (std::uint32_t leaf = 0u; leaf < 3u; ++leaf) {
        __cpuid(
            registers.data(),
            static_cast<int>(0x80000002u + leaf));
        std::memcpy(
            brand.data() + leaf * 16u,
            registers.data(),
            16u);
    }
    std::string result(brand.data());
    const auto first = result.find_first_not_of(' ');
    const auto last = result.find_last_not_of(' ');
    if (first == std::string::npos) return "unknown";
    return result.substr(first, last - first + 1u);
#else
    const auto identifier = environment_value("PROCESSOR_IDENTIFIER");
    return identifier
               ? safe_display_label(*identifier, "unknown")
               : std::string("unknown");
#endif
}

[[nodiscard]] HostInformation inspect_host() {
    HostInformation result;
    result.os_family = "Windows";
    using RtlGetVersionFunction =
        LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto address =
        ntdll == nullptr
            ? nullptr
            : GetProcAddress(ntdll, "RtlGetVersion");
    RtlGetVersionFunction rtl_get_version = nullptr;
    static_assert(
        sizeof(rtl_get_version) == sizeof(address));
    std::memcpy(
        &rtl_get_version, &address, sizeof(address));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version != nullptr &&
        rtl_get_version(&version) == 0) {
        result.os_version =
            std::to_string(version.dwMajorVersion) + "." +
            std::to_string(version.dwMinorVersion) + "." +
            std::to_string(version.dwBuildNumber);
    }
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    result.architecture =
        windows_architecture(system.wProcessorArchitecture);
    result.logical_processors =
        static_cast<std::uint64_t>(
            GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    result.cpu_model =
        safe_display_label(windows_cpu_brand(), "unknown", 192u);

    DWORD bytes = 0u;
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore, nullptr, &bytes) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0u &&
        bytes <= 16u * 1'024u * 1'024u) {
        std::vector<std::byte> buffer(bytes);
        if (GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<
                    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    buffer.data()),
                &bytes)) {
            std::size_t offset = 0u;
            constexpr auto information_header_bytes =
                sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) +
                sizeof(DWORD);
            while (offset + information_header_bytes <= bytes) {
                const auto* information =
                    reinterpret_cast<
                        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        buffer.data() + offset);
                if (information->Size < information_header_bytes ||
                    offset + information->Size > bytes)
                    break;
                if (information->Relationship ==
                    RelationProcessorCore)
                    ++result.physical_cores;
                offset += information->Size;
            }
        }
    }
    if (result.physical_cores == 0u)
        result.physical_cores = result.logical_processors;

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        result.ram_bytes =
            static_cast<std::uint64_t>(memory.ullTotalPhys);
    return result;
}

#else

[[nodiscard]] std::string trimmed(
    const std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1u));
}

[[nodiscard]] HostInformation inspect_host() {
    HostInformation result;
    struct utsname system {};
    if (::uname(&system) == 0) {
        result.os_family =
            safe_display_label(system.sysname, "unknown");
        result.os_version =
            safe_display_label(system.release, "unknown");
        result.architecture =
            safe_display_label(system.machine, "unknown");
    }
    const auto logical = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (logical > 0)
        result.logical_processors =
            static_cast<std::uint64_t>(logical);
    const auto pages = ::sysconf(_SC_PHYS_PAGES);
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        const auto maximum =
            std::numeric_limits<std::uint64_t>::max();
        const auto page_count =
            static_cast<std::uint64_t>(pages);
        const auto bytes_per_page =
            static_cast<std::uint64_t>(page_size);
        result.ram_bytes =
            page_count > maximum / bytes_per_page
                ? maximum
                : page_count * bytes_per_page;
    }

    std::ifstream cpu_info("/proc/cpuinfo");
    std::string line;
    std::string physical_id;
    std::string core_id;
    std::unordered_set<std::string> cores;
    const auto finish_processor = [&]() {
        if (!physical_id.empty() && !core_id.empty())
            cores.insert(physical_id + ":" + core_id);
        physical_id.clear();
        core_id.clear();
    };
    while (std::getline(cpu_info, line)) {
        if (line.empty()) {
            finish_processor();
            continue;
        }
        const auto delimiter = line.find(':');
        if (delimiter == std::string::npos) continue;
        const auto key = trimmed(
            std::string_view(line).substr(0u, delimiter));
        const auto value = trimmed(
            std::string_view(line).substr(delimiter + 1u));
        if ((key == "model name" || key == "Hardware") &&
            result.cpu_model == "unknown")
            result.cpu_model =
                safe_display_label(value, "unknown", 192u);
        else if (key == "physical id")
            physical_id = value;
        else if (key == "core id")
            core_id = value;
    }
    finish_processor();
    result.physical_cores =
        cores.empty()
            ? result.logical_processors
            : static_cast<std::uint64_t>(cores.size());
    return result;
}

#endif

struct ResourceValues final {
    bool cpu_available = false;
    bool memory_available = false;
    bool faults_available = false;
    bool io_available = false;
    bool io_blocks_available = false;
    bool processes_available = false;
    std::uint64_t user_cpu_ms = 0u;
    std::uint64_t kernel_cpu_ms = 0u;
    std::uint64_t working_set_bytes = 0u;
    std::uint64_t working_set_peak_bytes = 0u;
    std::uint64_t private_commit_bytes = 0u;
    std::uint64_t private_commit_peak_bytes = 0u;
    std::uint64_t page_faults = 0u;
    std::uint64_t io_read_bytes = 0u;
    std::uint64_t io_write_bytes = 0u;
    std::uint64_t io_other_bytes = 0u;
    std::uint64_t io_read_operations = 0u;
    std::uint64_t io_write_operations = 0u;
    std::uint64_t io_other_operations = 0u;
    std::uint64_t io_input_blocks = 0u;
    std::uint64_t io_output_blocks = 0u;
    std::uint64_t active_processes = 0u;
};

void add_resource_values(
    ResourceValues& destination,
    const ResourceValues& source,
    const bool include_current_values = true) noexcept {
    if (source.cpu_available) {
        destination.cpu_available = true;
        destination.user_cpu_ms = saturating_add(
            destination.user_cpu_ms, source.user_cpu_ms);
        destination.kernel_cpu_ms = saturating_add(
            destination.kernel_cpu_ms, source.kernel_cpu_ms);
    }
    if (source.memory_available) {
        destination.memory_available = true;
        if (include_current_values) {
            destination.working_set_bytes = saturating_add(
                destination.working_set_bytes,
                source.working_set_bytes);
            destination.private_commit_bytes = saturating_add(
                destination.private_commit_bytes,
                source.private_commit_bytes);
        }
        destination.working_set_peak_bytes = std::max(
            destination.working_set_peak_bytes,
            source.working_set_peak_bytes);
        destination.private_commit_peak_bytes = std::max(
            destination.private_commit_peak_bytes,
            source.private_commit_peak_bytes);
    }
    if (source.faults_available) {
        destination.faults_available = true;
        destination.page_faults = saturating_add(
            destination.page_faults, source.page_faults);
    }
    if (source.io_available) {
        destination.io_available = true;
        destination.io_read_bytes = saturating_add(
            destination.io_read_bytes, source.io_read_bytes);
        destination.io_write_bytes = saturating_add(
            destination.io_write_bytes, source.io_write_bytes);
        destination.io_other_bytes = saturating_add(
            destination.io_other_bytes, source.io_other_bytes);
        destination.io_read_operations = saturating_add(
            destination.io_read_operations,
            source.io_read_operations);
        destination.io_write_operations = saturating_add(
            destination.io_write_operations,
            source.io_write_operations);
        destination.io_other_operations = saturating_add(
            destination.io_other_operations,
            source.io_other_operations);
    }
    if (source.io_blocks_available) {
        destination.io_blocks_available = true;
        destination.io_input_blocks = saturating_add(
            destination.io_input_blocks,
            source.io_input_blocks);
        destination.io_output_blocks = saturating_add(
            destination.io_output_blocks,
            source.io_output_blocks);
    }
    if (source.processes_available) {
        destination.processes_available = true;
        if (include_current_values)
            destination.active_processes = saturating_add(
                destination.active_processes,
                source.active_processes);
    }
}

void take_resource_maxima(
    ResourceValues& destination,
    const ResourceValues& source) noexcept {
    if (source.cpu_available) {
        destination.cpu_available = true;
        destination.user_cpu_ms =
            std::max(destination.user_cpu_ms, source.user_cpu_ms);
        destination.kernel_cpu_ms =
            std::max(destination.kernel_cpu_ms, source.kernel_cpu_ms);
    }
    if (source.faults_available) {
        destination.faults_available = true;
        destination.page_faults =
            std::max(destination.page_faults, source.page_faults);
    }
    if (source.io_available) {
        destination.io_available = true;
        destination.io_read_bytes =
            std::max(destination.io_read_bytes, source.io_read_bytes);
        destination.io_write_bytes =
            std::max(destination.io_write_bytes, source.io_write_bytes);
        destination.io_other_bytes =
            std::max(destination.io_other_bytes, source.io_other_bytes);
        destination.io_read_operations = std::max(
            destination.io_read_operations,
            source.io_read_operations);
        destination.io_write_operations = std::max(
            destination.io_write_operations,
            source.io_write_operations);
        destination.io_other_operations = std::max(
            destination.io_other_operations,
            source.io_other_operations);
    }
    if (source.io_blocks_available) {
        destination.io_blocks_available = true;
        destination.io_input_blocks = std::max(
            destination.io_input_blocks,
            source.io_input_blocks);
        destination.io_output_blocks = std::max(
            destination.io_output_blocks,
            source.io_output_blocks);
    }
    if (source.memory_available) {
        destination.memory_available = true;
        destination.working_set_peak_bytes = std::max(
            destination.working_set_peak_bytes,
            std::max(
                source.working_set_bytes,
                source.working_set_peak_bytes));
        destination.private_commit_peak_bytes = std::max(
            destination.private_commit_peak_bytes,
            std::max(
                source.private_commit_bytes,
                source.private_commit_peak_bytes));
    }
}

#ifdef _WIN32

[[nodiscard]] std::uint64_t file_time_milliseconds(
    const FILETIME time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<std::uint64_t>(value.QuadPart / 10'000u);
}

[[nodiscard]] ResourceValues capture_windows_process(
    const HANDLE process) noexcept {
    ResourceValues result;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(
            process, &created, &exited, &kernel, &user)) {
        result.cpu_available = true;
        result.user_cpu_ms = file_time_milliseconds(user);
        result.kernel_cpu_ms = file_time_milliseconds(kernel);
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (K32GetProcessMemoryInfo(
            process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory))) {
        result.memory_available = true;
        result.faults_available = true;
        result.working_set_bytes =
            static_cast<std::uint64_t>(memory.WorkingSetSize);
        result.working_set_peak_bytes =
            static_cast<std::uint64_t>(memory.PeakWorkingSetSize);
        result.private_commit_bytes =
            static_cast<std::uint64_t>(memory.PrivateUsage);
        result.private_commit_peak_bytes =
            result.private_commit_bytes;
        result.page_faults =
            static_cast<std::uint64_t>(memory.PageFaultCount);
    }

    IO_COUNTERS io{};
    if (GetProcessIoCounters(process, &io)) {
        result.io_available = true;
        result.io_read_bytes =
            static_cast<std::uint64_t>(io.ReadTransferCount);
        result.io_write_bytes =
            static_cast<std::uint64_t>(io.WriteTransferCount);
        result.io_other_bytes =
            static_cast<std::uint64_t>(io.OtherTransferCount);
        result.io_read_operations =
            static_cast<std::uint64_t>(io.ReadOperationCount);
        result.io_write_operations =
            static_cast<std::uint64_t>(io.WriteOperationCount);
        result.io_other_operations =
            static_cast<std::uint64_t>(io.OtherOperationCount);
    }
    result.processes_available = true;
    result.active_processes = 1u;
    return result;
}

struct TreeCapture final {
    ResourceValues values;
    bool supported = true;
    bool complete = true;
};

[[nodiscard]] TreeCapture capture_windows_job(
    const std::uintptr_t native_handle) noexcept {
    TreeCapture result;
    if (native_handle == 0u) {
        result.supported = false;
        result.complete = false;
        return result;
    }
    const auto job = reinterpret_cast<HANDLE>(native_handle);
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(
            job,
            JobObjectBasicAndIoAccountingInformation,
            &accounting,
            sizeof(accounting),
            nullptr)) {
        result.complete = false;
        return result;
    }

    result.values.cpu_available = true;
    result.values.user_cpu_ms =
        static_cast<std::uint64_t>(
            accounting.BasicInfo.TotalUserTime.QuadPart /
            10'000);
    result.values.kernel_cpu_ms =
        static_cast<std::uint64_t>(
            accounting.BasicInfo.TotalKernelTime.QuadPart /
            10'000);
    result.values.faults_available = true;
    result.values.page_faults =
        static_cast<std::uint64_t>(
            accounting.BasicInfo.TotalPageFaultCount);
    result.values.io_available = true;
    result.values.io_read_bytes =
        static_cast<std::uint64_t>(
            accounting.IoInfo.ReadTransferCount);
    result.values.io_write_bytes =
        static_cast<std::uint64_t>(
            accounting.IoInfo.WriteTransferCount);
    result.values.io_other_bytes =
        static_cast<std::uint64_t>(
            accounting.IoInfo.OtherTransferCount);
    result.values.io_read_operations =
        static_cast<std::uint64_t>(
            accounting.IoInfo.ReadOperationCount);
    result.values.io_write_operations =
        static_cast<std::uint64_t>(
            accounting.IoInfo.WriteOperationCount);
    result.values.io_other_operations =
        static_cast<std::uint64_t>(
            accounting.IoInfo.OtherOperationCount);
    result.values.processes_available = true;
    result.values.active_processes =
        static_cast<std::uint64_t>(
            accounting.BasicInfo.ActiveProcesses);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended{};
    if (QueryInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &extended,
            sizeof(extended),
            nullptr)) {
        result.values.memory_available = true;
        result.values.private_commit_peak_bytes =
            static_cast<std::uint64_t>(
                extended.PeakJobMemoryUsed);
    } else {
        result.complete = false;
    }

    std::size_t capacity = 64u;
    constexpr std::size_t maximum_processes = 4'096u;
    std::vector<std::byte> storage;
    PJOBOBJECT_BASIC_PROCESS_ID_LIST identifiers = nullptr;
    for (;;) {
        const auto bytes =
            sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
            (capacity - 1u) * sizeof(ULONG_PTR);
        storage.resize(bytes);
        identifiers =
            reinterpret_cast<PJOBOBJECT_BASIC_PROCESS_ID_LIST>(
                storage.data());
        if (QueryInformationJobObject(
                job,
                JobObjectBasicProcessIdList,
                identifiers,
                static_cast<DWORD>(storage.size()),
                nullptr))
            break;
        if (GetLastError() != ERROR_MORE_DATA ||
            capacity == maximum_processes) {
            identifiers = nullptr;
            result.complete = false;
            break;
        }
        capacity = std::min(
            maximum_processes, capacity * 2u);
    }

    if (identifiers != nullptr) {
        const auto count = std::min<std::size_t>(
            identifiers->NumberOfProcessIdsInList,
            capacity);
        if (identifiers->NumberOfAssignedProcesses >
            identifiers->NumberOfProcessIdsInList)
            result.complete = false;
        std::uint64_t current_working_set = 0u;
        std::uint64_t current_private_commit = 0u;
        bool captured_memory = false;
        for (std::size_t index = 0u; index < count; ++index) {
            const auto process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION |
                    PROCESS_VM_READ,
                FALSE,
                static_cast<DWORD>(
                    identifiers->ProcessIdList[index]));
            if (process == nullptr) {
                result.complete = false;
                continue;
            }
            const auto process_values =
                capture_windows_process(process);
            CloseHandle(process);
            if (!process_values.memory_available) {
                result.complete = false;
                continue;
            }
            captured_memory = true;
            current_working_set = saturating_add(
                current_working_set,
                process_values.working_set_bytes);
            current_private_commit = saturating_add(
                current_private_commit,
                process_values.private_commit_bytes);
        }
        if (captured_memory || count == 0u) {
            result.values.memory_available = true;
            result.values.working_set_bytes =
                current_working_set;
            result.values.private_commit_bytes =
                current_private_commit;
            result.values.working_set_peak_bytes =
                current_working_set;
            result.values.private_commit_peak_bytes = std::max(
                result.values.private_commit_peak_bytes,
                current_private_commit);
        }
    }
    return result;
}

#else

struct TreeCapture final {
    ResourceValues values;
    bool supported = true;
    bool complete = true;
};

[[nodiscard]] bool read_text_file(
    const std::filesystem::path& path,
    std::string& result) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) return false;
    result = buffer.str();
    return true;
}

[[nodiscard]] std::optional<std::uint64_t> status_kilobytes(
    const std::string_view status,
    const std::string_view field) noexcept {
    const auto position = status.find(field);
    if (position == std::string_view::npos) return std::nullopt;
    auto value_begin = position + field.size();
    while (value_begin < status.size() &&
           (status[value_begin] == ' ' ||
            status[value_begin] == '\t'))
        ++value_begin;
    auto value_end = value_begin;
    while (value_end < status.size() &&
           status[value_end] >= '0' &&
           status[value_end] <= '9')
        ++value_end;
    const auto value = parse_unsigned_decimal(
        status.substr(value_begin, value_end - value_begin));
    if (!value ||
        *value > std::numeric_limits<std::uint64_t>::max() /
                     1'024u)
        return std::nullopt;
    return *value * 1'024u;
}

[[nodiscard]] std::optional<std::uint64_t> io_field(
    const std::string_view io,
    const std::string_view field) noexcept {
    const auto position = io.find(field);
    if (position == std::string_view::npos) return std::nullopt;
    auto value_begin = position + field.size();
    while (value_begin < io.size() &&
           (io[value_begin] == ' ' ||
            io[value_begin] == '\t'))
        ++value_begin;
    auto value_end = value_begin;
    while (value_end < io.size() &&
           io[value_end] >= '0' &&
           io[value_end] <= '9')
        ++value_end;
    return parse_unsigned_decimal(
        io.substr(value_begin, value_end - value_begin));
}

struct PosixProcess final {
    std::int64_t process_group = -1;
    ResourceValues values;
};

[[nodiscard]] std::optional<PosixProcess> capture_posix_process(
    const std::uint64_t process_id) {
    const auto root =
        std::filesystem::path("/proc") /
        std::to_string(process_id);
    std::string stat;
    if (!read_text_file(root / "stat", stat))
        return std::nullopt;
    const auto command_end = stat.rfind(')');
    if (command_end == std::string::npos ||
        command_end + 2u >= stat.size())
        return std::nullopt;
    std::istringstream fields(stat.substr(command_end + 2u));
    std::vector<std::string> tokens;
    std::string token;
    while (fields >> token) tokens.push_back(std::move(token));
    // tokens[0] is field 3 (state). pgrp/minflt/majflt/utime/stime
    // are fields 5/10/12/14/15 respectively.
    if (tokens.size() <= 12u) return std::nullopt;
    const auto pgrp = parse_unsigned_decimal(tokens[2u]);
    const auto minor_faults = parse_unsigned_decimal(tokens[7u]);
    const auto major_faults = parse_unsigned_decimal(tokens[9u]);
    const auto user_ticks = parse_unsigned_decimal(tokens[11u]);
    const auto kernel_ticks = parse_unsigned_decimal(tokens[12u]);
    if (!pgrp || !minor_faults || !major_faults ||
        !user_ticks || !kernel_ticks)
        return std::nullopt;

    PosixProcess result;
    result.process_group =
        static_cast<std::int64_t>(*pgrp);
    const auto ticks_per_second = ::sysconf(_SC_CLK_TCK);
    if (ticks_per_second > 0) {
        const auto to_milliseconds =
            [ticks_per_second](const std::uint64_t ticks) {
                const auto value =
                    static_cast<long double>(ticks) *
                    1'000.0L /
                    static_cast<long double>(ticks_per_second);
                return static_cast<std::uint64_t>(
                    std::min<long double>(
                        value,
                        static_cast<long double>(
                            std::numeric_limits<
                                std::uint64_t>::max())));
            };
        result.values.cpu_available = true;
        result.values.user_cpu_ms =
            to_milliseconds(*user_ticks);
        result.values.kernel_cpu_ms =
            to_milliseconds(*kernel_ticks);
    }
    result.values.faults_available = true;
    result.values.page_faults =
        saturating_add(*minor_faults, *major_faults);
    result.values.processes_available = true;
    result.values.active_processes = 1u;

    std::string status;
    if (read_text_file(root / "status", status)) {
        const auto resident = status_kilobytes(
            status, "VmRSS:");
        const auto resident_peak = status_kilobytes(
            status, "VmHWM:");
        const auto private_resident = status_kilobytes(
            status, "RssAnon:");
        if (resident || resident_peak || private_resident) {
            result.values.memory_available = true;
            result.values.working_set_bytes =
                resident.value_or(0u);
            result.values.working_set_peak_bytes =
                resident_peak.value_or(
                    result.values.working_set_bytes);
            // Linux does not expose a Windows-style private commit counter
            // cheaply per process. RssAnon is reported under the common
            // private/commit field with sampled quality.
            result.values.private_commit_bytes =
                private_resident.value_or(0u);
            result.values.private_commit_peak_bytes =
                result.values.private_commit_bytes;
        }
    }
    std::string io;
    if (read_text_file(root / "io", io)) {
        const auto read_bytes = io_field(io, "read_bytes:");
        const auto write_bytes = io_field(io, "write_bytes:");
        const auto read_operations = io_field(io, "syscr:");
        const auto write_operations = io_field(io, "syscw:");
        if (read_bytes || write_bytes ||
            read_operations || write_operations) {
            result.values.io_available = true;
            result.values.io_read_bytes =
                read_bytes.value_or(0u);
            result.values.io_write_bytes =
                write_bytes.value_or(0u);
            result.values.io_read_operations =
                read_operations.value_or(0u);
            result.values.io_write_operations =
                write_operations.value_or(0u);
        }
    }
    return result;
}

[[nodiscard]] ResourceValues capture_posix_self() noexcept {
#if defined(__linux__)
    if (const auto self = capture_posix_process(
            static_cast<std::uint64_t>(::getpid())))
        return self->values;
#endif
    ResourceValues result;
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) == 0) {
        result.cpu_available = true;
        result.user_cpu_ms =
            static_cast<std::uint64_t>(
                usage.ru_utime.tv_sec) *
                1'000u +
            static_cast<std::uint64_t>(
                usage.ru_utime.tv_usec) /
                1'000u;
        result.kernel_cpu_ms =
            static_cast<std::uint64_t>(
                usage.ru_stime.tv_sec) *
                1'000u +
            static_cast<std::uint64_t>(
                usage.ru_stime.tv_usec) /
                1'000u;
        result.faults_available = true;
        result.page_faults = saturating_add(
            static_cast<std::uint64_t>(usage.ru_minflt),
            static_cast<std::uint64_t>(usage.ru_majflt));
        result.processes_available = true;
        result.active_processes = 1u;
    }
    return result;
}

[[nodiscard]] TreeCapture capture_posix_group(
    const std::int64_t process_group,
    const std::span<const std::int64_t> extra_processes) noexcept {
    TreeCapture result;
#if defined(__linux__)
    if (process_group <= 0) {
        result.supported = false;
        result.complete = false;
        return result;
    }
    std::error_code error;
    std::unordered_set<std::uint64_t> captured_processes;
    std::size_t inspected = 0u;
    constexpr std::size_t maximum_inspected_processes = 65'536u;
    for (std::filesystem::directory_iterator iterator(
             "/proc",
             std::filesystem::directory_options::
                 skip_permission_denied,
             error),
         end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (++inspected > maximum_inspected_processes) {
            result.complete = false;
            break;
        }
        const auto name =
            iterator->path().filename().string();
        const auto id = parse_unsigned_decimal(name);
        if (!id) continue;
        const auto process = capture_posix_process(*id);
        if (!process ||
            process->process_group != process_group)
            continue;
        // The CLI normally lives outside the supervised group. Avoid a
        // duplicate if an embedding host deliberately registers its own.
        if (*id == static_cast<std::uint64_t>(::getpid()))
            continue;
        captured_processes.insert(*id);
        add_resource_values(
            result.values, process->values);
    }
    if (error) result.complete = false;
    for (const auto process_id : extra_processes) {
        if (process_id <= 0 ||
            process_id == static_cast<std::int64_t>(::getpid()))
            continue;
        const auto unsigned_id =
            static_cast<std::uint64_t>(process_id);
        if (captured_processes.contains(unsigned_id)) continue;
        const auto process = capture_posix_process(unsigned_id);
        if (process) {
            captured_processes.insert(unsigned_id);
            add_resource_values(result.values, process->values);
            continue;
        }
        // Vanishing between the supervisor snapshot and this sampled query
        // is ordinary; a still-live known descendant must not disappear from
        // the resource-quality proof.
        if (::kill(static_cast<pid_t>(process_id), 0) == 0 ||
            errno == EPERM)
            result.complete = false;
    }
#else
    static_cast<void>(process_group);
    static_cast<void>(extra_processes);
    result.supported = false;
    result.complete = false;
#endif
    return result;
}

#endif

struct CapturedResourceRecord final {
    ResourceValues values;
    bool tree_registered = false;
    bool retired_trees_included = false;
    bool wait_accounting_included = false;
    bool supervised_descendants_included = false;
    bool tree_query_complete = true;
    std::string quality = "unsupported";
    std::string cpu_quality = "unsupported";
    std::string memory_quality = "unsupported";
    std::string faults_quality = "unsupported";
    std::string processes_quality = "unsupported";
    std::string io_bytes_quality = "unsupported";
    std::string io_blocks_quality = "unsupported";
    std::optional<std::uint64_t> effective_core_percent_milli;
    std::optional<std::uint64_t>
        effective_host_percent_milli;
    PortBuildGpuResourceSample gpu;
};

[[nodiscard]] std::string format_environment_manifest() {
    struct EnvironmentField final {
        const char* name;
        const char* category;
    };
    constexpr std::array fields{
        EnvironmentField{
            "KATANA_ANALYSIS_JOBS", "job"},
        EnvironmentField{
            "KATANA_CODEGEN_JOBS", "job-primary"},
        EnvironmentField{
            "KATANA_PORT_CODEGEN_JOBS", "job-legacy-alias"},
        EnvironmentField{
            "KATANA_HOST_BUILD_JOBS", "job-legacy-alias"},
        EnvironmentField{
            "KATANA_HOST_COMPILE_JOBS", "job-primary"},
        EnvironmentField{
            "CMAKE_BUILD_PARALLEL_LEVEL", "job"},
        EnvironmentField{
            "KATANA_PORT_HOST_COMMAND_TIMEOUT_MS", "job"},
        EnvironmentField{
            "KATANA_RUNTIME_JOBS", "job"},
        EnvironmentField{
            "KATANA_COMPILER_CACHE", "cache"},
        EnvironmentField{
            "SCCACHE_DIR", "cache"},
        EnvironmentField{
            "SCCACHE_CACHE_SIZE", "cache"},
        EnvironmentField{
            "CCACHE_DIR", "cache"},
        EnvironmentField{
            "CCACHE_MAXSIZE", "cache"},
    };
    std::string output = "[";
    for (std::size_t index = 0u; index < fields.size(); ++index) {
        if (index != 0u) output.push_back(',');
        const auto value =
            environment_value(fields[index].name);
        output += "{\"name\":";
        append_json_string(output, fields[index].name);
        output += ",\"category\":";
        append_json_string(output, fields[index].category);
        output += ",\"state\":";
        append_json_string(
            output, value ? "configured" : "unset");
        output += ",\"raw_value_omitted\":true}";
    }
    output.push_back(']');
    return output;
}

[[nodiscard]] std::string format_manifest(
    const PortBuildTelemetryOptions& options) {
    const auto host = inspect_host();
    const auto observer_toolchain = safe_display_label(
        compiled_toolchain(), "unknown");
    const auto build_profile = safe_identifier(
        options.build_profile.empty()
            ? compiled_build_profile()
            : options.build_profile,
        "configured-profile");
    const auto job_kind =
        safe_identifier(options.job_kind, "port-build");
    const auto gpu_identity = safe_display_label(
        options.gpu_identity,
        options.gpu_identity.empty()
            ? "unreported"
            : "configured-gpu");
    const auto gpu_backend = safe_identifier(
        options.gpu_backend,
        options.gpu_backend.empty()
            ? "none"
            : "configured");

    std::string output;
    output.reserve(2'048u);
    output += "\"manifest\":{\"host\":{\"os_family\":";
    append_json_string(output, host.os_family);
    output += ",\"os_version\":";
    append_json_string(output, host.os_version);
    output += ",\"architecture\":";
    append_json_string(output, host.architecture);
    output += ",\"cpu_model\":";
    append_json_string(output, host.cpu_model);
    output += ",\"physical_cores\":";
    append_unsigned(output, host.physical_cores);
    output += ",\"logical_processors\":";
    append_unsigned(output, host.logical_processors);
    output += ",\"smt_present\":";
    output +=
        host.logical_processors > host.physical_cores
            ? "true"
            : "false";
    output += ",\"ram_bytes\":";
    append_unsigned(output, host.ram_bytes);
    output +=
        "},\"telemetry_binary\":{\"role\":\"observer\","
        "\"compiler_identity\":";
    append_json_string(output, observer_toolchain);
    output += ",\"cplusplus\":";
    append_unsigned(
        output, static_cast<std::uint64_t>(__cplusplus));
    output +=
        ",\"cplusplus_quality\":\"compiler-predefined-macro-raw\"";
    output += ",\"build_profile\":";
    append_json_string(output, build_profile);
    output += "},\"job\":{\"kind\":";
    append_json_string(output, job_kind);
    output += ",\"requested_environment\":";
    output += format_environment_manifest();
    output += ",\"host_compile_budget\":{\"requested\":";
    append_unsigned(
        output, options.host_compile_jobs_requested);
    output += ",\"effective\":";
    append_unsigned(
        output, options.host_compile_jobs_effective);
    output +=
        ",\"quality\":\"hard-process-wide-upper-bound\"}";
    output += "},\"gpu\":{\"identity\":";
    append_json_string(output, gpu_identity);
    output += ",\"backend\":";
    append_json_string(output, gpu_backend);
    output += ",\"memory_bytes\":";
    append_optional_unsigned(
        output, options.gpu_memory_bytes);
    output += ",\"telemetry_quality\":";
    append_json_string(
        output,
        options.gpu_identity.empty()
            ? "unsupported"
            : "caller-provided-identity");
    output +=
        "},\"privacy\":{\"private_paths\":\"omitted\","
        "\"guest_addresses\":\"omitted\","
        "\"raw_environment_values\":\"omitted\"}}";
    return output;
}

[[nodiscard]] bool resolved_label_valid(
    const std::string_view value,
    const std::size_t maximum_size = 160u) noexcept {
    if (value.empty() || value.size() > maximum_size ||
        path_like(value) || guest_address_like(value))
        return false;
    return std::none_of(
        value.begin(),
        value.end(),
        [](const char character) {
            const auto byte =
                static_cast<unsigned char>(character);
            return byte < 0x20u || byte == 0x7fu;
        });
}

[[nodiscard]] bool resolved_environment_valid(
    const PortBuildResolvedEnvironment& environment) noexcept {
    const std::array labels{
        std::string_view(environment.compiler_identity),
        std::string_view(environment.compiler_version),
        std::string_view(environment.compiler_quality),
        std::string_view(environment.linker_identity),
        std::string_view(environment.linker_version),
        std::string_view(environment.linker_quality),
        std::string_view(environment.cmake_version),
        std::string_view(environment.generator_identity),
        std::string_view(environment.generator_version),
        std::string_view(environment.generator_version_quality),
        std::string_view(environment.cache_launcher_identity),
        std::string_view(environment.cache_launcher_quality),
        std::string_view(environment.platform.filesystem_type),
        std::string_view(environment.platform.filesystem_quality),
        std::string_view(environment.platform.storage_type),
        std::string_view(environment.platform.storage_quality),
        std::string_view(environment.platform.energy_profile),
        std::string_view(environment.platform.energy_quality)};
    return std::all_of(
               labels.begin(),
               labels.end(),
               [](const auto value) {
                   return resolved_label_valid(value);
               }) &&
           environment.analysis_jobs != 0u &&
           environment.host_compile_jobs_requested != 0u &&
           environment.host_compile_jobs_effective != 0u &&
           environment.host_compile_jobs_effective <=
               environment.host_compile_jobs_requested &&
           environment.runtime_jobs != 0u;
}

[[nodiscard]] std::string format_resolved_environment(
    const PortBuildResolvedEnvironment& environment) {
    std::string output;
    output.reserve(1'536u);
    output +=
        "\"resolved_environment\":{\"contract_version\":2,"
        "\"source\":\"post-configure-cmake-state\","
        "\"toolchain\":{\"compiler\":{\"identity\":";
    append_json_string(output, environment.compiler_identity);
    output += ",\"version\":";
    append_json_string(output, environment.compiler_version);
    output += ",\"quality\":";
    append_json_string(output, environment.compiler_quality);
    output += "},\"linker\":{\"identity\":";
    append_json_string(output, environment.linker_identity);
    output += ",\"version\":";
    append_json_string(output, environment.linker_version);
    output += ",\"quality\":";
    append_json_string(output, environment.linker_quality);
    output += "}},\"cmake\":{\"version\":";
    append_json_string(output, environment.cmake_version);
    output += "},\"generator\":{\"identity\":";
    append_json_string(output, environment.generator_identity);
    output += ",\"version\":";
    append_json_string(output, environment.generator_version);
    output += ",\"version_quality\":";
    append_json_string(
        output, environment.generator_version_quality);
    output += "},\"jobs\":{\"analysis\":";
    append_unsigned(output, environment.analysis_jobs);
    output += ",\"codegen\":";
    append_unsigned(output, environment.codegen_jobs);
    output += ",\"host_compile_requested\":";
    append_unsigned(
        output, environment.host_compile_jobs_requested);
    output += ",\"host_compile_effective\":";
    append_unsigned(
        output, environment.host_compile_jobs_effective);
    output += ",\"host_build\":";
    append_unsigned(
        output, environment.host_compile_jobs_effective);
    output += ",\"runtime_parallel_work\":";
    append_unsigned(output, environment.runtime_jobs);
    output +=
        ",\"runtime_parallel_work_quality\":"
        "\"effective-runtime-worker-capacity-not-sdk-compile\","
        "\"quality\":\"phase-specific-effective-capacity\"},"
        "\"cache_launcher\":{\"identity\":";
    append_json_string(
        output, environment.cache_launcher_identity);
    output += ",\"enabled\":";
    output += environment.cache_launcher_identity == "none"
                  ? "false"
                  : "true";
    output += ",\"quality\":";
    append_json_string(
        output, environment.cache_launcher_quality);
    output += "},\"platform\":{\"filesystem\":{\"type\":";
    append_json_string(
        output, environment.platform.filesystem_type);
    output += ",\"quality\":";
    append_json_string(
        output, environment.platform.filesystem_quality);
    output += "},\"storage\":{\"type\":";
    append_json_string(output, environment.platform.storage_type);
    output += ",\"quality\":";
    append_json_string(
        output, environment.platform.storage_quality);
    output += "},\"energy\":{\"profile\":";
    append_json_string(output, environment.platform.energy_profile);
    output += ",\"quality\":";
    append_json_string(
        output, environment.platform.energy_quality);
    output +=
        "}},\"privacy\":{\"private_paths\":\"omitted\","
        "\"raw_environment_values\":\"omitted\"}}";
    return output;
}

[[nodiscard]] bool host_command_observation_valid(
    const PortBuildHostCommandObservation& observation) noexcept {
    return resolved_label_valid(observation.stage, 80u) &&
           (!observation.forwarded_signal ||
            (*observation.forwarded_signal > 0 &&
             *observation.forwarded_signal <= 255)) &&
           (!observation.interrupted ||
            observation.forwarded_signal.has_value()) &&
           (!observation.timed_out ||
            !observation.interrupted) &&
           resolved_label_valid(
               observation.process_tree_scope, 80u) &&
           (!observation.process_tree_query_complete ||
            observation.process_tree_scope != "unsupported");
}

[[nodiscard]] std::string format_host_command_observation(
    const PortBuildHostCommandObservation& observation) {
    std::string output = "\"host_command\":{\"contract_version\":2,\"stage\":";
    append_json_string(output, observation.stage);
    output += ",\"host_exit_code\":";
    if (observation.host_exit_code)
        append_signed(output, *observation.host_exit_code);
    else
        output += "null";
    output += ",\"timed_out\":";
    output += observation.timed_out ? "true" : "false";
    output += ",\"interrupted\":";
    output += observation.interrupted ? "true" : "false";
    output += ",\"forwarded_signal\":";
    if (observation.forwarded_signal)
        append_signed(output, *observation.forwarded_signal);
    else
        output += "null";
    output += ",\"process_tree_quiescent\":";
    output += observation.process_tree_quiescent ? "true" : "false";
    output += ",\"process_tree_scope\":";
    append_json_string(output, observation.process_tree_scope);
    output += ",\"process_tree_query_complete\":";
    output += observation.process_tree_query_complete
                  ? "true"
                  : "false";
    output += '}';
    return output;
}

[[nodiscard]] bool phase_timings_valid(
    const std::uint64_t total_ms,
    const std::span<const PortBuildPhaseTimingSample> samples) noexcept {
    static_cast<void>(total_ms);
    if (samples.empty() || samples.size() > 4'096u) return false;
    return std::all_of(
        samples.begin(), samples.end(),
        [](const PortBuildPhaseTimingSample& sample) {
            return resolved_label_valid(sample.phase, 160u);
        });
}

[[nodiscard]] std::string format_phase_timings(
    const std::uint64_t total_ms,
    const std::span<const PortBuildPhaseTimingSample> samples) {
    std::string output =
        "\"phase_timings\":{\"contract_version\":1,\"total_ms\":";
    append_unsigned(output, total_ms);
    output += ",\"samples\":[";
    for (std::size_t index = 0u; index < samples.size(); ++index) {
        if (index != 0u) output.push_back(',');
        output += "{\"phase\":";
        append_json_string(output, samples[index].phase);
        output += ",\"duration_ms\":";
        append_unsigned(output, samples[index].duration_ms);
        output += ",\"parallel\":";
        output += samples[index].parallel ? "true" : "false";
        output.push_back('}');
    }
    output += "]}";
    return output;
}

[[nodiscard]] PortBuildResolvedPlatform
inspect_resolved_platform_impl(
    const std::filesystem::path& build_path) noexcept {
    PortBuildResolvedPlatform result;
#ifdef _WIN32
    try {
        std::array<wchar_t, MAX_PATH + 1u> volume_root{};
        if (GetVolumePathNameW(
                build_path.c_str(),
                volume_root.data(),
                static_cast<DWORD>(volume_root.size()))) {
            std::array<wchar_t, 64u> filesystem{};
            if (GetVolumeInformationW(
                    volume_root.data(),
                    nullptr,
                    0u,
                    nullptr,
                    nullptr,
                    nullptr,
                    filesystem.data(),
                    static_cast<DWORD>(filesystem.size()))) {
                std::string name;
                for (const auto character : filesystem) {
                    if (character == L'\0') break;
                    if (character < 0 || character > 0x7f) {
                        name.clear();
                        break;
                    }
                    name.push_back(static_cast<char>(character));
                }
                if (!name.empty()) {
                    result.filesystem_type = safe_display_label(
                        name, "unknown", 63u);
                    result.filesystem_quality = "os-volume-exact";
                }
            }
            switch (GetDriveTypeW(volume_root.data())) {
            case DRIVE_FIXED:
                result.storage_type = "fixed";
                break;
            case DRIVE_REMOVABLE:
                result.storage_type = "removable";
                break;
            case DRIVE_REMOTE:
                result.storage_type = "network";
                break;
            case DRIVE_CDROM:
                result.storage_type = "optical";
                break;
            case DRIVE_RAMDISK:
                result.storage_type = "ramdisk";
                break;
            default:
                break;
            }
            if (result.storage_type != "unknown")
                result.storage_quality = "os-drive-class";
        }

        const auto library = LoadLibraryW(L"powrprof.dll");
        if (library != nullptr) {
            using PowerGetActiveSchemeFunction =
                DWORD(WINAPI*)(HKEY, GUID**);
            const auto address = GetProcAddress(
                library, "PowerGetActiveScheme");
            PowerGetActiveSchemeFunction get_active_scheme = nullptr;
            static_assert(
                sizeof(get_active_scheme) == sizeof(address));
            std::memcpy(
                &get_active_scheme,
                &address,
                sizeof(address));
            GUID* scheme = nullptr;
            if (get_active_scheme != nullptr &&
                get_active_scheme(nullptr, &scheme) == ERROR_SUCCESS &&
                scheme != nullptr) {
                constexpr GUID power_saver{
                    0xa1841308,
                    0x3541,
                    0x4fab,
                    {0xbc, 0x81, 0xf7, 0x15,
                     0x56, 0xf2, 0x0b, 0x4a}};
                constexpr GUID balanced{
                    0x381b4222,
                    0xf694,
                    0x41f0,
                    {0x96, 0x85, 0xff, 0x5b,
                     0xb2, 0x60, 0xdf, 0x2e}};
                constexpr GUID high_performance{
                    0x8c5e7fda,
                    0xe8bf,
                    0x4a96,
                    {0x9a, 0x85, 0xa6, 0xe2,
                     0x3a, 0x8c, 0x63, 0x5c}};
                result.energy_profile =
                    std::memcmp(scheme, &power_saver, sizeof(GUID)) == 0
                        ? "power-saver"
                        : std::memcmp(scheme, &balanced, sizeof(GUID)) == 0
                              ? "balanced"
                              : std::memcmp(
                                        scheme,
                                        &high_performance,
                                        sizeof(GUID)) == 0
                                    ? "high-performance"
                                    : "custom";
                result.energy_quality = "os-active-scheme-exact";
                static_cast<void>(LocalFree(scheme));
            }
            static_cast<void>(FreeLibrary(library));
        }
    } catch (...) {
    }
#else
#if defined(__linux__)
    try {
        struct statfs filesystem_information {};
        if (::statfs(
                build_path.c_str(),
                &filesystem_information) == 0) {
            const auto type = static_cast<unsigned long>(
                filesystem_information.f_type);
            switch (type) {
            case 0xef53ul:
                result.filesystem_type = "ext-family";
                break;
            case 0x58465342ul:
                result.filesystem_type = "xfs";
                break;
            case 0x9123683eul:
                result.filesystem_type = "btrfs";
                break;
            case 0x01021994ul:
                result.filesystem_type = "tmpfs";
                result.storage_type = "memory";
                result.storage_quality = "kernel-filesystem-class";
                break;
            case 0x858458f6ul:
                result.filesystem_type = "ramfs";
                result.storage_type = "memory";
                result.storage_quality = "kernel-filesystem-class";
                break;
            case 0x6969ul:
                result.filesystem_type = "nfs";
                result.storage_type = "network";
                result.storage_quality = "kernel-filesystem-class";
                break;
            case 0xff534d42ul:
                result.filesystem_type = "cifs";
                result.storage_type = "network";
                result.storage_quality = "kernel-filesystem-class";
                break;
            case 0x65735546ul:
                result.filesystem_type = "fuse";
                break;
            default:
                break;
            }
            if (result.filesystem_type != "unknown")
                result.filesystem_quality = "kernel-statfs-exact";
        }

        if (result.storage_type == "unknown") {
            struct stat path_information {};
            if (::stat(build_path.c_str(), &path_information) == 0) {
                const auto device =
                    std::to_string(
                        static_cast<unsigned long long>(
                            major(path_information.st_dev))) +
                    ':' +
                    std::to_string(
                        static_cast<unsigned long long>(
                            minor(path_information.st_dev)));
                std::error_code canonical_error;
                auto current = std::filesystem::weakly_canonical(
                    std::filesystem::path("/sys/dev/block") / device,
                    canonical_error);
                while (!canonical_error &&
                       !current.empty() &&
                       current != current.root_path()) {
                    std::ifstream rotational(
                        current / "queue" / "rotational",
                        std::ios::binary);
                    std::string value;
                    if (std::getline(rotational, value)) {
                        if (!value.empty() && value.back() == '\r')
                            value.pop_back();
                        if (value == "0" || value == "1") {
                            result.storage_type =
                                value == "0"
                                    ? "solid-state"
                                    : "rotational";
                            result.storage_quality =
                                "kernel-block-queue-exact";
                            break;
                        }
                    }
                    const auto parent = current.parent_path();
                    if (parent == current) break;
                    current = parent;
                }
            }
        }

        std::ifstream profile(
            "/sys/firmware/acpi/platform_profile",
            std::ios::binary);
        std::string value;
        if (std::getline(profile, value)) {
            if (!value.empty() && value.back() == '\r')
                value.pop_back();
            if (resolved_label_valid(value, 64u)) {
                result.energy_profile = value;
                result.energy_quality =
                    "kernel-platform-profile-exact";
            }
        }
    } catch (...) {
    }
#else
    static_cast<void>(build_path);
#endif
#endif
    return result;
}

[[nodiscard]] bool regular_non_link_file(
    const std::filesystem::path& path) noexcept {
    try {
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(
            path, status_error);
        if (status_error ||
            !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status))
            return false;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u;
#else
        return true;
#endif
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::optional<std::string>
telemetry_cmake_cache_value(
    const std::filesystem::path& cache_path,
    const std::string_view key) {
    if (!regular_non_link_file(cache_path))
        throw std::runtime_error(
            "CMakeCache ist keine sichere regulaere Datei.");
    std::ifstream cache(cache_path, std::ios::binary);
    if (!cache)
        throw std::runtime_error(
            "CMakeCache konnte nicht gelesen werden.");
    std::optional<std::string> result;
    std::string line;
    while (std::getline(cache, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() <= key.size() ||
            !line.starts_with(key) ||
            line[key.size()] != ':')
            continue;
        const auto assignment = line.find(
            '=', key.size() + 1u);
        if (assignment == std::string::npos || result)
            throw std::runtime_error(
                "CMakeCache ist fuer einen kritischen Wert "
                "ungueltig oder mehrdeutig.");
        result = line.substr(assignment + 1u);
    }
    if (!cache.eof())
        throw std::runtime_error(
            "CMakeCache konnte nicht vollstaendig gelesen werden.");
    return result;
}

[[nodiscard]] std::optional<std::string>
telemetry_cmake_compiler_state_value(
    const std::filesystem::path& state_path,
    const std::string_view key) {
    if (!regular_non_link_file(state_path))
        throw std::runtime_error(
            "CMake-Compilerzustand ist keine sichere regulaere Datei.");
    std::ifstream state(state_path, std::ios::binary);
    if (!state)
        throw std::runtime_error(
            "CMake-Compilerzustand konnte nicht gelesen werden.");
    const auto prefix =
        std::string("set(") + std::string(key) + " \"";
    constexpr std::string_view suffix = "\")";
    std::optional<std::string> result;
    std::string line;
    while (std::getline(state, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.starts_with(prefix) ||
            !line.ends_with(suffix))
            continue;
        if (result)
            throw std::runtime_error(
                "CMake-Compilerzustand ist fuer einen kritischen "
                "Toolchainwert mehrdeutig.");
        result = line.substr(
            prefix.size(),
            line.size() - prefix.size() - suffix.size());
    }
    if (!state.eof())
        throw std::runtime_error(
            "CMake-Compilerzustand konnte nicht vollstaendig gelesen werden.");
    return result;
}

[[nodiscard]] std::string require_resolved_value(
    const std::optional<std::string>& value,
    const std::string_view key) {
    if (!value || value->empty())
        throw std::runtime_error(
            "Post-Configure-Telemetrie vermisst den Wert " +
            std::string(key) + '.');
    if (!resolved_label_valid(*value))
        throw std::runtime_error(
            "Post-Configure-Telemetrie besitzt einen unsicheren Wert " +
            std::string(key) + '.');
    return *value;
}

[[nodiscard]] std::string cache_launcher_identity(
    const std::filesystem::path& cache_path) {
    const auto launcher = telemetry_cmake_cache_value(
        cache_path, "CMAKE_CXX_COMPILER_LAUNCHER");
    if (!launcher || launcher->empty()) return "none";
    const auto separator = launcher->find(';');
    const auto executable = launcher->substr(0u, separator);
    auto identity =
        std::filesystem::path(executable).filename().string();
    if (identity.empty()) return "custom";
    std::transform(
        identity.begin(),
        identity.end(),
        identity.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (identity.ends_with(".exe"))
        identity.resize(identity.size() - 4u);
    return resolved_label_valid(identity, 64u)
               ? identity
               : std::string("custom");
}

[[nodiscard]] std::string tool_identity_from_path(
    const std::filesystem::path& path) {
    auto identity = path.filename().string();
    if (identity.empty())
        throw std::runtime_error(
            "Toolchainprogramm besitzt keine portable Identitaet.");
    std::transform(
        identity.begin(),
        identity.end(),
        identity.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (identity.ends_with(".exe"))
        identity.resize(identity.size() - 4u);
    if (!resolved_label_valid(identity, 64u))
        throw std::runtime_error(
            "Toolchainprogramm besitzt keine sichere Identitaet.");
    return identity;
}

void bind_linker_environment(
    PortBuildResolvedEnvironment& environment,
    const std::filesystem::path& compiler_state) {
    const auto linker_identity =
        telemetry_cmake_compiler_state_value(
            compiler_state,
            "CMAKE_CXX_COMPILER_LINKER_ID");
    const auto linker_version =
        telemetry_cmake_compiler_state_value(
            compiler_state,
            "CMAKE_CXX_COMPILER_LINKER_VERSION");
    if (linker_identity && !linker_identity->empty() &&
        linker_version && !linker_version->empty()) {
        environment.linker_identity = require_resolved_value(
            linker_identity,
            "CMAKE_CXX_COMPILER_LINKER_ID");
        environment.linker_version = require_resolved_value(
            linker_version,
            "CMAKE_CXX_COMPILER_LINKER_VERSION");
        return;
    }

    auto linker_path = telemetry_cmake_compiler_state_value(
        compiler_state, "CMAKE_CXX_COMPILER_LINKER");
    if (!linker_path || linker_path->empty())
        linker_path = telemetry_cmake_compiler_state_value(
            compiler_state, "CMAKE_LINKER");
    if (!linker_path || linker_path->empty())
        throw std::runtime_error(
            "CMake-Compilerzustand besitzt weder Linkerversion noch "
            "Linkerprogramm.");
    const auto path = std::filesystem::path(*linker_path);
    const auto provenance =
        katana::io::capture_input_provenance(
            "child-linker-binary", path);
    environment.linker_identity =
        tool_identity_from_path(path);
    environment.linker_version =
        "sha256:" + provenance.sha256;
    environment.linker_quality =
        "binary-content-identity";
}

[[nodiscard]] PortBuildResolvedEnvironment
resolve_cmake_environment_impl(
    const std::filesystem::path& build_path,
    const std::size_t analysis_jobs,
    const std::size_t codegen_jobs,
    const std::size_t host_compile_jobs_requested,
    const std::size_t host_compile_jobs_effective,
    const std::size_t runtime_jobs) {
    const auto cache_path = build_path / "CMakeCache.txt";
    const auto cmake_major = require_resolved_value(
        telemetry_cmake_cache_value(
            cache_path, "CMAKE_CACHE_MAJOR_VERSION"),
        "CMAKE_CACHE_MAJOR_VERSION");
    const auto cmake_minor = require_resolved_value(
        telemetry_cmake_cache_value(
            cache_path, "CMAKE_CACHE_MINOR_VERSION"),
        "CMAKE_CACHE_MINOR_VERSION");
    const auto cmake_patch = require_resolved_value(
        telemetry_cmake_cache_value(
            cache_path, "CMAKE_CACHE_PATCH_VERSION"),
        "CMAKE_CACHE_PATCH_VERSION");
    const auto cmake_version =
        cmake_major + '.' + cmake_minor + '.' + cmake_patch;
    const auto compiler_state =
        build_path / "CMakeFiles" / cmake_version /
        "CMakeCXXCompiler.cmake";

    PortBuildResolvedEnvironment environment;
    environment.compiler_identity = require_resolved_value(
        telemetry_cmake_compiler_state_value(
            compiler_state, "CMAKE_CXX_COMPILER_ID"),
        "CMAKE_CXX_COMPILER_ID");
    environment.compiler_version = require_resolved_value(
        telemetry_cmake_compiler_state_value(
            compiler_state, "CMAKE_CXX_COMPILER_VERSION"),
        "CMAKE_CXX_COMPILER_VERSION");
    bind_linker_environment(environment, compiler_state);
    environment.cmake_version = cmake_version;
    environment.generator_identity = require_resolved_value(
        telemetry_cmake_cache_value(
            cache_path, "CMAKE_GENERATOR"),
        "CMAKE_GENERATOR");
    environment.generator_version = cmake_version;
    environment.cache_launcher_identity =
        cache_launcher_identity(cache_path);
    environment.analysis_jobs = analysis_jobs;
    environment.codegen_jobs = codegen_jobs;
    environment.host_compile_jobs_requested =
        host_compile_jobs_requested;
    environment.host_compile_jobs_effective =
        host_compile_jobs_effective;
    environment.runtime_jobs = runtime_jobs;
    environment.platform =
        inspect_resolved_platform_impl(build_path);
    if (!resolved_environment_valid(environment))
        throw std::runtime_error(
            "Post-Configure-Umgebungsrecord ist unvollstaendig.");
    return environment;
}

[[nodiscard]] std::string format_resource_record(
    const CapturedResourceRecord& snapshot,
    const std::string_view phase) {
    const auto& values = snapshot.values;
    const auto optional_if =
        [](const bool available, const std::uint64_t value)
        -> std::optional<std::uint64_t> {
        return available
                   ? std::optional<std::uint64_t>(value)
                   : std::nullopt;
    };
    std::string output;
    output.reserve(1'536u);
    output += "\"phase\":";
    append_json_string(
        output,
        safe_identifier(phase, phase.empty() ? "" : "custom"));
    output += ",\"resource\":{\"quality\":";
    append_json_string(output, snapshot.quality);
    output += ",\"tree_registered\":";
    output += snapshot.tree_registered ? "true" : "false";
    output += ",\"retired_trees_included\":";
    output +=
        snapshot.retired_trees_included ? "true" : "false";
    output += ",\"wait_accounting_included\":";
    output +=
        snapshot.wait_accounting_included ? "true" : "false";
    output += ",\"tree_query_complete\":";
    output += snapshot.tree_query_complete ? "true" : "false";
    output += ",\"cpu_quality\":";
    append_json_string(output, snapshot.cpu_quality);
    output += ",\"memory_quality\":";
    append_json_string(output, snapshot.memory_quality);
    output += ",\"faults_quality\":";
    append_json_string(output, snapshot.faults_quality);
    output += ",\"processes_quality\":";
    append_json_string(output, snapshot.processes_quality);
    output += ",\"io_bytes_quality\":";
    append_json_string(output, snapshot.io_bytes_quality);
    output += ",\"io_blocks_quality\":";
    append_json_string(output, snapshot.io_blocks_quality);
    output += ",\"cpu_user_ms\":";
    append_optional_unsigned(
        output,
        optional_if(values.cpu_available, values.user_cpu_ms));
    output += ",\"cpu_kernel_ms\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.cpu_available, values.kernel_cpu_ms));
    output += ",\"cpu_total_ms\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.cpu_available,
            saturating_add(
                values.user_cpu_ms, values.kernel_cpu_ms)));
    output += ",\"effective_core_percent_milli\":";
    append_optional_unsigned(
        output, snapshot.effective_core_percent_milli);
    output += ",\"effective_host_percent_milli\":";
    append_optional_unsigned(
        output, snapshot.effective_host_percent_milli);
    output += ",\"working_set_bytes_current\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.memory_available,
            values.working_set_bytes));
    output += ",\"working_set_bytes_peak\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.memory_available,
            values.working_set_peak_bytes));
    output += ",\"private_commit_bytes_current\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.memory_available,
            values.private_commit_bytes));
    output += ",\"private_commit_bytes_peak\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.memory_available,
            values.private_commit_peak_bytes));
    output += ",\"page_faults\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.faults_available, values.page_faults));
    output += ",\"io_read_bytes\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.io_available, values.io_read_bytes));
    output += ",\"io_write_bytes\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.io_available, values.io_write_bytes));
    output += ",\"io_other_bytes\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.io_available, values.io_other_bytes));
    output += ",\"io_read_operations\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.io_available,
            values.io_read_operations));
    output += ",\"io_write_operations\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.io_available,
            values.io_write_operations));
    output += ",\"io_other_operations\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.io_available,
            values.io_other_operations));
    output += ",\"io_input_blocks\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.io_blocks_available,
            values.io_input_blocks));
    output += ",\"io_output_blocks\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.io_blocks_available,
            values.io_output_blocks));
    output += ",\"active_processes\":";
    append_optional_unsigned(
        output,
        optional_if(
            values.processes_available,
            values.active_processes));
    output += "},\"gpu\":{\"quality\":";
    append_json_string(
        output,
        safe_identifier(snapshot.gpu.quality, "unsupported"));
    output += ",\"utilization_percent_milli\":";
    append_optional_unsigned(
        output, snapshot.gpu.utilization_percent_milli);
    output += ",\"memory_bytes_current\":";
    append_optional_unsigned(
        output, snapshot.gpu.memory_bytes_current);
    output += ",\"memory_bytes_peak\":";
    append_optional_unsigned(
        output, snapshot.gpu.memory_bytes_peak);
    output += ",\"host_to_device_bytes\":";
    append_optional_unsigned(
        output, snapshot.gpu.host_to_device_bytes);
    output += ",\"device_to_host_bytes\":";
    append_optional_unsigned(
        output, snapshot.gpu.device_to_host_bytes);
    output.push_back('}');
    return output;
}

} // namespace

PortBuildResolvedPlatform inspect_port_build_resolved_platform(
    const std::filesystem::path& build_path) noexcept {
    return inspect_resolved_platform_impl(build_path);
}

std::filesystem::path port_build_telemetry_writer_lock_path(
    const std::filesystem::path& jsonl_path) {
    const auto normalized =
        std::filesystem::absolute(jsonl_path).lexically_normal();
    if (normalized.empty() ||
        normalized == normalized.root_path() ||
        normalized.filename().empty() ||
        normalized.filename() == "." ||
        normalized.filename() == "..")
        throw std::invalid_argument(
            "Telemetrie-Writer-Lock besitzt kein gueltiges JSONL-Ziel.");
    auto lock_name = normalized.filename();
    lock_name += ".katana-telemetry-writer.lock";
    return normalized.parent_path() / lock_name;
}

PortBuildResolvedEnvironment resolve_port_build_cmake_environment(
    const std::filesystem::path& build_path,
    const std::size_t analysis_jobs,
    const std::size_t codegen_jobs,
    const std::size_t host_compile_jobs_requested,
    const std::size_t host_compile_jobs_effective,
    const std::size_t runtime_jobs) {
    return resolve_cmake_environment_impl(
        build_path,
        analysis_jobs,
        codegen_jobs,
        host_compile_jobs_requested,
        host_compile_jobs_effective,
        runtime_jobs);
}

class PortBuildTelemetryRecorder::Impl final {
  public:
    explicit Impl(PortBuildTelemetryOptions options) noexcept
        : options_(std::move(options)),
          started_(SteadyClock::now()) {
        try {
            options_.maximum_pending_records = std::clamp(
                options_.maximum_pending_records,
                minimum_pending_records,
                maximum_pending_records);
            options_.maximum_record_bytes = std::clamp(
                options_.maximum_record_bytes,
                minimum_record_bytes,
                maximum_record_bytes);
            options_.resource_sample_interval = std::clamp(
                options_.resource_sample_interval,
                std::chrono::milliseconds(100),
                std::chrono::milliseconds(10'000));
            if (!options_.jsonl_path) return;
            *options_.jsonl_path =
                std::filesystem::absolute(*options_.jsonl_path).
                    lexically_normal();
            inspect_logical_processors_ =
                std::max<std::uint64_t>(
                    1u,
                    static_cast<std::uint64_t>(
                        std::thread::hardware_concurrency()));
            const auto parent =
                options_.jsonl_path->parent_path();
            if (!parent.empty()) {
                std::error_code directory_error;
                std::filesystem::create_directories(
                    parent, directory_error);
                if (directory_error) {
                    io_failed_ = true;
                    return;
                }
            }
            if (!open_temporary_output()) {
                io_failed_ = true;
                discard_temporary_output();
                release_writer_lock();
                return;
            }
            accepting_ = true;
            queue_.push_back(Record{
                std::string(port_build_manifest_schema),
                format_manifest(options_),
                true,
                0u});
            enabled_ = true;
            worker_ = std::thread([this] { writer_loop(); });
        } catch (...) {
            accepting_ = false;
            enabled_ = false;
            io_failed_ = true;
            queue_.clear();
            discard_temporary_output();
            release_writer_lock();
        }
    }

    ~Impl() noexcept {
        finish(
            PortBuildTerminalOutcome::Abandoned,
            1,
            "recorder-destroyed");
        if (worker_.joinable()) worker_.join();
        discard_temporary_output();
        release_writer_lock();
    }

    [[nodiscard]] bool enabled() const noexcept {
        try {
            std::scoped_lock lock(writer_mutex_);
            return enabled_ && accepting_ && !closing_ &&
                   !io_failed_;
        } catch (...) {
            return false;
        }
    }

    void observe_progress(
        const ProgressEvent& source_event) noexcept {
        if (!begin_producer()) return;
        try {
            auto observed =
                upstream_dropped_observations_.load(
                    std::memory_order_relaxed);
            while (observed < source_event.dropped_observations &&
                   !upstream_dropped_observations_.
                       compare_exchange_weak(
                           observed,
                           source_event.dropped_observations,
                           std::memory_order_relaxed,
                           std::memory_order_relaxed)) {
            }
            if (!source_event.telemetry_complete ||
                !progress_cache_accounting_valid(
                    source_event.counters))
                upstream_incomplete_.store(
                    true, std::memory_order_relaxed);

            auto event = source_event;
            if (!event.label.empty() &&
                (path_like(event.label) ||
                 guest_address_like(event.label)))
                event.label = "redacted";
            if (event.label.size() >
                maximum_progress_label_bytes) {
                event.label.resize(
                    maximum_progress_label_bytes);
                event.label += "...[truncated]";
                upstream_incomplete_.store(
                    true, std::memory_order_relaxed);
            }
            auto payload = std::string("\"progress\":");
            payload += format_progress_event_json(event);
            const auto critical =
                event.state == ProgressState::Started ||
                event.state == ProgressState::Completed ||
                event.state == ProgressState::Cached ||
                event.state == ProgressState::Skipped ||
                event.state == ProgressState::Failed;
            static_cast<void>(enqueue(
                Record{
                    std::string(port_build_progress_schema),
                    std::move(payload),
                    critical,
                    elapsed_milliseconds()}));
            sample_resources_if_due(
                progress_operation_name(event.operation),
                false);
        } catch (...) {
            note_lost();
        }
        end_producer();
    }

    [[nodiscard]] bool record_resolved_environment(
        PortBuildResolvedEnvironment environment) noexcept {
        if (!begin_producer()) return false;
        bool accepted = false;
        bool reserved = false;
        try {
            if (!resolved_environment_valid(environment)) {
                note_lost();
            } else {
                {
                    std::scoped_lock lock(writer_mutex_);
                    if (resolved_environment_recorded_ ||
                        resolved_environment_in_flight_) {
                        lost_records_ = saturating_add(
                            lost_records_, 1u);
                    } else {
                        resolved_environment_in_flight_ = true;
                        reserved = true;
                    }
                }
                if (reserved) {
                    accepted = enqueue(Record{
                        std::string(
                            port_build_resolved_environment_schema),
                        format_resolved_environment(environment),
                        true,
                        elapsed_milliseconds()});
                    std::scoped_lock lock(writer_mutex_);
                    resolved_environment_in_flight_ = false;
                    if (accepted)
                        resolved_environment_recorded_ = true;
                }
            }
        } catch (...) {
            if (reserved) {
                try {
                    std::scoped_lock lock(writer_mutex_);
                    resolved_environment_in_flight_ = false;
                } catch (...) {
                }
            }
            note_lost();
            accepted = false;
        }
        end_producer();
        return accepted;
    }

    [[nodiscard]] bool record_host_command(
        PortBuildHostCommandObservation observation) noexcept {
        if (!begin_producer()) return false;
        bool accepted = false;
        try {
            if (!host_command_observation_valid(observation)) {
                note_lost();
            } else {
                accepted = enqueue(Record{
                    std::string(port_build_host_command_schema),
                    format_host_command_observation(observation),
                    true,
                    elapsed_milliseconds()});
            }
        } catch (...) {
            note_lost();
            accepted = false;
        }
        end_producer();
        return accepted;
    }

    [[nodiscard]] bool record_phase_timings(
        const std::uint64_t total_ms,
        const std::span<const PortBuildPhaseTimingSample> samples) noexcept {
        if (!begin_producer()) return false;
        bool accepted = false;
        bool reserved = false;
        try {
            if (!phase_timings_valid(total_ms, samples)) {
                note_lost();
            } else {
                {
                    std::scoped_lock lock(writer_mutex_);
                    if (phase_timings_recorded_ ||
                        phase_timings_in_flight_) {
                        lost_records_ = saturating_add(
                            lost_records_, 1u);
                    } else {
                        phase_timings_in_flight_ = true;
                        reserved = true;
                    }
                }
                if (reserved) {
                    accepted = enqueue(Record{
                        std::string(port_build_phase_timings_schema),
                        format_phase_timings(total_ms, samples),
                        true,
                        elapsed_milliseconds()});
                    std::scoped_lock lock(writer_mutex_);
                    phase_timings_in_flight_ = false;
                    if (accepted) phase_timings_recorded_ = true;
                }
            }
        } catch (...) {
            if (reserved) {
                try {
                    std::scoped_lock lock(writer_mutex_);
                    phase_timings_in_flight_ = false;
                } catch (...) {
                }
            }
            note_lost();
            accepted = false;
        }
        end_producer();
        return accepted;
    }

    [[nodiscard]] bool mark_upstream_incomplete(
        const std::string_view reason,
        const std::uint64_t cumulative_dropped_observations) noexcept {
        if (!begin_producer()) return false;
        bool retained = false;
        try {
            const auto safe_reason = safe_identifier(
                reason,
                reason.empty() ? "unspecified" : "invalid-reason");
            std::scoped_lock lock(writer_mutex_);
            upstream_incomplete_.store(
                true, std::memory_order_relaxed);
            if (upstream_incomplete_reason_.empty())
                upstream_incomplete_reason_ = safe_reason;
            auto observed =
                upstream_dropped_observations_.load(
                    std::memory_order_relaxed);
            while (observed < cumulative_dropped_observations &&
                   !upstream_dropped_observations_.
                       compare_exchange_weak(
                           observed,
                           cumulative_dropped_observations,
                           std::memory_order_relaxed,
                           std::memory_order_relaxed)) {
            }
            retained = true;
        } catch (...) {
            upstream_incomplete_.store(
                true, std::memory_order_relaxed);
            note_lost();
        }
        end_producer();
        return retained;
    }

    void sample_resources(
        const std::string_view phase) noexcept {
        if (!begin_producer()) return;
        try {
            sample_resources_if_due(phase, true);
        } catch (...) {
            note_lost();
        }
        end_producer();
    }

    void set_gpu_resource_sample(
        PortBuildGpuResourceSample sample) noexcept {
        if (!enabled()) return;
        try {
            if (sample.utilization_percent_milli)
                *sample.utilization_percent_milli =
                    std::min<std::uint64_t>(
                    *sample.utilization_percent_milli,
                    100'000u);
            sample.quality =
                safe_identifier(sample.quality, "unsupported");
            std::scoped_lock lock(resource_mutex_);
            gpu_sample_ = std::move(sample);
        } catch (...) {
            note_lost();
        }
    }

#ifdef _WIN32
    void register_windows_job(
        const std::uintptr_t native_job_handle) noexcept {
        if (!enabled()) return;
        try {
            std::scoped_lock lock(resource_mutex_);
            retire_process_tree_locked();
            windows_job_handle_ = native_job_handle;
            last_tree_capture_ = {};
            active_tree_complete_ = true;
        } catch (...) {
            note_lost();
        }
    }
#else
    void register_posix_process_group(
        const std::int64_t process_group) noexcept {
        if (!enabled()) return;
        try {
            std::scoped_lock lock(resource_mutex_);
            retire_process_tree_locked();
            posix_process_group_ = process_group;
            posix_additional_processes_.clear();
            last_tree_capture_ = {};
            // /proc is a live snapshot. Short-lived descendants can exit
            // between samples, so the whole process-tree query is never
            // represented as exact merely because directory iteration
            // succeeded.
            active_tree_complete_ = false;
        } catch (...) {
            note_lost();
        }
    }

    void update_posix_process_tree_members(
        const std::span<const std::int64_t> process_ids) noexcept {
        if (!enabled()) return;
        try {
            constexpr std::size_t maximum_processes = 65'536u;
            if (process_ids.size() > maximum_processes ||
                std::any_of(
                    process_ids.begin(),
                    process_ids.end(),
                    [](const std::int64_t process) {
                        return process <= 0;
                    })) {
                note_lost();
                return;
            }
            std::vector<std::int64_t> normalized(
                process_ids.begin(), process_ids.end());
            std::sort(normalized.begin(), normalized.end());
            normalized.erase(
                std::unique(
                    normalized.begin(), normalized.end()),
                normalized.end());
            bool registered = false;
            {
                std::scoped_lock lock(resource_mutex_);
                registered = posix_process_group_ > 0;
                if (registered)
                    posix_additional_processes_ =
                        std::move(normalized);
            }
            if (!registered) {
                note_lost();
                return;
            }
        } catch (...) {
            note_lost();
        }
    }

    void record_posix_process_tree_final_sample(
        const PortBuildPosixProcessTreeFinalSample&
            sample) noexcept {
        if (!enabled()) return;
        try {
            ResourceValues final_values;
            if (sample.cpu_available) {
                final_values.cpu_available = true;
                final_values.user_cpu_ms =
                    sample.user_cpu_ms;
                final_values.kernel_cpu_ms =
                    sample.kernel_cpu_ms;
            }
            if (sample.faults_available) {
                final_values.faults_available = true;
                final_values.page_faults =
                    sample.page_faults;
            }
            if (sample.working_set_peak_available) {
                final_values.memory_available = true;
                final_values.working_set_peak_bytes =
                    sample.working_set_peak_bytes;
            }
            if (sample.io_blocks_available) {
                final_values.io_blocks_available = true;
                final_values.io_input_blocks =
                    sample.io_input_blocks;
                final_values.io_output_blocks =
                    sample.io_output_blocks;
            }
            std::scoped_lock lock(resource_mutex_);
            if (posix_process_group_ <= 0) {
                note_lost();
                return;
            }
            take_resource_maxima(
                last_tree_capture_, final_values);
            posix_final_sample_present_ = true;
        } catch (...) {
            note_lost();
        }
    }
#endif

    void clear_process_tree() noexcept {
        if (!enabled()) return;
        try {
            std::scoped_lock lock(resource_mutex_);
            retire_process_tree_locked();
        } catch (...) {
            note_lost();
        }
    }

    void finish(
        const PortBuildTerminalOutcome outcome,
        const int exit_code,
        const std::string_view terminal_phase) noexcept {
        try {
            std::scoped_lock finish_lock(finish_mutex_);
            {
                std::scoped_lock lock(writer_mutex_);
                if (!enabled_ || terminal_emitted_)
                    return;
            }
            sample_resources_if_due(
                terminal_phase.empty()
                    ? "terminal"
                    : terminal_phase,
                true);
            clear_process_tree();

            std::unique_lock lock(writer_mutex_);
            if (!enabled_ || terminal_emitted_) return;
            closing_ = true;
            producer_done_.wait(lock, [&] {
                return active_producers_ == 0u;
            });
            if (!enabled_ || io_failed_ ||
                terminal_emitted_)
                return;
            if (!terminal_requested_) {
                accepting_ = false;
                terminal_request_ = TerminalRequest{
                    outcome,
                    exit_code,
                    safe_identifier(
                        terminal_phase,
                        terminal_phase.empty()
                            ? ""
                            : "custom"),
                    admitted_elapsed_milliseconds_locked()};
                terminal_requested_ = true;
                writer_changed_.notify_all();
            }
            writer_done_.wait(lock, [&] {
                return terminal_emitted_ || io_failed_ ||
                       !worker_.joinable();
            });
            lock.unlock();
            if (worker_.joinable()) worker_.join();
            bool terminal_written = false;
            {
                std::scoped_lock status_lock(writer_mutex_);
                terminal_written =
                    terminal_emitted_ && !io_failed_;
            }
            const auto published =
                terminal_written &&
                publish_temporary_output();
            {
                std::scoped_lock status_lock(writer_mutex_);
                published_ = published;
                if (!published) {
                    io_failed_ = true;
                    enabled_ = false;
                    accepting_ = false;
                }
            }
            if (!published)
                discard_temporary_output();
        } catch (...) {
            std::scoped_lock lock(writer_mutex_);
            closing_ = true;
            accepting_ = false;
            enabled_ = false;
            io_failed_ = true;
            writer_changed_.notify_all();
            writer_done_.notify_all();
        }
    }

    [[nodiscard]] PortBuildTelemetryStatus
    status() const noexcept {
        try {
            std::scoped_lock lock(writer_mutex_);
            const auto upstream =
                upstream_dropped_observations_.load(
                    std::memory_order_relaxed);
            return {
                enabled_ && accepting_ && !io_failed_,
                terminal_emitted_ && published_,
                terminal_stream_complete_locked() &&
                    (!terminal_emitted_ || published_),
                io_failed_,
                written_records_,
                lost_records_,
                upstream,
                resolved_environment_recorded_};
        } catch (...) {
            return {
                false, false, false, true, 0u, 0u, 0u, false};
        }
    }

  private:
    struct Record final {
        std::string schema;
        std::string fields;
        bool critical = false;
        std::uint64_t elapsed_ms = 0u;
    };

    struct TerminalRequest final {
        PortBuildTerminalOutcome outcome =
            PortBuildTerminalOutcome::Abandoned;
        int exit_code = 1;
        std::string phase;
        std::uint64_t elapsed_ms = 0u;
    };

    [[nodiscard]] std::uint64_t
    elapsed_milliseconds() const noexcept {
        const auto elapsed =
            std::chrono::duration_cast<
                std::chrono::milliseconds>(
                SteadyClock::now() - started_);
        return elapsed.count() < 0
                   ? 0u
                   : static_cast<std::uint64_t>(
                         elapsed.count());
    }

    void note_lost(
        const std::uint64_t amount = 1u) noexcept {
        std::scoped_lock lock(writer_mutex_);
        lost_records_ =
            saturating_add(lost_records_, amount);
    }

    [[nodiscard]] bool begin_producer() noexcept {
        try {
            std::scoped_lock lock(writer_mutex_);
            if (!enabled_ || !accepting_ || io_failed_)
                return false;
            if (closing_) {
                lost_records_ =
                    saturating_add(lost_records_, 1u);
                return false;
            }
            ++active_producers_;
            return true;
        } catch (...) {
            return false;
        }
    }

    void end_producer() noexcept {
        try {
            std::scoped_lock lock(writer_mutex_);
            if (active_producers_ != 0u)
                --active_producers_;
            if (active_producers_ == 0u)
                producer_done_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] bool enqueue(Record record) noexcept {
        try {
            std::scoped_lock lock(writer_mutex_);
            if (!enabled_ || !accepting_ || io_failed_) {
                lost_records_ =
                    saturating_add(lost_records_, 1u);
                return false;
            }
            if (record.fields.size() +
                    record.schema.size() + 512u >
                options_.maximum_record_bytes) {
                lost_records_ =
                    saturating_add(lost_records_, 1u);
                return false;
            }
            if (queue_.size() >=
                options_.maximum_pending_records) {
                if (record.critical) {
                    const auto expendable = std::find_if(
                        queue_.begin(),
                        queue_.end(),
                        [](const Record& queued) {
                            return !queued.critical;
                        });
                    if (expendable != queue_.end()) {
                        queue_.erase(expendable);
                        lost_records_ = saturating_add(
                            lost_records_, 1u);
                    } else {
                        lost_records_ = saturating_add(
                            lost_records_, 1u);
                        return false;
                    }
                } else {
                    lost_records_ =
                        saturating_add(lost_records_, 1u);
                    return false;
                }
            }
            // Timestamp and queue order must be one admission epoch. A
            // producer may spend arbitrary time formatting its record before
            // acquiring writer_mutex_; retaining that earlier timestamp can
            // make elapsed_ms regress even though sequence remains ordered.
            record.elapsed_ms = admitted_elapsed_milliseconds_locked();
            queue_.push_back(std::move(record));
            writer_changed_.notify_one();
            return true;
        } catch (...) {
            note_lost();
            return false;
        }
    }

    [[nodiscard]] std::uint64_t
    admitted_elapsed_milliseconds_locked() noexcept {
        const auto observed = elapsed_milliseconds();
        last_admitted_elapsed_ms_ =
            std::max(last_admitted_elapsed_ms_, observed);
        return last_admitted_elapsed_ms_;
    }

    void sample_resources_if_due(
        const std::string_view phase,
        const bool force) noexcept {
        try {
            CapturedResourceRecord snapshot;
            {
                std::scoped_lock lock(resource_mutex_);
                const auto now = SteadyClock::now();
                if (!force &&
                    last_resource_sample_ !=
                        SteadyClock::time_point{} &&
                    now - last_resource_sample_ <
                        options_.resource_sample_interval)
                    return;
                last_resource_sample_ = now;
                snapshot = capture_resources_locked(now);
            }
            static_cast<void>(enqueue(Record{
                std::string(port_build_resource_schema),
                format_resource_record(snapshot, phase),
                false,
                elapsed_milliseconds()}));
        } catch (...) {
            note_lost();
        }
    }

    [[nodiscard]] TreeCapture
    capture_active_tree_locked() noexcept {
#ifdef _WIN32
        if (windows_job_handle_ == 0u) {
            TreeCapture empty;
            empty.supported = false;
            return empty;
        }
        return capture_windows_job(windows_job_handle_);
#else
        if (posix_process_group_ <= 0) {
            TreeCapture empty;
            empty.supported = false;
            return empty;
        }
        return capture_posix_group(
            posix_process_group_,
            posix_additional_processes_);
#endif
    }

    [[nodiscard]] bool tree_registered_locked() const noexcept {
#ifdef _WIN32
        return windows_job_handle_ != 0u;
#else
        return posix_process_group_ > 0;
#endif
    }

    void retire_process_tree_locked() noexcept {
        if (!tree_registered_locked()) return;
        const auto capture = capture_active_tree_locked();
        active_tree_complete_ =
            active_tree_complete_ &&
            capture.supported && capture.complete;
        take_resource_maxima(
            last_tree_capture_, capture.values);
        add_resource_values(
            retired_tree_cumulative_,
            last_tree_capture_,
            false);
        retired_tree_cumulative_.working_set_peak_bytes =
            std::max(
                retired_tree_cumulative_.
                    working_set_peak_bytes,
                last_tree_capture_.
                    working_set_peak_bytes);
        retired_tree_cumulative_.
            private_commit_peak_bytes =
            std::max(
                retired_tree_cumulative_.
                    private_commit_peak_bytes,
                last_tree_capture_.
                    private_commit_peak_bytes);
        retired_tree_present_ = true;
        retired_tree_complete_ =
            retired_tree_complete_ &&
            active_tree_complete_;
#ifdef _WIN32
        windows_job_handle_ = 0u;
#else
#if defined(__linux__)
        retired_posix_descendants_present_ =
            retired_posix_descendants_present_ ||
            !posix_additional_processes_.empty();
#endif
        posix_process_group_ = -1;
        posix_additional_processes_.clear();
#endif
        last_tree_capture_ = {};
    }

    [[nodiscard]] CapturedResourceRecord
    capture_resources_locked(
        const SteadyClock::time_point now) noexcept {
        CapturedResourceRecord result;
#ifdef _WIN32
        const auto self =
            capture_windows_process(GetCurrentProcess());
#else
        const auto self = capture_posix_self();
#endif
        add_resource_values(result.values, self);
        add_resource_values(
            result.values,
            retired_tree_cumulative_,
            false);

        result.tree_registered = tree_registered_locked();
        result.retired_trees_included =
            retired_tree_present_;
#ifndef _WIN32
        result.wait_accounting_included =
            posix_final_sample_present_;
#if defined(__linux__)
        result.supervised_descendants_included =
            retired_posix_descendants_present_ ||
            !posix_additional_processes_.empty();
#endif
#endif
        result.tree_query_complete =
            retired_tree_complete_;
        if (result.tree_registered) {
            const auto tree = capture_active_tree_locked();
            active_tree_complete_ =
                active_tree_complete_ &&
                tree.supported && tree.complete;
            result.tree_query_complete =
                result.tree_query_complete &&
                active_tree_complete_;
            take_resource_maxima(
                last_tree_capture_, tree.values);
            add_resource_values(
                result.values, tree.values);
        }
        const auto any_core_family =
            result.values.cpu_available ||
            result.values.memory_available ||
            result.values.faults_available ||
            result.values.processes_available;
        const auto all_core_families =
            result.values.cpu_available &&
            result.values.memory_available &&
            result.values.faults_available &&
            result.values.processes_available;
        result.quality = !any_core_family
                             ? "unsupported"
                             : all_core_families
                                   ? "sampled"
                                   : "partial";
#ifdef _WIN32
        const auto tree_included =
            result.tree_registered ||
            result.retired_trees_included;
        if (result.values.cpu_available)
            result.cpu_quality = tree_included
                                     ? "job-cumulative"
                                     : "process-cumulative";
        if (result.values.memory_available)
            result.memory_quality = tree_included
                                        ? "sampled-process-tree"
                                        : "sampled-process";
        if (result.values.faults_available)
            result.faults_quality = tree_included
                                        ? "job-cumulative"
                                        : "process-cumulative";
        if (result.values.processes_available)
            result.processes_quality = tree_included
                                           ? "job-active-count"
                                           : "process-self";
        if (result.values.io_available)
            result.io_bytes_quality =
                tree_included
                    ? "job-cumulative"
                    : "process-cumulative";
#else
#if defined(__linux__)
        if (result.values.cpu_available)
            result.cpu_quality = result.wait_accounting_included
                                     ? "sampled-procfs-plus-wait4-supervised-tree"
                                     : result.supervised_descendants_included
                                           ? "subreaper-descendant-tree-sampled-procfs"
                                           : "process-group-sampled-procfs";
        if (result.values.memory_available)
            result.memory_quality = result.wait_accounting_included
                                        ? "sampled-procfs-plus-wait4-supervised-tree-peak"
                                        : result.supervised_descendants_included
                                              ? "subreaper-descendant-tree-sampled-procfs"
                                              : "process-group-sampled-procfs";
        if (result.values.faults_available)
            result.faults_quality = result.wait_accounting_included
                                        ? "sampled-procfs-plus-wait4-supervised-tree"
                                        : result.supervised_descendants_included
                                              ? "subreaper-descendant-tree-sampled-procfs"
                                              : "process-group-sampled-procfs";
        if (result.values.processes_available)
            result.processes_quality =
                result.supervised_descendants_included
                    ? "subreaper-descendant-tree-sampled"
                    : "process-group-sampled";
#else
        if (result.values.cpu_available)
            result.cpu_quality = result.wait_accounting_included
                                     ? "getrusage-self-plus-wait4-root"
                                     : "getrusage-self";
        if (result.values.memory_available)
            result.memory_quality = "wait4-root-peak-only";
        if (result.values.faults_available)
            result.faults_quality = result.wait_accounting_included
                                        ? "getrusage-self-plus-wait4-root"
                                        : "getrusage-self";
        if (result.values.processes_available)
            result.processes_quality = "process-self-only";
#endif
        if (result.values.io_available)
            result.io_bytes_quality =
#if defined(__linux__)
                result.supervised_descendants_included
                    ? "subreaper-descendant-tree-sampled-procfs"
                    : "process-group-sampled-procfs";
#else
                "sampled-procfs";
#endif
        if (result.values.io_blocks_available &&
            result.wait_accounting_included)
            result.io_blocks_quality =
                "wait4-cumulative-block-count";
#endif

        observed_peak_working_set_ = std::max(
            observed_peak_working_set_,
            result.values.working_set_bytes);
        observed_peak_private_commit_ = std::max(
            observed_peak_private_commit_,
            result.values.private_commit_bytes);
        result.values.working_set_peak_bytes = std::max(
            result.values.working_set_peak_bytes,
            observed_peak_working_set_);
        result.values.private_commit_peak_bytes = std::max(
            result.values.private_commit_peak_bytes,
            observed_peak_private_commit_);

        if (result.values.cpu_available) {
            const auto total_cpu = saturating_add(
                result.values.user_cpu_ms,
                result.values.kernel_cpu_ms);
            if (previous_resource_sample_ &&
                total_cpu >= previous_total_cpu_ms_) {
                const auto wall =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        now - *previous_resource_sample_);
                if (wall.count() > 0) {
                    const auto cpu_delta =
                        total_cpu - previous_total_cpu_ms_;
                    const auto core_utilization =
                        static_cast<long double>(cpu_delta) *
                        100'000.0L /
                        static_cast<long double>(wall.count());
                    result.effective_core_percent_milli =
                        static_cast<std::uint64_t>(
                            std::min<long double>(
                                core_utilization,
                                static_cast<long double>(
                                    std::numeric_limits<
                                        std::uint64_t>::max())));
                    const auto logical =
                        std::max<std::uint64_t>(
                            1u,
                            inspect_logical_processors_);
                    result.effective_host_percent_milli =
                        std::min<std::uint64_t>(
                            100'000u,
                            *result.
                                 effective_core_percent_milli /
                                logical);
                }
            }
            previous_resource_sample_ = now;
            previous_total_cpu_ms_ = total_cpu;
        }
        result.gpu = gpu_sample_;
        return result;
    }

    [[nodiscard]] std::string format_record(
        const Record& record,
        const std::uint64_t sequence) const {
        const auto upstream =
            upstream_dropped_observations_.load(
                std::memory_order_relaxed);
        const auto complete =
            record.schema == port_build_terminal_schema
                ? terminal_stream_complete_locked()
                : lost_records_ == 0u && upstream == 0u &&
                      !upstream_incomplete_.load(
                          std::memory_order_relaxed) &&
                      !io_failed_;
        std::string output;
        output.reserve(record.fields.size() + 512u);
        output += "{\"schema\":";
        append_json_string(output, record.schema);
        output += ",\"schema_version\":1,\"stream_schema\":";
        append_json_string(
            output, port_build_telemetry_stream_schema);
        output += ",\"stream_schema_version\":";
        append_unsigned(
            output,
            port_build_telemetry_stream_schema_version);
        output += ",\"sequence\":";
        append_unsigned(output, sequence);
        output += ",\"elapsed_ms\":";
        append_unsigned(output, record.elapsed_ms);
        output.push_back(',');
        output += record.fields;
        output += ",\"lost_records\":";
        append_unsigned(output, lost_records_);
        output += ",\"upstream_dropped_observations\":";
        append_unsigned(output, upstream);
        output += ",\"upstream_incomplete\":";
        output += upstream_incomplete_.load(
                      std::memory_order_relaxed)
                      ? "true"
                      : "false";
        output += ",\"upstream_incomplete_reason\":";
        if (upstream_incomplete_reason_.empty())
            output += "null";
        else
            append_json_string(
                output, upstream_incomplete_reason_);
        output += ",\"telemetry_complete\":";
        output += complete ? "true" : "false";
        output.push_back('}');
        return output;
    }

    [[nodiscard]] Record terminal_record(
        const TerminalRequest& request) const {
        std::string fields = "\"outcome\":";
        append_json_string(
            fields,
            port_build_terminal_outcome_name(
                request.outcome));
        fields += ",\"exit_code\":";
        append_signed(fields, request.exit_code);
        fields += ",\"phase\":";
        append_json_string(fields, request.phase);
        fields += ",\"summary\":{\"written_before_terminal\":";
        append_unsigned(fields, written_records_);
        fields += ",\"queue_capacity\":";
        append_unsigned(
            fields,
            static_cast<std::uint64_t>(
                options_.maximum_pending_records));
        fields += ",\"maximum_record_bytes\":";
        append_unsigned(
            fields,
            static_cast<std::uint64_t>(
                options_.maximum_record_bytes));
        fields.push_back('}');
        return {
            std::string(port_build_terminal_schema),
            std::move(fields),
            true,
            request.elapsed_ms};
    }

    [[nodiscard]] bool
    terminal_stream_complete_locked() const noexcept {
        return !io_failed_ && lost_records_ == 0u &&
               upstream_dropped_observations_.load(
                   std::memory_order_relaxed) == 0u &&
               !upstream_incomplete_.load(
                   std::memory_order_relaxed) &&
               (!options_.require_resolved_environment ||
                resolved_environment_recorded_) &&
               (!options_.require_phase_timings ||
                phase_timings_recorded_);
    }

    void fail_writer_locked(
        const std::uint64_t additional_lost) noexcept {
        lost_records_ = saturating_add(
            lost_records_, additional_lost);
        io_failed_ = true;
        accepting_ = false;
        enabled_ = false;
        queue_.clear();
        writer_done_.notify_all();
    }

    [[nodiscard]] bool open_output_parent() noexcept {
        if (!options_.jsonl_path ||
            options_.jsonl_path->filename().empty() ||
            options_.jsonl_path->filename() == "." ||
            options_.jsonl_path->filename() == "..")
            return false;
        try {
            final_output_name_ =
                options_.jsonl_path->filename();
            const auto requested_parent =
                options_.jsonl_path->parent_path().empty()
                    ? std::filesystem::path(".")
                    : options_.jsonl_path->parent_path();
            parent_output_path_ =
                std::filesystem::absolute(requested_parent).
                    lexically_normal();
#ifdef _WIN32
            parent_output_handle_ = CreateFileW(
                parent_output_path_.c_str(),
                FILE_LIST_DIRECTORY | FILE_ADD_FILE |
                    FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (parent_output_handle_ ==
                INVALID_HANDLE_VALUE)
                return false;
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (GetFileType(parent_output_handle_) !=
                    FILE_TYPE_DISK ||
                !GetFileInformationByHandle(
                    parent_output_handle_,
                    &parent_output_identity_) ||
                !GetFileInformationByHandleEx(
                    parent_output_handle_,
                    FileAttributeTagInfo,
                    &attributes,
                    sizeof(attributes)) ||
                (attributes.FileAttributes &
                 FILE_ATTRIBUTE_DIRECTORY) == 0u ||
                (attributes.FileAttributes &
                 FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
                return false;
#else
            auto flags = O_RDONLY;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
            flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            parent_output_descriptor_ = ::open(
                parent_output_path_.c_str(), flags);
            if (parent_output_descriptor_ < 0)
                return false;
            struct stat information {};
            if (::fstat(
                    parent_output_descriptor_,
                    &information) != 0 ||
                !S_ISDIR(information.st_mode))
                return false;
            parent_output_device_ = information.st_dev;
            parent_output_inode_ = information.st_ino;
#endif
            return validate_output_parent();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool validate_output_parent() const noexcept {
        try {
#ifdef _WIN32
            if (parent_output_handle_ ==
                    INVALID_HANDLE_VALUE ||
                parent_output_path_.empty())
                return false;
            FILE_ATTRIBUTE_TAG_INFO held_attributes{};
            BY_HANDLE_FILE_INFORMATION held_identity{};
            if (GetFileType(parent_output_handle_) !=
                    FILE_TYPE_DISK ||
                !GetFileInformationByHandle(
                    parent_output_handle_,
                    &held_identity) ||
                !GetFileInformationByHandleEx(
                    parent_output_handle_,
                    FileAttributeTagInfo,
                    &held_attributes,
                    sizeof(held_attributes)) ||
                (held_attributes.FileAttributes &
                 FILE_ATTRIBUTE_DIRECTORY) == 0u ||
                (held_attributes.FileAttributes &
                 FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
                !same_windows_file_identity(
                    held_identity,
                    parent_output_identity_))
                return false;
            const auto named_parent = CreateFileW(
                parent_output_path_.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (named_parent == INVALID_HANDLE_VALUE)
                return false;
            FILE_ATTRIBUTE_TAG_INFO named_attributes{};
            BY_HANDLE_FILE_INFORMATION named_identity{};
            const auto valid =
                GetFileType(named_parent) == FILE_TYPE_DISK &&
                GetFileInformationByHandle(
                    named_parent, &named_identity) &&
                GetFileInformationByHandleEx(
                    named_parent,
                    FileAttributeTagInfo,
                    &named_attributes,
                    sizeof(named_attributes)) &&
                (named_attributes.FileAttributes &
                 FILE_ATTRIBUTE_DIRECTORY) != 0u &&
                (named_attributes.FileAttributes &
                 FILE_ATTRIBUTE_REPARSE_POINT) == 0u &&
                same_windows_file_identity(
                    named_identity,
                    parent_output_identity_);
            static_cast<void>(CloseHandle(named_parent));
            return valid;
#else
            if (parent_output_descriptor_ < 0 ||
                parent_output_path_.empty())
                return false;
            struct stat held_information {};
            struct stat named_information {};
            return ::fstat(
                       parent_output_descriptor_,
                       &held_information) == 0 &&
                   ::lstat(
                       parent_output_path_.c_str(),
                       &named_information) == 0 &&
                   S_ISDIR(held_information.st_mode) &&
                   S_ISDIR(named_information.st_mode) &&
                   held_information.st_dev ==
                       parent_output_device_ &&
                   held_information.st_ino ==
                       parent_output_inode_ &&
                   named_information.st_dev ==
                       parent_output_device_ &&
                   named_information.st_ino ==
                       parent_output_inode_;
#endif
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool
    validate_existing_output_target() const noexcept {
        if (final_output_name_.empty()) return false;
#ifdef _WIN32
        HANDLE existing = INVALID_HANDLE_VALUE;
        const auto opened = open_windows_directory_child(
            parent_output_handle_,
            final_output_name_,
            FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            FILE_OPEN,
            existing);
        if (opened == WindowsRelativeOpenResult::NotFound)
            return true;
        if (opened != WindowsRelativeOpenResult::Opened)
            return false;
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        BY_HANDLE_FILE_INFORMATION identity{};
        const auto valid =
            GetFileType(existing) == FILE_TYPE_DISK &&
            GetFileInformationByHandle(existing, &identity) &&
            GetFileInformationByHandleEx(
                existing,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) &&
            (attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY |
              FILE_ATTRIBUTE_REPARSE_POINT)) == 0u &&
            identity.nNumberOfLinks == 1u;
        static_cast<void>(CloseHandle(existing));
        return valid;
#else
        struct stat information {};
        if (::fstatat(
                parent_output_descriptor_,
                final_output_name_.c_str(),
                &information,
                AT_SYMLINK_NOFOLLOW) != 0)
            return errno == ENOENT;
        return S_ISREG(information.st_mode) &&
               information.st_nlink == 1;
#endif
    }

    [[nodiscard]] bool acquire_writer_lock() noexcept {
        try {
            if (!options_.jsonl_path ||
                !validate_output_parent())
                return false;
            writer_lock_name_ =
                port_build_telemetry_writer_lock_path(
                    *options_.jsonl_path).
                    filename();
            if (writer_lock_name_.empty() ||
                writer_lock_name_ == final_output_name_)
                return false;
#ifdef _WIN32
            const auto opened = open_windows_directory_child(
                parent_output_handle_,
                writer_lock_name_,
                FILE_READ_DATA | FILE_READ_ATTRIBUTES |
                    SYNCHRONIZE,
                0u,
                FILE_OPEN_IF,
                writer_lock_handle_);
            if (opened != WindowsRelativeOpenResult::Opened)
                return false;
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (GetFileType(writer_lock_handle_) != FILE_TYPE_DISK ||
                !GetFileInformationByHandle(
                    writer_lock_handle_,
                    &writer_lock_identity_) ||
                !GetFileInformationByHandleEx(
                    writer_lock_handle_,
                    FileAttributeTagInfo,
                    &attributes,
                    sizeof(attributes)) ||
                (attributes.FileAttributes &
                 (FILE_ATTRIBUTE_DIRECTORY |
                  FILE_ATTRIBUTE_REPARSE_POINT)) != 0u ||
                writer_lock_identity_.nNumberOfLinks != 1u) {
                release_writer_lock();
                return false;
            }
#else
            auto flags = O_RDONLY | O_CREAT;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            writer_lock_descriptor_ = ::openat(
                parent_output_descriptor_,
                writer_lock_name_.c_str(),
                flags,
                0600);
            if (writer_lock_descriptor_ < 0)
                return false;
            struct stat information {};
            if (::fstat(
                    writer_lock_descriptor_,
                    &information) != 0 ||
                !S_ISREG(information.st_mode) ||
                information.st_nlink != 1) {
                release_writer_lock();
                return false;
            }
            for (;;) {
                if (::flock(
                        writer_lock_descriptor_,
                        LOCK_EX | LOCK_NB) == 0)
                    break;
                if (errno == EINTR) continue;
                release_writer_lock();
                return false;
            }
            writer_lock_device_ = information.st_dev;
            writer_lock_inode_ = information.st_ino;
#endif
            if (!validate_writer_lock()) {
                release_writer_lock();
                return false;
            }
            return true;
        } catch (...) {
            release_writer_lock();
            return false;
        }
    }

    [[nodiscard]] bool validate_writer_lock() const noexcept {
        try {
#ifdef _WIN32
            if (writer_lock_handle_ == INVALID_HANDLE_VALUE)
                return false;
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            BY_HANDLE_FILE_INFORMATION identity{};
            const auto held_valid =
                GetFileType(writer_lock_handle_) == FILE_TYPE_DISK &&
                GetFileInformationByHandle(
                    writer_lock_handle_, &identity) &&
                GetFileInformationByHandleEx(
                    writer_lock_handle_,
                    FileAttributeTagInfo,
                    &attributes,
                    sizeof(attributes)) &&
                (attributes.FileAttributes &
                 (FILE_ATTRIBUTE_DIRECTORY |
                  FILE_ATTRIBUTE_REPARSE_POINT)) == 0u &&
                identity.nNumberOfLinks == 1u &&
                same_windows_file_identity(
                    identity, writer_lock_identity_);
            if (!held_valid ||
                parent_output_handle_ == INVALID_HANDLE_VALUE ||
                writer_lock_name_.empty())
                return false;
            HANDLE named = INVALID_HANDLE_VALUE;
            if (open_windows_directory_child(
                    parent_output_handle_,
                    writer_lock_name_,
                    FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                        FILE_SHARE_DELETE,
                    FILE_OPEN,
                    named) != WindowsRelativeOpenResult::Opened)
                return false;
            FILE_ATTRIBUTE_TAG_INFO named_attributes{};
            BY_HANDLE_FILE_INFORMATION named_identity{};
            const auto named_valid =
                GetFileType(named) == FILE_TYPE_DISK &&
                GetFileInformationByHandle(
                    named, &named_identity) &&
                GetFileInformationByHandleEx(
                    named,
                    FileAttributeTagInfo,
                    &named_attributes,
                    sizeof(named_attributes)) &&
                (named_attributes.FileAttributes &
                 (FILE_ATTRIBUTE_DIRECTORY |
                  FILE_ATTRIBUTE_REPARSE_POINT)) == 0u &&
                named_identity.nNumberOfLinks == 1u &&
                same_windows_file_identity(
                    named_identity, writer_lock_identity_);
            static_cast<void>(CloseHandle(named));
            return named_valid;
#else
            if (writer_lock_descriptor_ < 0 ||
                parent_output_descriptor_ < 0 ||
                writer_lock_name_.empty())
                return false;
            struct stat held_information {};
            struct stat named_information {};
            return ::fstat(
                       writer_lock_descriptor_,
                       &held_information) == 0 &&
                   ::fstatat(
                       parent_output_descriptor_,
                       writer_lock_name_.c_str(),
                       &named_information,
                       AT_SYMLINK_NOFOLLOW) == 0 &&
                   S_ISREG(held_information.st_mode) &&
                   S_ISREG(named_information.st_mode) &&
                   held_information.st_nlink == 1 &&
                   named_information.st_nlink == 1 &&
                   held_information.st_dev ==
                       writer_lock_device_ &&
                   held_information.st_ino ==
                       writer_lock_inode_ &&
                   named_information.st_dev ==
                       writer_lock_device_ &&
                   named_information.st_ino ==
                       writer_lock_inode_;
#endif
        } catch (...) {
            return false;
        }
    }

    void release_writer_lock() noexcept {
#ifdef _WIN32
        if (writer_lock_handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(writer_lock_handle_));
            writer_lock_handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (writer_lock_descriptor_ >= 0) {
            static_cast<void>(
                ::flock(writer_lock_descriptor_, LOCK_UN));
            static_cast<void>(::close(writer_lock_descriptor_));
            writer_lock_descriptor_ = -1;
        }
#endif
        writer_lock_name_.clear();
    }

    [[nodiscard]] bool open_temporary_output() noexcept {
        if (!options_.jsonl_path ||
            options_.jsonl_path->filename().empty())
            return false;
#ifdef _WIN32
        if (windows_reserved_disk_filename(
                *options_.jsonl_path))
            return false;
#endif
        if (!open_output_parent()) return false;
        if (!validate_existing_output_target() ||
            !acquire_writer_lock())
            return false;
        static std::atomic<std::uint64_t> next_identity{0u};
        const auto identity =
            next_identity.fetch_add(
                1u, std::memory_order_relaxed);
#ifdef _WIN32
        const auto process_identity =
            static_cast<std::uint64_t>(GetCurrentProcessId());
#else
        const auto process_identity =
            static_cast<std::uint64_t>(::getpid());
#endif
        const auto clock_identity =
            static_cast<std::uint64_t>(
                SteadyClock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0u;
             attempt < 64u;
             ++attempt) {
            if (!validate_output_parent()) {
                temporary_output_name_.clear();
                return false;
            }
            temporary_output_name_ = final_output_name_;
            temporary_output_name_ +=
                ".katana-telemetry-tmp-" +
                std::to_string(process_identity) + "-" +
                std::to_string(clock_identity) + "-" +
                std::to_string(identity) + "-" +
                std::to_string(attempt);
#ifdef _WIN32
            const auto opened = open_windows_directory_child(
                parent_output_handle_,
                temporary_output_name_,
                GENERIC_WRITE | FILE_READ_ATTRIBUTES |
                    DELETE | SYNCHRONIZE,
                FILE_SHARE_READ,
                FILE_CREATE,
                output_handle_);
            if (opened !=
                WindowsRelativeOpenResult::Opened) {
                if (opened ==
                    WindowsRelativeOpenResult::NameCollision)
                    continue;
                temporary_output_name_.clear();
                return false;
            }
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (GetFileType(output_handle_) != FILE_TYPE_DISK ||
                !GetFileInformationByHandle(
                    output_handle_,
                    &output_identity_) ||
                !GetFileInformationByHandleEx(
                    output_handle_,
                    FileAttributeTagInfo,
                    &attributes,
                    sizeof(attributes)) ||
                (attributes.FileAttributes &
                 (FILE_ATTRIBUTE_DIRECTORY |
                  FILE_ATTRIBUTE_REPARSE_POINT)) != 0u ||
                output_identity_.nNumberOfLinks != 1u) {
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = TRUE;
                static_cast<void>(SetFileInformationByHandle(
                    output_handle_,
                    FileDispositionInfo,
                    &disposition,
                    sizeof(disposition)));
                static_cast<void>(CloseHandle(
                    output_handle_));
                output_handle_ = INVALID_HANDLE_VALUE;
                temporary_output_name_.clear();
                return false;
            }
#else
            auto flags =
                O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            output_descriptor_ =
                ::openat(
                    parent_output_descriptor_,
                    temporary_output_name_.c_str(),
                    flags,
                    0600);
            if (output_descriptor_ < 0) {
                if (errno == EEXIST) continue;
                temporary_output_name_.clear();
                return false;
            }
            struct stat information {};
            if (::fstat(
                    output_descriptor_,
                    &information) != 0 ||
                !S_ISREG(information.st_mode) ||
                information.st_nlink != 1) {
                static_cast<void>(
                    ::close(output_descriptor_));
                output_descriptor_ = -1;
                static_cast<void>(
                    ::unlinkat(
                        parent_output_descriptor_,
                        temporary_output_name_.c_str(),
                        0));
                temporary_output_name_.clear();
                return false;
            }
            output_device_ = information.st_dev;
            output_inode_ = information.st_ino;
#endif
            return true;
        }
        temporary_output_name_.clear();
        return false;
    }

    [[nodiscard]] bool write_output(
        const std::string_view bytes) noexcept {
#ifdef _WIN32
        if (output_handle_ == INVALID_HANDLE_VALUE)
            return false;
        std::size_t written = 0u;
        while (written < bytes.size()) {
            const auto chunk =
                static_cast<DWORD>(
                    std::min<std::size_t>(
                        bytes.size() - written,
                        std::numeric_limits<DWORD>::max()));
            DWORD transferred = 0u;
            if (!WriteFile(
                    output_handle_,
                    bytes.data() + written,
                    chunk,
                    &transferred,
                    nullptr) ||
                transferred == 0u)
                return false;
            written += transferred;
        }
        return true;
#else
        if (output_descriptor_ < 0) return false;
        std::size_t written = 0u;
        while (written < bytes.size()) {
            const auto transferred =
                ::write(
                    output_descriptor_,
                    bytes.data() + written,
                    bytes.size() - written);
            if (transferred < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (transferred == 0) return false;
            written +=
                static_cast<std::size_t>(transferred);
        }
        return true;
#endif
    }

    [[nodiscard]] bool publish_temporary_output() noexcept {
        if (!options_.jsonl_path ||
            temporary_output_name_.empty() ||
            final_output_name_.empty() ||
            !validate_output_parent() ||
            !validate_writer_lock())
            return false;
#ifdef _WIN32
        if (output_handle_ == INVALID_HANDLE_VALUE)
            return false;
        if (!FlushFileBuffers(output_handle_))
            return false;
        FILE_ATTRIBUTE_TAG_INFO temporary_attributes{};
        BY_HANDLE_FILE_INFORMATION temporary_identity{};
        if (GetFileType(output_handle_) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(
                output_handle_,
                &temporary_identity) ||
            !GetFileInformationByHandleEx(
                output_handle_,
                FileAttributeTagInfo,
                &temporary_attributes,
                sizeof(temporary_attributes)) ||
            (temporary_attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY |
              FILE_ATTRIBUTE_REPARSE_POINT)) != 0u ||
            temporary_identity.dwVolumeSerialNumber !=
                output_identity_.dwVolumeSerialNumber ||
            temporary_identity.nFileIndexHigh !=
                output_identity_.nFileIndexHigh ||
            temporary_identity.nFileIndexLow !=
                output_identity_.nFileIndexLow ||
            temporary_identity.nNumberOfLinks != 1u)
            return false;
        const auto final_name_bytes =
            final_output_name_.native().size() *
            sizeof(wchar_t);
        const auto rename_size =
            offsetof(FILE_RENAME_INFO, FileName) +
            final_name_bytes;
        if (final_name_bytes == 0u ||
            final_name_bytes >
                std::numeric_limits<DWORD>::max() ||
            rename_size >
                std::numeric_limits<DWORD>::max())
            return false;
        std::vector<std::byte> rename_storage(rename_size);
        auto* rename_information =
            reinterpret_cast<FILE_RENAME_INFO*>(
                rename_storage.data());
        rename_information->ReplaceIfExists = TRUE;
        rename_information->RootDirectory =
            parent_output_handle_;
        rename_information->FileNameLength =
            static_cast<DWORD>(final_name_bytes);
        std::memcpy(
            rename_information->FileName,
            final_output_name_.native().data(),
            final_name_bytes);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto rename_address =
            ntdll == nullptr
                ? nullptr
                : GetProcAddress(
                      ntdll, "NtSetInformationFile");
        if (rename_address == nullptr) return false;
        using NtSetInformationFileFunction = NTSTATUS(NTAPI*)(
            HANDLE,
            PIO_STATUS_BLOCK,
            PVOID,
            ULONG,
            FILE_INFORMATION_CLASS);
        NtSetInformationFileFunction nt_set_information_file =
            nullptr;
        static_assert(
            sizeof(nt_set_information_file) ==
            sizeof(rename_address));
        std::memcpy(
            &nt_set_information_file,
            &rename_address,
            sizeof(rename_address));
        IO_STATUS_BLOCK rename_status_block{};
        constexpr auto file_rename_information =
            static_cast<FILE_INFORMATION_CLASS>(10);
        const auto rename_status = nt_set_information_file(
            output_handle_,
            &rename_status_block,
            rename_information,
            static_cast<ULONG>(rename_size),
            file_rename_information);
        if (rename_status < 0)
            return false;
        output_renamed_ = true;
        if (!validate_output_parent()) return false;
        HANDLE published = INVALID_HANDLE_VALUE;
        if (open_windows_directory_child(
                parent_output_handle_,
                final_output_name_,
                FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE,
                FILE_OPEN,
                published) !=
            WindowsRelativeOpenResult::Opened)
            return false;
        FILE_ATTRIBUTE_TAG_INFO published_attributes{};
        BY_HANDLE_FILE_INFORMATION published_identity{};
        const auto valid =
            GetFileType(published) == FILE_TYPE_DISK &&
            GetFileInformationByHandle(
                published, &published_identity) &&
            GetFileInformationByHandleEx(
                published,
                FileAttributeTagInfo,
                &published_attributes,
                sizeof(published_attributes)) &&
            (published_attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY |
              FILE_ATTRIBUTE_REPARSE_POINT)) == 0u &&
            published_identity.dwVolumeSerialNumber ==
                output_identity_.dwVolumeSerialNumber &&
            published_identity.nFileIndexHigh ==
                output_identity_.nFileIndexHigh &&
            published_identity.nFileIndexLow ==
                output_identity_.nFileIndexLow &&
            published_identity.nNumberOfLinks == 1u;
        static_cast<void>(CloseHandle(published));
        if (!valid) return false;
        static_cast<void>(CloseHandle(output_handle_));
        output_handle_ = INVALID_HANDLE_VALUE;
        static_cast<void>(CloseHandle(parent_output_handle_));
        parent_output_handle_ = INVALID_HANDLE_VALUE;
#else
        if (output_descriptor_ < 0) return false;
        if (::fsync(output_descriptor_) != 0)
            return false;
        struct stat descriptor_information {};
        struct stat path_information {};
        if (::fstat(
                output_descriptor_,
                &descriptor_information) != 0 ||
            ::fstatat(
                parent_output_descriptor_,
                temporary_output_name_.c_str(),
                &path_information,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(descriptor_information.st_mode) ||
            !S_ISREG(path_information.st_mode) ||
            descriptor_information.st_dev != output_device_ ||
            descriptor_information.st_ino != output_inode_ ||
            descriptor_information.st_nlink != 1 ||
            path_information.st_dev != output_device_ ||
            path_information.st_ino != output_inode_ ||
            path_information.st_nlink != 1)
            return false;
        if (::renameat(
                parent_output_descriptor_,
                temporary_output_name_.c_str(),
                parent_output_descriptor_,
                final_output_name_.c_str()) != 0)
            return false;
        output_renamed_ = true;
        struct stat published_information {};
        if (::fstatat(
                parent_output_descriptor_,
                final_output_name_.c_str(),
                &published_information,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(published_information.st_mode) ||
            published_information.st_dev != output_device_ ||
            published_information.st_ino != output_inode_ ||
            published_information.st_nlink != 1 ||
            !validate_output_parent())
            return false;
        const auto durable =
            ::fsync(parent_output_descriptor_) == 0;
        if (!durable) return false;
        if (::close(output_descriptor_) != 0) {
            output_descriptor_ = -1;
            return false;
        }
        output_descriptor_ = -1;
        static_cast<void>(::close(
            parent_output_descriptor_));
        parent_output_descriptor_ = -1;
#endif
        temporary_output_name_.clear();
        final_output_name_.clear();
        parent_output_path_.clear();
        return true;
    }

    void discard_temporary_output() noexcept {
#ifdef _WIN32
        if (output_handle_ != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            static_cast<void>(SetFileInformationByHandle(
                output_handle_,
                FileDispositionInfo,
                &disposition,
                sizeof(disposition)));
            static_cast<void>(CloseHandle(output_handle_));
            output_handle_ = INVALID_HANDLE_VALUE;
        }
        if (parent_output_handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(
                parent_output_handle_));
            parent_output_handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (output_descriptor_ >= 0) {
            static_cast<void>(::close(output_descriptor_));
            output_descriptor_ = -1;
        }
        if (!output_renamed_ &&
            parent_output_descriptor_ >= 0 &&
            !temporary_output_name_.empty())
            static_cast<void>(::unlinkat(
                parent_output_descriptor_,
                temporary_output_name_.c_str(),
                0));
        if (parent_output_descriptor_ >= 0) {
            static_cast<void>(::close(
                parent_output_descriptor_));
            parent_output_descriptor_ = -1;
        }
#endif
        temporary_output_name_.clear();
        final_output_name_.clear();
        parent_output_path_.clear();
    }

    [[nodiscard]] bool write_record(
        const Record& record) noexcept {
        try {
            std::string line;
            {
                std::scoped_lock lock(writer_mutex_);
                line = format_record(
                    record, next_sequence_);
                if (line.size() >
                    options_.maximum_record_bytes) {
                    lost_records_ = saturating_add(
                        lost_records_, 1u);
                    if (record.schema ==
                        port_build_terminal_schema)
                        fail_writer_locked(0u);
                    return true;
                }
            }
            line.push_back('\n');
            if (!write_output(line)) {
                std::scoped_lock lock(writer_mutex_);
                fail_writer_locked(
                    static_cast<std::uint64_t>(
                        queue_.size()));
                return false;
            }
            std::scoped_lock lock(writer_mutex_);
            ++next_sequence_;
            ++written_records_;
            return true;
        } catch (...) {
            std::scoped_lock lock(writer_mutex_);
            fail_writer_locked(
                static_cast<std::uint64_t>(queue_.size()));
            return false;
        }
    }

    void writer_loop() noexcept {
        try {
            for (;;) {
                std::optional<Record> next;
                bool terminal = false;
                {
                    std::unique_lock lock(writer_mutex_);
                    writer_changed_.wait(lock, [&] {
                        return !queue_.empty() ||
                               terminal_requested_ ||
                               io_failed_;
                    });
                    if (io_failed_) return;
                    if (!queue_.empty()) {
                        next.emplace(
                            std::move(queue_.front()));
                        queue_.pop_front();
                    } else if (terminal_requested_ &&
                               terminal_request_) {
                        auto terminal_request =
                            *terminal_request_;
                        if (terminal_request.outcome ==
                                PortBuildTerminalOutcome::Completed &&
                            !terminal_stream_complete_locked()) {
                            terminal_request.outcome =
                                PortBuildTerminalOutcome::Failed;
                            terminal_request.exit_code = exit_status(
                                ExitCode::InputOutput);
                        }
                        next.emplace(
                            terminal_record(
                                terminal_request));
                        terminal = true;
                    }
                }
                if (!next || !write_record(*next))
                    return;
                if (!terminal) continue;
                std::scoped_lock lock(writer_mutex_);
                terminal_emitted_ = true;
                accepting_ = false;
                enabled_ = false;
                writer_done_.notify_all();
                return;
            }
        } catch (...) {
            std::scoped_lock lock(writer_mutex_);
            const auto pending =
                static_cast<std::uint64_t>(queue_.size()) +
                (terminal_requested_ && !terminal_emitted_
                     ? 1u
                     : 0u);
            fail_writer_locked(
                std::max<std::uint64_t>(
                    1u, pending));
        }
    }

    PortBuildTelemetryOptions options_;
    const SteadyClock::time_point started_;

    mutable std::mutex writer_mutex_;
    std::mutex finish_mutex_;
    std::condition_variable writer_changed_;
    std::condition_variable writer_done_;
    std::condition_variable producer_done_;
    std::filesystem::path parent_output_path_;
    std::filesystem::path final_output_name_;
    std::filesystem::path temporary_output_name_;
    std::filesystem::path writer_lock_name_;
    bool output_renamed_ = false;
#ifdef _WIN32
    HANDLE parent_output_handle_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION parent_output_identity_{};
    HANDLE output_handle_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION output_identity_{};
    HANDLE writer_lock_handle_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION writer_lock_identity_{};
#else
    int parent_output_descriptor_ = -1;
    dev_t parent_output_device_{};
    ino_t parent_output_inode_{};
    int output_descriptor_ = -1;
    dev_t output_device_{};
    ino_t output_inode_{};
    int writer_lock_descriptor_ = -1;
    dev_t writer_lock_device_{};
    ino_t writer_lock_inode_{};
#endif
    std::deque<Record> queue_;
    // Protected by writer_mutex_. This is the elapsed timestamp of the last
    // admitted queue or terminal record, not of the last producer which began
    // formatting a record.
    std::uint64_t last_admitted_elapsed_ms_ = 0u;
    std::thread worker_;
    std::optional<TerminalRequest> terminal_request_;
    bool enabled_ = false;
    bool accepting_ = false;
    bool closing_ = false;
    bool terminal_requested_ = false;
    bool terminal_emitted_ = false;
    bool published_ = false;
    bool io_failed_ = false;
    std::uint64_t next_sequence_ = 0u;
    std::uint64_t written_records_ = 0u;
    std::uint64_t lost_records_ = 0u;
    std::size_t active_producers_ = 0u;
    std::atomic<std::uint64_t>
        upstream_dropped_observations_{0u};
    std::atomic_bool upstream_incomplete_{false};
    std::string upstream_incomplete_reason_;
    bool resolved_environment_recorded_ = false;
    bool resolved_environment_in_flight_ = false;
    bool phase_timings_recorded_ = false;
    bool phase_timings_in_flight_ = false;

    std::mutex resource_mutex_;
    SteadyClock::time_point last_resource_sample_{};
    std::optional<SteadyClock::time_point>
        previous_resource_sample_;
    std::uint64_t previous_total_cpu_ms_ = 0u;
    std::uint64_t inspect_logical_processors_ = 1u;
    std::uint64_t observed_peak_working_set_ = 0u;
    std::uint64_t observed_peak_private_commit_ = 0u;
    ResourceValues retired_tree_cumulative_;
    ResourceValues last_tree_capture_;
    bool retired_tree_present_ = false;
    bool retired_tree_complete_ = true;
    bool active_tree_complete_ = true;
    PortBuildGpuResourceSample gpu_sample_;
#ifdef _WIN32
    std::uintptr_t windows_job_handle_ = 0u;
#else
    std::int64_t posix_process_group_ = -1;
    std::vector<std::int64_t> posix_additional_processes_;
    bool posix_final_sample_present_ = false;
#if defined(__linux__)
    bool retired_posix_descendants_present_ = false;
#endif
#endif
};

PortBuildTelemetryRecorder::PortBuildTelemetryRecorder(
    PortBuildTelemetryOptions options) noexcept {
    try {
        impl_ =
            std::make_unique<Impl>(std::move(options));
    } catch (...) {
        impl_.reset();
    }
}

PortBuildTelemetryRecorder::~PortBuildTelemetryRecorder() noexcept =
    default;

bool PortBuildTelemetryRecorder::enabled() const noexcept {
    return impl_ != nullptr && impl_->enabled();
}

ProgressCallback
PortBuildTelemetryRecorder::progress_callback() noexcept {
    try {
        return [this](const ProgressEvent& event) noexcept {
            observe_progress(event);
        };
    } catch (...) {
        return {};
    }
}

void PortBuildTelemetryRecorder::observe_progress(
    const ProgressEvent& event) noexcept {
    if (impl_ != nullptr) impl_->observe_progress(event);
}

bool PortBuildTelemetryRecorder::record_resolved_environment(
    PortBuildResolvedEnvironment environment) noexcept {
    return impl_ != nullptr &&
           impl_->record_resolved_environment(
               std::move(environment));
}

bool PortBuildTelemetryRecorder::record_host_command(
    PortBuildHostCommandObservation observation) noexcept {
    return impl_ != nullptr &&
           impl_->record_host_command(std::move(observation));
}

bool PortBuildTelemetryRecorder::record_phase_timings(
    const std::uint64_t total_ms,
    const std::span<const PortBuildPhaseTimingSample> samples) noexcept {
    return impl_ != nullptr &&
           impl_->record_phase_timings(total_ms, samples);
}

bool PortBuildTelemetryRecorder::mark_upstream_incomplete(
    const std::string_view reason,
    const std::uint64_t cumulative_dropped_observations) noexcept {
    return impl_ != nullptr &&
           impl_->mark_upstream_incomplete(
               reason, cumulative_dropped_observations);
}

void PortBuildTelemetryRecorder::sample_resources(
    const std::string_view phase) noexcept {
    if (impl_ != nullptr) impl_->sample_resources(phase);
}

void PortBuildTelemetryRecorder::set_gpu_resource_sample(
    PortBuildGpuResourceSample sample) noexcept {
    if (impl_ != nullptr)
        impl_->set_gpu_resource_sample(std::move(sample));
}

#ifdef _WIN32
void PortBuildTelemetryRecorder::register_windows_job(
    const std::uintptr_t native_job_handle) noexcept {
    if (impl_ != nullptr)
        impl_->register_windows_job(native_job_handle);
}
#else
void PortBuildTelemetryRecorder::register_posix_process_group(
    const std::int64_t process_group) noexcept {
    if (impl_ != nullptr)
        impl_->register_posix_process_group(process_group);
}

void PortBuildTelemetryRecorder::update_posix_process_tree_members(
    const std::span<const std::int64_t> process_ids) noexcept {
    if (impl_ != nullptr)
        impl_->update_posix_process_tree_members(process_ids);
}

void PortBuildTelemetryRecorder::
    record_posix_process_tree_final_sample(
        const PortBuildPosixProcessTreeFinalSample&
            sample) noexcept {
    if (impl_ != nullptr)
        impl_->record_posix_process_tree_final_sample(
            sample);
}
#endif

void PortBuildTelemetryRecorder::clear_process_tree() noexcept {
    if (impl_ != nullptr) impl_->clear_process_tree();
}

void PortBuildTelemetryRecorder::finish(
    const PortBuildTerminalOutcome outcome,
    const int exit_code,
    const std::string_view terminal_phase) noexcept {
    if (impl_ != nullptr)
        impl_->finish(outcome, exit_code, terminal_phase);
}

PortBuildTelemetryStatus
PortBuildTelemetryRecorder::status() const noexcept {
    if (impl_ != nullptr) return impl_->status();
    return {false, false, false, true, 0u, 0u, 0u, false};
}

std::string_view port_build_terminal_outcome_name(
    const PortBuildTerminalOutcome outcome) noexcept {
    switch (outcome) {
    case PortBuildTerminalOutcome::Completed:
        return "completed";
    case PortBuildTerminalOutcome::Failed:
        return "failed";
    case PortBuildTerminalOutcome::Cancelled:
        return "cancelled";
    case PortBuildTerminalOutcome::Abandoned:
        return "abandoned";
    }
    return "unknown";
}

} // namespace katana::cli
