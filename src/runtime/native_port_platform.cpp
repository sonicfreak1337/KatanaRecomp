#include "katana/runtime/native_port_platform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>
#include <xinput.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::size_t maximum_platform_identifier_bytes = 128u;
constexpr std::size_t maximum_save_slot_bytes = 64u;
constexpr std::uint64_t maximum_content_budget_bytes =
    64ull * 1024u * 1024u * 1024u;
constexpr std::uint32_t maximum_save_budget_bytes = 64u * 1024u * 1024u;

void saturating_increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

void saturating_add(std::uint64_t& value,
                    const std::uint64_t addition) noexcept {
    value = addition > std::numeric_limits<std::uint64_t>::max() - value
                ? std::numeric_limits<std::uint64_t>::max()
                : value + addition;
}

[[nodiscard]] bool valid_identifier(const std::string_view value,
                                    const std::size_t maximum_bytes) noexcept {
    if (value.empty() || value.size() > maximum_bytes) return false;
    return std::all_of(
        value.begin(), value.end(), [](const unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '-' || character == '_' || character == '.';
        });
}

[[nodiscard]] bool valid_path_identifier(
    const std::string_view value,
    const std::size_t maximum_bytes) noexcept {
    if (!valid_identifier(value, maximum_bytes) || value.front() == '.' ||
        value.back() == '.' || value.find("..") != std::string_view::npos)
        return false;
    const auto dot = value.find('.');
    const auto base = value.substr(0u, dot);
    std::string uppercase;
    uppercase.reserve(base.size());
    for (const auto character : base)
        uppercase.push_back(character >= 'a' && character <= 'z'
                                ? static_cast<char>(character - 'a' + 'A')
                                : character);
    if (uppercase == "CON" || uppercase == "PRN" || uppercase == "AUX" ||
        uppercase == "NUL")
        return false;
    if (uppercase.size() == 4u &&
        (uppercase.starts_with("COM") || uppercase.starts_with("LPT")) &&
        uppercase[3] >= '1' && uppercase[3] <= '9')
        return false;
    return true;
}

[[nodiscard]] bool valid_content_relative_path(
    const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) return false;
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..")
            return false;
    }
    return true;
}

[[nodiscard]] std::string copy_validated_identifier(
    const std::string_view value,
    const std::size_t maximum_bytes,
    const char* const operation) {
    if (!valid_identifier(value, maximum_bytes))
        throw NativePortPlatformError(
            NativePortPlatformFailure::InvalidConfig, 0u, operation);
    return std::string(value);
}

[[nodiscard]] std::string copy_validated_path_identifier(
    const std::string_view value,
    const std::size_t maximum_bytes,
    const char* const operation) {
    if (!valid_path_identifier(value, maximum_bytes))
        throw NativePortPlatformError(
            NativePortPlatformFailure::InvalidConfig, 0u, operation);
    return std::string(value);
}

} // namespace

#ifdef _WIN32

namespace {

class UniqueHandle final {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(value_, INVALID_HANDLE_VALUE);
    }
    void reset(const HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
        if (*this) CloseHandle(value_);
        value_ = replacement;
    }

  private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class UniqueModule final {
  public:
    UniqueModule() = default;
    explicit UniqueModule(const HMODULE value) noexcept : value_(value) {}
    ~UniqueModule() {
        if (value_ != nullptr) FreeLibrary(value_);
    }
    UniqueModule(const UniqueModule&) = delete;
    UniqueModule& operator=(const UniqueModule&) = delete;
    UniqueModule(UniqueModule&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
    UniqueModule& operator=(UniqueModule&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) FreeLibrary(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HMODULE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

  private:
    HMODULE value_ = nullptr;
};

[[noreturn]] void fail_platform(const NativePortPlatformFailure failure,
                                const std::uint32_t code,
                                const char* const operation) {
    throw NativePortPlatformError(
        failure, code == 0u ? 1u : code, operation);
}

[[nodiscard]] std::filesystem::path absolute_normalized(
    const std::filesystem::path& path,
    const char* const operation) {
    try {
        const auto result = std::filesystem::absolute(path).lexically_normal();
        if (result.empty() || result.root_path().empty())
            fail_platform(
                NativePortPlatformFailure::InvalidConfig, 0u, operation);
        return result;
    } catch (const NativePortPlatformError&) {
        throw;
    } catch (...) {
        fail_platform(NativePortPlatformFailure::InvalidConfig,
                      0u,
                      operation);
    }
}

[[nodiscard]] std::filesystem::path extended_path(
    const std::filesystem::path& path) {
    const auto normalized = absolute_normalized(path, "path-normalize");
    const auto native = normalized.native();
    if (native.starts_with(LR"(\\?\)")) return normalized;
    if (native.starts_with(LR"(\\)"))
        return std::filesystem::path(
            std::wstring(LR"(\\?\UNC\)") + native.substr(2u));
    return std::filesystem::path(std::wstring(LR"(\\?\)") + native);
}

[[nodiscard]] DWORD file_attributes(
    const std::filesystem::path& path) noexcept {
    try {
        return GetFileAttributesW(extended_path(path).c_str());
    } catch (...) {
        return INVALID_FILE_ATTRIBUTES;
    }
}

void require_safe_existing_components(
    const std::filesystem::path& path,
    const bool final_must_be_directory,
    const NativePortPlatformFailure failure,
    const char* const operation) {
    const auto normalized = absolute_normalized(path, operation);
    auto current = normalized.root_path();
    for (const auto& component : normalized.relative_path()) {
        if (component.empty() || component == ".") continue;
        if (component == "..") fail_platform(failure, 0u, operation);
        current /= component;
        const auto attributes = file_attributes(current);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            fail_platform(failure, GetLastError(), operation);
        const bool final = current == normalized;
        const bool directory =
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
        if ((!final && !directory) ||
            (final && directory != final_must_be_directory))
            fail_platform(failure, ERROR_DIRECTORY, operation);
    }
}

void ensure_safe_directory_chain(
    const std::filesystem::path& path,
    const NativePortPlatformFailure failure,
    const char* const operation) {
    const auto normalized = absolute_normalized(path, operation);
    auto current = normalized.root_path();
    for (const auto& component : normalized.relative_path()) {
        if (component.empty() || component == ".") continue;
        if (component == "..") fail_platform(failure, 0u, operation);
        current /= component;
        auto attributes = file_attributes(current);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                fail_platform(failure, error, operation);
            if (CreateDirectoryW(extended_path(current).c_str(), nullptr) ==
                    FALSE &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                fail_platform(failure, GetLastError(), operation);
            attributes = file_attributes(current);
        }
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            fail_platform(failure, GetLastError(), operation);
    }
}

[[nodiscard]] UniqueHandle open_directory(
    const std::filesystem::path& path,
    const DWORD sharing,
    const NativePortPlatformFailure failure,
    const char* const operation) {
    const auto handle = CreateFileW(
        extended_path(path).c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        sharing,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        fail_platform(failure, GetLastError(), operation);
    UniqueHandle result(handle);
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(result.get(), &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        fail_platform(failure, GetLastError(), operation);
    return result;
}

[[nodiscard]] std::filesystem::path final_path_from_handle(
    const HANDLE handle,
    const NativePortPlatformFailure failure,
    const char* const operation) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const auto required =
        GetFinalPathNameByHandleW(handle, nullptr, 0u, flags);
    if (required == 0u)
        fail_platform(failure, GetLastError(), operation);
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1u);
    const auto copied = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (copied == 0u || copied >= buffer.size())
        fail_platform(failure, GetLastError(), operation);
    std::wstring value(buffer.data(), copied);
    constexpr std::wstring_view unc_prefix = LR"(\\?\UNC\)";
    constexpr std::wstring_view local_prefix = LR"(\\?\)";
    if (value.starts_with(unc_prefix))
        value = LR"(\\)" + value.substr(unc_prefix.size());
    else if (value.starts_with(local_prefix))
        value.erase(0u, local_prefix.size());
    return std::filesystem::path(std::move(value)).lexically_normal();
}

[[nodiscard]] bool same_component_case_insensitive(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
    const auto& left_native = left.native();
    const auto& right_native = right.native();
    if (left_native.size() > static_cast<std::size_t>(
                                 std::numeric_limits<int>::max()) ||
        right_native.size() > static_cast<std::size_t>(
                                  std::numeric_limits<int>::max()))
        return false;
    return CompareStringOrdinal(
               left_native.c_str(),
               static_cast<int>(left_native.size()),
               right_native.c_str(),
               static_cast<int>(right_native.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool path_is_within_case_insensitive(
    const std::filesystem::path& path,
    const std::filesystem::path& root,
    const bool allow_equal = false) noexcept {
    auto path_component = path.begin();
    for (auto root_component = root.begin(); root_component != root.end();
         ++root_component, ++path_component) {
        if (path_component == path.end() ||
            !same_component_case_insensitive(*path_component,
                                             *root_component))
            return false;
    }
    return allow_equal ? true : path_component != path.end();
}

[[nodiscard]] std::filesystem::path canonical_directory_from_handle(
    const UniqueHandle& handle,
    const NativePortPlatformFailure failure,
    const char* const operation) {
    const auto result = final_path_from_handle(handle.get(), failure, operation);
    require_safe_existing_components(result, true, failure, operation);
    return result;
}

class Sha256 final {
  public:
    Sha256() try {
        auto status = BCryptOpenAlgorithmProvider(
            &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0u);
        if (status < 0)
            fail_platform(NativePortPlatformFailure::ResourceLimit,
                          static_cast<std::uint32_t>(status),
                          "sha256-provider");
        DWORD copied = 0u;
        DWORD object_bytes = 0u;
        status = BCryptGetProperty(
            algorithm_,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_bytes),
            sizeof(object_bytes),
            &copied,
            0u);
        if (status < 0 || copied != sizeof(object_bytes) || object_bytes == 0u)
            fail_platform(NativePortPlatformFailure::ResourceLimit,
                          static_cast<std::uint32_t>(status),
                          "sha256-object-size");
        object_.resize(object_bytes);
        status = BCryptCreateHash(algorithm_,
                                  &hash_,
                                  object_.data(),
                                  static_cast<ULONG>(object_.size()),
                                  nullptr,
                                  0u,
                                  0u);
        if (status < 0)
            fail_platform(NativePortPlatformFailure::ResourceLimit,
                          static_cast<std::uint32_t>(status),
                          "sha256-create");
    } catch (...) {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0u);
        throw;
    }

    ~Sha256() {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0u);
    }

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const void* const bytes, std::size_t byte_count) {
        const auto* cursor = static_cast<const std::uint8_t*>(bytes);
        while (byte_count != 0u) {
            const auto chunk = static_cast<ULONG>(
                std::min<std::size_t>(byte_count,
                                      std::numeric_limits<ULONG>::max()));
            const auto status = BCryptHashData(
                hash_, const_cast<PUCHAR>(cursor), chunk, 0u);
            if (status < 0)
                fail_platform(NativePortPlatformFailure::ResourceLimit,
                              static_cast<std::uint32_t>(status),
                              "sha256-update");
            cursor += chunk;
            byte_count -= chunk;
        }
    }

    template <typename T>
    void update_scalar(const T value) {
        static_assert(std::is_integral_v<T>);
        std::array<std::uint8_t, sizeof(T)> bytes{};
        using Unsigned = std::make_unsigned_t<T>;
        const auto unsigned_value = static_cast<Unsigned>(value);
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>(
                unsigned_value >> (index * 8u));
        update(bytes.data(), bytes.size());
    }

    [[nodiscard]] std::array<std::byte, 32u> finish() {
        std::array<std::byte, 32u> result{};
        const auto status = BCryptFinishHash(
            hash_,
            reinterpret_cast<PUCHAR>(result.data()),
            static_cast<ULONG>(result.size()),
            0u);
        if (status < 0)
            fail_platform(NativePortPlatformFailure::ResourceLimit,
                          static_cast<std::uint32_t>(status),
                          "sha256-finish");
        BCryptDestroyHash(hash_);
        hash_ = nullptr;
        return result;
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<std::uint8_t> object_;
};

[[nodiscard]] std::string digest_identity(
    const std::array<std::byte, 32u>& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(7u + digest.size() * 2u);
    for (const auto byte : digest) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        result.push_back(digits[value >> 4u]);
        result.push_back(digits[value & 0x0Fu]);
    }
    return result;
}

void seek_handle(const HANDLE handle,
                 const std::uint64_t offset,
                 const NativePortPlatformFailure failure,
                 const char* const operation) {
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<LONGLONG>::max()))
        fail_platform(failure, ERROR_ARITHMETIC_OVERFLOW, operation);
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(handle, target, nullptr, FILE_BEGIN) == FALSE)
        fail_platform(failure, GetLastError(), operation);
}

void read_handle_exact(const HANDLE handle,
                       const std::uint64_t offset,
                       const std::span<std::byte> destination,
                       const NativePortPlatformFailure failure,
                       const char* const operation) {
    if (destination.empty()) return;
    seek_handle(handle, offset, failure, operation);
    auto* cursor = destination.data();
    auto remaining = destination.size();
    while (remaining != 0u) {
        const auto request = static_cast<DWORD>(
            std::min<std::size_t>(remaining,
                                  std::numeric_limits<DWORD>::max()));
        DWORD copied = 0u;
        if (ReadFile(handle, cursor, request, &copied, nullptr) == FALSE ||
            copied == 0u)
            fail_platform(failure, GetLastError(), operation);
        cursor += copied;
        remaining -= copied;
    }
}

void write_handle_exact(const HANDLE handle,
                        const std::span<const std::byte> source,
                        const NativePortPlatformFailure failure,
                        const char* const operation) {
    const auto* cursor = source.data();
    auto remaining = source.size();
    while (remaining != 0u) {
        const auto request = static_cast<DWORD>(
            std::min<std::size_t>(remaining,
                                  std::numeric_limits<DWORD>::max()));
        DWORD copied = 0u;
        if (WriteFile(handle, cursor, request, &copied, nullptr) == FALSE ||
            copied == 0u)
            fail_platform(failure, GetLastError(), operation);
        cursor += copied;
        remaining -= copied;
    }
}

struct PlatformTelemetry final {
    NativePortPlatformSnapshot snapshot;
};

} // namespace

class NativePortReadOnlyFile::Impl final {
  public:
    Impl(UniqueHandle handle,
         std::string logical_id,
         const std::uint64_t source_offset,
         const std::uint64_t byte_size,
         std::shared_ptr<PlatformTelemetry> telemetry)
        : handle_(std::move(handle)),
          logical_id_(std::move(logical_id)),
          source_offset_(source_offset),
          byte_size_(byte_size),
          telemetry_(std::move(telemetry)),
          owner_thread_(std::this_thread::get_id()) {
        snapshot_.byte_size = byte_size_;
    }

    ~Impl() noexcept {
        if (std::this_thread::get_id() != owner_thread_) std::terminate();
    }

    [[nodiscard]] std::string_view logical_id() const noexcept {
        return logical_id_;
    }
    [[nodiscard]] std::uint64_t byte_size() const noexcept {
        return byte_size_;
    }
    void read_at(const std::uint64_t offset,
                 const std::span<std::byte> destination) {
        require_owner_thread();
        if (offset > byte_size_ ||
            destination.size() > byte_size_ - offset ||
            offset > std::numeric_limits<std::uint64_t>::max() -
                         source_offset_)
            fail_platform(NativePortPlatformFailure::ContentRead,
                          ERROR_HANDLE_EOF,
                          "content-read-range");
        if (destination.empty()) return;
        read_handle_exact(handle_.get(),
                          source_offset_ + offset,
                          destination,
                          NativePortPlatformFailure::ContentRead,
                          "content-read");
        saturating_increment(snapshot_.read_operations);
        saturating_add(snapshot_.bytes_read, destination.size());
        saturating_increment(
            telemetry_->snapshot.content_read_operations);
        saturating_add(telemetry_->snapshot.content_bytes_read,
                       destination.size());
    }
    [[nodiscard]] NativePortContentFileSnapshot snapshot() const {
        require_owner_thread();
        return snapshot_;
    }

  private:
    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_)
            fail_platform(NativePortPlatformFailure::ThreadViolation,
                          ERROR_INVALID_THREAD_ID,
                          "content-thread");
    }

    UniqueHandle handle_;
    std::string logical_id_;
    std::uint64_t source_offset_ = 0u;
    std::uint64_t byte_size_ = 0u;
    std::shared_ptr<PlatformTelemetry> telemetry_;
    std::thread::id owner_thread_;
    NativePortContentFileSnapshot snapshot_;
};

namespace {

using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

struct XInputApi final {
    UniqueModule module;
    XInputGetStateFunction get_state = nullptr;
    XInputSetStateFunction set_state = nullptr;
};

[[nodiscard]] std::filesystem::path system_directory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const auto copied =
            GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (copied == 0u)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          GetLastError(),
                          "input-system-directory");
        if (copied < buffer.size())
            return std::filesystem::path(
                std::wstring(buffer.data(), copied));
        buffer.resize(static_cast<std::size_t>(copied) + 1u);
    }
}

[[nodiscard]] XInputApi load_xinput() {
    constexpr std::array names{
        L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"};
    const auto root = system_directory();
    for (const auto* const name : names) {
        UniqueModule module(
            LoadLibraryExW((root / name).c_str(), nullptr, 0u));
        if (!module) continue;
        const auto get_state = reinterpret_cast<XInputGetStateFunction>(
            GetProcAddress(module.get(), "XInputGetState"));
        const auto set_state = reinterpret_cast<XInputSetStateFunction>(
            GetProcAddress(module.get(), "XInputSetState"));
        if (get_state != nullptr && set_state != nullptr)
            return {std::move(module), get_state, set_state};
    }
    return {};
}

[[nodiscard]] float normalized_axis(const SHORT value) noexcept {
    return value < 0
               ? std::max(-1.0f, static_cast<float>(value) / 32'768.0f)
               : std::min(1.0f, static_cast<float>(value) / 32'767.0f);
}

[[nodiscard]] std::uint32_t native_buttons(const WORD value) noexcept {
    std::uint32_t result = 0u;
    const auto copy = [&](const WORD source,
                          const NativePortGamepadButton destination) {
        if ((value & source) != 0u)
            result |= native_port_gamepad_button_mask(destination);
    };
    copy(XINPUT_GAMEPAD_DPAD_UP, NativePortGamepadButton::DpadUp);
    copy(XINPUT_GAMEPAD_DPAD_DOWN, NativePortGamepadButton::DpadDown);
    copy(XINPUT_GAMEPAD_DPAD_LEFT, NativePortGamepadButton::DpadLeft);
    copy(XINPUT_GAMEPAD_DPAD_RIGHT, NativePortGamepadButton::DpadRight);
    copy(XINPUT_GAMEPAD_START, NativePortGamepadButton::Menu);
    copy(XINPUT_GAMEPAD_BACK, NativePortGamepadButton::View);
    copy(XINPUT_GAMEPAD_LEFT_THUMB, NativePortGamepadButton::LeftStick);
    copy(XINPUT_GAMEPAD_RIGHT_THUMB, NativePortGamepadButton::RightStick);
    copy(XINPUT_GAMEPAD_LEFT_SHOULDER,
         NativePortGamepadButton::LeftShoulder);
    copy(XINPUT_GAMEPAD_RIGHT_SHOULDER,
         NativePortGamepadButton::RightShoulder);
    copy(XINPUT_GAMEPAD_A, NativePortGamepadButton::A);
    copy(XINPUT_GAMEPAD_B, NativePortGamepadButton::B);
    copy(XINPUT_GAMEPAD_X, NativePortGamepadButton::X);
    copy(XINPUT_GAMEPAD_Y, NativePortGamepadButton::Y);
    return result;
}

constexpr std::array<std::byte, 16u> save_magic{
    std::byte{'K'}, std::byte{'A'}, std::byte{'T'}, std::byte{'A'},
    std::byte{'N'}, std::byte{'A'}, std::byte{'S'}, std::byte{'A'},
    std::byte{'V'}, std::byte{'E'}, std::byte{0x0D}, std::byte{0x0A},
    std::byte{0x1A}, std::byte{0x0A}, std::byte{0x00}, std::byte{0x01}};
constexpr std::uint32_t save_format_version = 2u;
constexpr std::size_t save_header_bytes = 80u;

template <typename T>
void write_little(std::span<std::byte> destination,
                  const std::size_t offset,
                  const T value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > destination.size() || sizeof(T) > destination.size() - offset)
        fail_platform(NativePortPlatformFailure::SaveWrite,
                      ERROR_ARITHMETIC_OVERFLOW,
                      "save-header-write");
    for (std::size_t index = 0u; index < sizeof(T); ++index)
        destination[offset + index] = std::byte(
            static_cast<std::uint8_t>(value >> (index * 8u)));
}

template <typename T>
[[nodiscard]] T read_little(const std::span<const std::byte> source,
                            const std::size_t offset) noexcept {
    static_assert(std::is_unsigned_v<T>);
    if (offset > source.size() || sizeof(T) > source.size() - offset)
        return 0u;
    T result = 0u;
    for (std::size_t index = 0u; index < sizeof(T); ++index)
        result |= static_cast<T>(std::to_integer<std::uint8_t>(
                      source[offset + index]))
                  << (index * 8u);
    return result;
}

[[nodiscard]] std::array<std::byte, 32u> save_digest(
    const std::string_view project_id,
    const std::string_view slot_id,
    const std::uint32_t schema_version,
    const std::uint64_t generation,
    const std::span<const std::byte> payload) {
    constexpr std::string_view domain = "katana-native-save-v2";
    Sha256 hash;
    hash.update_scalar<std::uint32_t>(
        static_cast<std::uint32_t>(domain.size()));
    hash.update(domain.data(), domain.size());
    hash.update_scalar<std::uint32_t>(
        static_cast<std::uint32_t>(project_id.size()));
    hash.update(project_id.data(), project_id.size());
    hash.update_scalar<std::uint32_t>(
        static_cast<std::uint32_t>(slot_id.size()));
    hash.update(slot_id.data(), slot_id.size());
    hash.update_scalar(schema_version);
    hash.update_scalar(generation);
    hash.update_scalar<std::uint64_t>(payload.size());
    hash.update(payload.data(), payload.size());
    return hash.finish();
}

[[nodiscard]] std::vector<std::byte> encode_save(
    const std::string_view project_id,
    const NativePortSaveKey& key,
    const std::uint64_t generation,
    const std::span<const std::byte> payload) {
    if (payload.size() >
        std::numeric_limits<std::size_t>::max() - save_header_bytes)
        fail_platform(NativePortPlatformFailure::ResourceLimit,
                      ERROR_ARITHMETIC_OVERFLOW,
                      "save-encode-size");
    std::vector<std::byte> result(save_header_bytes + payload.size());
    std::copy(save_magic.begin(), save_magic.end(), result.begin());
    auto bytes = std::span(result);
    write_little<std::uint32_t>(bytes, 16u, save_format_version);
    write_little<std::uint32_t>(bytes, 20u,
                                static_cast<std::uint32_t>(save_header_bytes));
    write_little<std::uint32_t>(bytes, 24u, key.schema_version);
    write_little<std::uint32_t>(bytes, 28u, 0u);
    write_little<std::uint64_t>(bytes, 32u, generation);
    write_little<std::uint64_t>(bytes, 40u, payload.size());
    const auto digest = save_digest(
        project_id, key.slot_id, key.schema_version, generation, payload);
    std::copy(digest.begin(), digest.end(), result.begin() + 48u);
    std::copy(payload.begin(), payload.end(),
              result.begin() + static_cast<std::ptrdiff_t>(save_header_bytes));
    return result;
}

struct SaveProbe final {
    NativePortSaveLoadStatus status = NativePortSaveLoadStatus::Missing;
    std::uint32_t stored_schema_version = 0u;
    std::uint64_t generation = 0u;
    std::vector<std::byte> payload;
    std::uint64_t bytes_read = 0u;
};

[[nodiscard]] bool path_missing(const std::filesystem::path& path) noexcept {
    const auto attributes = file_attributes(path);
    if (attributes != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

void require_safe_optional_regular_file(
    const std::filesystem::path& path,
    const NativePortPlatformFailure failure,
    const char* const operation) {
    const auto attributes = file_attributes(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return;
        fail_platform(failure, error, operation);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        fail_platform(failure, ERROR_REPARSE_TAG_INVALID, operation);
}

[[nodiscard]] SaveProbe probe_save_file(
    const std::filesystem::path& path,
    const std::filesystem::path& save_root,
    const std::string_view project_id,
    const NativePortSaveKey& key,
    const std::uint32_t maximum_payload_bytes) {
    require_safe_optional_regular_file(
        path, NativePortPlatformFailure::SaveBoundary, "save-path");
    if (path_missing(path)) return {};
    const auto handle = CreateFileW(
        extended_path(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        fail_platform(NativePortPlatformFailure::SaveRead,
                      GetLastError(),
                      "save-open");
    UniqueHandle locked(handle);
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(locked.get(), &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        fail_platform(NativePortPlatformFailure::SaveBoundary,
                      GetLastError(),
                      "save-handle");
    const auto final_path = final_path_from_handle(
        locked.get(), NativePortPlatformFailure::SaveBoundary, "save-final-path");
    if (!path_is_within_case_insensitive(final_path, save_root))
        fail_platform(NativePortPlatformFailure::SaveBoundary,
                      ERROR_ACCESS_DENIED,
                      "save-root-boundary");
    LARGE_INTEGER size{};
    if (GetFileSizeEx(locked.get(), &size) == FALSE || size.QuadPart < 0)
        fail_platform(NativePortPlatformFailure::SaveRead,
                      GetLastError(),
                      "save-size");
    if (static_cast<std::uint64_t>(size.QuadPart) >
        save_header_bytes +
            static_cast<std::uint64_t>(maximum_payload_bytes))
        return {NativePortSaveLoadStatus::Corrupt};
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    read_handle_exact(locked.get(),
                      0u,
                      bytes,
                      NativePortPlatformFailure::SaveRead,
                      "save-read");
    SaveProbe result;
    result.bytes_read = bytes.size();
    const auto source = std::span<const std::byte>(bytes);
    if (source.size() < save_header_bytes ||
        !std::equal(save_magic.begin(), save_magic.end(), source.begin()) ||
        read_little<std::uint32_t>(source, 16u) != save_format_version ||
        read_little<std::uint32_t>(source, 20u) != save_header_bytes ||
        read_little<std::uint32_t>(source, 28u) != 0u) {
        result.status = NativePortSaveLoadStatus::Corrupt;
        return result;
    }
    result.stored_schema_version = read_little<std::uint32_t>(source, 24u);
    result.generation = read_little<std::uint64_t>(source, 32u);
    const auto payload_bytes = read_little<std::uint64_t>(source, 40u);
    if (result.stored_schema_version == 0u || result.generation == 0u ||
        payload_bytes > maximum_payload_bytes ||
        payload_bytes != source.size() - save_header_bytes) {
        result.status = NativePortSaveLoadStatus::Corrupt;
        return result;
    }
    const auto payload = source.subspan(save_header_bytes);
    const auto expected = save_digest(project_id,
                                      key.slot_id,
                                      result.stored_schema_version,
                                      result.generation,
                                      payload);
    if (!std::equal(expected.begin(), expected.end(), source.begin() + 48u)) {
        result.status = NativePortSaveLoadStatus::Corrupt;
        return result;
    }
    if (result.stored_schema_version != key.schema_version) {
        result.status = NativePortSaveLoadStatus::IncompatibleSchema;
        return result;
    }
    result.status = NativePortSaveLoadStatus::Loaded;
    result.payload.assign(payload.begin(), payload.end());
    return result;
}

[[nodiscard]] std::filesystem::path save_file_path(
    const std::filesystem::path& root,
    const std::string_view slot_id,
    const std::wstring_view suffix) {
    std::wstring name;
    name.reserve(slot_id.size() + suffix.size());
    for (const auto character : slot_id)
        name.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(character)));
    name.append(suffix);
    return root / name;
}

} // namespace

class NativePortPlatformServices::Impl final {
  public:
    explicit Impl(const NativePortPlatformConfig& config)
        : project_id_(copy_validated_path_identifier(
              config.project_id,
              maximum_platform_identifier_bytes,
              "project-id")),
          maximum_content_file_bytes_(config.maximum_content_file_bytes),
          maximum_save_payload_bytes_(config.maximum_save_payload_bytes),
          owner_thread_(std::this_thread::get_id()),
          telemetry_(std::make_shared<PlatformTelemetry>()) {
        validate_config(config);
        require_safe_existing_components(
            config.content_root,
            true,
            NativePortPlatformFailure::ContentBoundary,
            "content-root");
        content_root_handle_ = open_directory(
            config.content_root,
            FILE_SHARE_READ,
            NativePortPlatformFailure::ContentBoundary,
            "content-root-open");
        content_root_ = canonical_directory_from_handle(
            content_root_handle_,
            NativePortPlatformFailure::ContentBoundary,
            "content-root-final");

        // Reject overlapping roots before creating any writable directory.
        // Otherwise a rejected configuration could still mutate the declared
        // read-only content tree as a side effect of validation.
        const auto requested_user_data_root =
            absolute_normalized(config.user_data_root, "user-data-root");
        if (path_is_within_case_insensitive(
                content_root_, requested_user_data_root, true) ||
            path_is_within_case_insensitive(
                requested_user_data_root, content_root_, true))
            fail_platform(NativePortPlatformFailure::InvalidConfig,
                          ERROR_INVALID_PARAMETER,
                          "content-save-root-overlap");
        ensure_safe_directory_chain(
            requested_user_data_root,
            NativePortPlatformFailure::SaveBoundary,
            "user-data-root");
        user_data_root_handle_ = open_directory(
            requested_user_data_root,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NativePortPlatformFailure::SaveBoundary,
            "user-data-root-open");
        user_data_root_ = canonical_directory_from_handle(
            user_data_root_handle_,
            NativePortPlatformFailure::SaveBoundary,
            "user-data-root-final");
        if (path_is_within_case_insensitive(
                content_root_, user_data_root_, true) ||
            path_is_within_case_insensitive(
                user_data_root_, content_root_, true))
            fail_platform(NativePortPlatformFailure::InvalidConfig,
                          ERROR_INVALID_PARAMETER,
                          "content-save-root-overlap");

        save_root_ = user_data_root_ / project_id_ / "saves";
        ensure_safe_directory_chain(
            save_root_,
            NativePortPlatformFailure::SaveBoundary,
            "save-root-create");
        save_root_handle_ = open_directory(
            save_root_,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NativePortPlatformFailure::SaveBoundary,
            "save-root-open");
        save_root_ = canonical_directory_from_handle(
            save_root_handle_,
            NativePortPlatformFailure::SaveBoundary,
            "save-root-final");
        if (!path_is_within_case_insensitive(save_root_, user_data_root_))
            fail_platform(NativePortPlatformFailure::SaveBoundary,
                          ERROR_ACCESS_DENIED,
                          "save-root-boundary");
        open_save_lock();

        xinput_ = load_xinput();
        if (config.require_gamepad_backend && xinput_.get_state == nullptr)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_MOD_NOT_FOUND,
                          "xinput-load");
        telemetry_->snapshot.native_file_backend = true;
        telemetry_->snapshot.native_gamepad_backend =
            xinput_.get_state != nullptr;
        telemetry_->snapshot.native_save_backend = true;
    }

    ~Impl() noexcept {
        if (std::this_thread::get_id() != owner_thread_) std::terminate();
        if (xinput_.set_state != nullptr) {
            XINPUT_VIBRATION stopped{};
            for (std::size_t index = 0u;
                 index < vibration_active_.size();
                 ++index) {
                if (vibration_active_[index])
                    static_cast<void>(xinput_.set_state(
                        static_cast<DWORD>(index), &stopped));
            }
        }
    }

    [[nodiscard]] const std::filesystem::path& content_root() const {
        require_owner_thread();
        return content_root_;
    }
    [[nodiscard]] const std::filesystem::path& user_data_root() const {
        require_owner_thread();
        return user_data_root_;
    }

    [[nodiscard]] std::unique_ptr<NativePortReadOnlyFile> open_content_file(
        const NativePortContentFileBinding& binding) {
        require_owner_thread();
        const auto logical_id = copy_validated_identifier(
            binding.logical_id,
            maximum_platform_identifier_bytes,
            "content-logical-id");
        if (!valid_content_relative_path(binding.content_relative_path) ||
            !valid_native_port_sha256_identity(binding.byte_identity) ||
            binding.byte_size == 0u ||
            binding.byte_size > maximum_content_file_bytes_ ||
            binding.source_offset > static_cast<std::uint64_t>(
                                        std::numeric_limits<LONGLONG>::max()) ||
            binding.byte_size >
                static_cast<std::uint64_t>(
                    std::numeric_limits<LONGLONG>::max()) -
                    binding.source_offset)
            fail_platform(NativePortPlatformFailure::InvalidConfig,
                          ERROR_INVALID_PARAMETER,
                          "content-binding");
        const auto unresolved =
            (content_root_ / binding.content_relative_path).lexically_normal();
        require_safe_existing_components(
            unresolved,
            false,
            NativePortPlatformFailure::ContentBoundary,
            "content-path");
        const auto handle = CreateFileW(
            extended_path(unresolved).c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_RANDOM_ACCESS,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            fail_platform(NativePortPlatformFailure::ContentRead,
                          GetLastError(),
                          "content-open");
        UniqueHandle locked(handle);
        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(locked.get(), &information) == FALSE ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            fail_platform(NativePortPlatformFailure::ContentBoundary,
                          GetLastError(),
                          "content-handle");
        const auto final_path = final_path_from_handle(
            locked.get(),
            NativePortPlatformFailure::ContentBoundary,
            "content-final-path");
        if (!path_is_within_case_insensitive(final_path, content_root_))
            fail_platform(NativePortPlatformFailure::ContentBoundary,
                          ERROR_ACCESS_DENIED,
                          "content-root-boundary");
        LARGE_INTEGER size{};
        if (GetFileSizeEx(locked.get(), &size) == FALSE || size.QuadPart < 0)
            fail_platform(NativePortPlatformFailure::ContentRead,
                          GetLastError(),
                          "content-size");
        const auto file_bytes = static_cast<std::uint64_t>(size.QuadPart);
        if (binding.source_offset > file_bytes ||
            binding.byte_size > file_bytes - binding.source_offset)
            fail_platform(NativePortPlatformFailure::ContentBoundary,
                          ERROR_HANDLE_EOF,
                          "content-source-range");

        constexpr std::size_t hash_chunk_bytes = 1u * 1024u * 1024u;
        std::vector<std::byte> buffer(
            static_cast<std::size_t>(
                std::min<std::uint64_t>(binding.byte_size,
                                        hash_chunk_bytes)));
        Sha256 hash;
        std::uint64_t verified = 0u;
        while (verified < binding.byte_size) {
            const auto current = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(),
                                        binding.byte_size - verified));
            read_handle_exact(locked.get(),
                              binding.source_offset + verified,
                              std::span(buffer).first(current),
                              NativePortPlatformFailure::ContentRead,
                              "content-verify-read");
            hash.update(buffer.data(), current);
            verified += current;
        }
        if (digest_identity(hash.finish()) != binding.byte_identity)
            fail_platform(NativePortPlatformFailure::ContentIdentity,
                          ERROR_INVALID_DATA,
                          "content-identity");
        saturating_increment(
            telemetry_->snapshot.content_open_operations);
        saturating_add(telemetry_->snapshot.content_bytes_verified,
                       binding.byte_size);
        return std::unique_ptr<NativePortReadOnlyFile>(
            new NativePortReadOnlyFile(std::make_unique<
                NativePortReadOnlyFile::Impl>(std::move(locked),
                                              logical_id,
                                              binding.source_offset,
                                              binding.byte_size,
                                              telemetry_)));
    }

    [[nodiscard]] NativePortInputSnapshot poll_gamepads() {
        require_owner_thread();
        NativePortInputSnapshot result = input_snapshot_;
        saturating_increment(result.poll_sequence);
        if (xinput_.get_state == nullptr) {
            input_snapshot_ = result;
            saturating_increment(telemetry_->snapshot.input_polls);
            return input_snapshot_;
        }
        for (std::size_t index = 0u; index < result.gamepads.size(); ++index) {
            XINPUT_STATE state{};
            const auto status = xinput_.get_state(
                static_cast<DWORD>(index), &state);
            const bool was_connected = result.gamepads[index].connected;
            if (status == ERROR_DEVICE_NOT_CONNECTED) {
                result.gamepads[index] = {};
            } else if (status != ERROR_SUCCESS) {
                telemetry_->snapshot.last_platform_error_code = status;
                fail_platform(NativePortPlatformFailure::InputBackend,
                              status,
                              "xinput-poll");
            } else {
                const auto& source = state.Gamepad;
                auto& destination = result.gamepads[index];
                destination.connected = true;
                destination.packet_number = state.dwPacketNumber;
                destination.buttons = native_buttons(source.wButtons);
                destination.left_stick_x_raw = source.sThumbLX;
                destination.left_stick_y_raw = source.sThumbLY;
                destination.right_stick_x_raw = source.sThumbRX;
                destination.right_stick_y_raw = source.sThumbRY;
                destination.left_trigger_raw = source.bLeftTrigger;
                destination.right_trigger_raw = source.bRightTrigger;
                destination.left_stick_x = normalized_axis(source.sThumbLX);
                destination.left_stick_y = normalized_axis(source.sThumbLY);
                destination.right_stick_x = normalized_axis(source.sThumbRX);
                destination.right_stick_y = normalized_axis(source.sThumbRY);
                destination.left_trigger =
                    static_cast<float>(source.bLeftTrigger) / 255.0f;
                destination.right_trigger =
                    static_cast<float>(source.bRightTrigger) / 255.0f;
            }
            if (was_connected != result.gamepads[index].connected)
                saturating_increment(result.connection_generation);
        }
        input_snapshot_ = result;
        saturating_increment(telemetry_->snapshot.input_polls);
        telemetry_->snapshot.input_connection_generation =
            input_snapshot_.connection_generation;
        return input_snapshot_;
    }

    [[nodiscard]] bool set_gamepad_vibration(
        const std::uint32_t controller_index,
        const NativePortGamepadVibration& vibration) {
        require_owner_thread();
        if (controller_index >= native_port_gamepad_count)
            fail_platform(NativePortPlatformFailure::InvalidController,
                          ERROR_INVALID_PARAMETER,
                          "vibration-controller");
        if (!std::isfinite(vibration.low_frequency) ||
            !std::isfinite(vibration.high_frequency) ||
            vibration.low_frequency < 0.0f ||
            vibration.low_frequency > 1.0f ||
            vibration.high_frequency < 0.0f ||
            vibration.high_frequency > 1.0f)
            fail_platform(NativePortPlatformFailure::InvalidVibration,
                          ERROR_INVALID_PARAMETER,
                          "vibration-range");
        if (xinput_.set_state == nullptr) return false;
        XINPUT_VIBRATION state{};
        state.wLeftMotorSpeed = static_cast<WORD>(std::lround(
            vibration.low_frequency * 65'535.0f));
        state.wRightMotorSpeed = static_cast<WORD>(std::lround(
            vibration.high_frequency * 65'535.0f));
        const auto status = xinput_.set_state(controller_index, &state);
        if (status == ERROR_DEVICE_NOT_CONNECTED) {
            vibration_active_[controller_index] = false;
            return false;
        }
        if (status != ERROR_SUCCESS) {
            telemetry_->snapshot.last_platform_error_code = status;
            fail_platform(NativePortPlatformFailure::InputBackend,
                          status,
                          "xinput-vibration");
        }
        vibration_active_[controller_index] =
            state.wLeftMotorSpeed != 0u || state.wRightMotorSpeed != 0u;
        return true;
    }

    [[nodiscard]] NativePortSaveLoadResult load_save(
        const NativePortSaveKey& key) {
        require_owner_thread();
        const auto result = load_save_internal(key);
        saturating_increment(telemetry_->snapshot.save_load_operations);
        saturating_add(telemetry_->snapshot.save_bytes_read,
                       result.bytes_read);
        return result.public_result;
    }

    [[nodiscard]] std::uint64_t store_save(
        const NativePortSaveKey& key,
        const std::span<const std::byte> payload) {
        require_owner_thread();
        validate_save_key(key);
        if (payload.size() > maximum_save_payload_bytes_)
            fail_platform(NativePortPlatformFailure::ResourceLimit,
                          ERROR_FILE_TOO_LARGE,
                          "save-payload-budget");
        const auto existing = load_save_internal(key);
        if (existing.public_result.status ==
                NativePortSaveLoadStatus::IncompatibleSchema)
            fail_platform(NativePortPlatformFailure::SaveIncompatible,
                          ERROR_REVISION_MISMATCH,
                          "save-store-schema");
        if (existing.public_result.status ==
                NativePortSaveLoadStatus::Corrupt)
            fail_platform(NativePortPlatformFailure::SaveCorrupt,
                          ERROR_CRC,
                          "save-store-corrupt");
        if (existing.public_result.generation ==
            std::numeric_limits<std::uint64_t>::max())
            fail_platform(NativePortPlatformFailure::SaveConflict,
                          ERROR_ARITHMETIC_OVERFLOW,
                          "save-generation");
        const auto generation = existing.public_result.generation + 1u;
        const auto bytes =
            encode_save(project_id_, key, generation, payload);
        const auto primary =
            save_file_path(save_root_, key.slot_id, L".ksave");
        const auto backup =
            save_file_path(save_root_, key.slot_id, L".ksave.bak");
        const auto temporary =
            save_file_path(save_root_, key.slot_id, L".ksave.tmp");
        require_safe_optional_regular_file(
            primary, NativePortPlatformFailure::SaveBoundary, "save-primary");
        require_safe_optional_regular_file(
            backup, NativePortPlatformFailure::SaveBoundary, "save-backup");
        require_safe_optional_regular_file(
            temporary, NativePortPlatformFailure::SaveBoundary, "save-temporary");
        if (!path_missing(temporary) &&
            DeleteFileW(extended_path(temporary).c_str()) == FALSE)
            fail_platform(NativePortPlatformFailure::SaveWrite,
                          GetLastError(),
                          "save-stale-temporary");

        const auto temporary_handle = CreateFileW(
            extended_path(temporary).c_str(),
            GENERIC_WRITE | GENERIC_READ,
            0u,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (temporary_handle == INVALID_HANDLE_VALUE)
            fail_platform(NativePortPlatformFailure::SaveWrite,
                          GetLastError(),
                          "save-temporary-create");
        {
            UniqueHandle locked(temporary_handle);
            write_handle_exact(locked.get(),
                               bytes,
                               NativePortPlatformFailure::SaveWrite,
                               "save-temporary-write");
            if (FlushFileBuffers(locked.get()) == FALSE)
                fail_platform(NativePortPlatformFailure::SaveWrite,
                              GetLastError(),
                              "save-temporary-flush");
        }
        try {
            if (!path_missing(primary)) {
                const auto backup_extended = extended_path(backup);
                const wchar_t* const backup_path =
                    existing.public_result.status ==
                            NativePortSaveLoadStatus::RecoveredFromBackup
                        ? nullptr
                        : backup_extended.c_str();
                if (ReplaceFileW(extended_path(primary).c_str(),
                                 extended_path(temporary).c_str(),
                                 backup_path,
                                 REPLACEFILE_WRITE_THROUGH,
                                 nullptr,
                                 nullptr) == FALSE)
                    fail_platform(NativePortPlatformFailure::SaveWrite,
                                  GetLastError(),
                                  "save-atomic-replace");
            } else if (MoveFileExW(extended_path(temporary).c_str(),
                                   extended_path(primary).c_str(),
                                   MOVEFILE_WRITE_THROUGH) == FALSE) {
                fail_platform(NativePortPlatformFailure::SaveWrite,
                              GetLastError(),
                              "save-atomic-create");
            }
        } catch (...) {
            require_safe_optional_regular_file(
                temporary,
                NativePortPlatformFailure::SaveBoundary,
                "save-failed-temporary");
            if (!path_missing(temporary))
                static_cast<void>(
                    DeleteFileW(extended_path(temporary).c_str()));
            throw;
        }

        const auto verified = probe_save_file(primary,
                                              save_root_,
                                              project_id_,
                                              key,
                                              maximum_save_payload_bytes_);
        if (verified.status != NativePortSaveLoadStatus::Loaded ||
            verified.generation != generation ||
            verified.payload.size() != payload.size() ||
            !std::equal(verified.payload.begin(),
                        verified.payload.end(),
                        payload.begin()))
            fail_platform(NativePortPlatformFailure::SaveWrite,
                          ERROR_WRITE_FAULT,
                          "save-post-commit-verify");
        saturating_increment(telemetry_->snapshot.save_store_operations);
        saturating_add(telemetry_->snapshot.save_bytes_read,
                       existing.bytes_read + verified.bytes_read);
        saturating_add(telemetry_->snapshot.save_bytes_written, bytes.size());
        return generation;
    }

    [[nodiscard]] NativePortPlatformSnapshot snapshot() const {
        require_owner_thread();
        return telemetry_->snapshot;
    }

  private:
    struct InternalSaveLoad final {
        NativePortSaveLoadResult public_result;
        std::uint64_t bytes_read = 0u;
    };

    static void validate_config(const NativePortPlatformConfig& config) {
        if (config.contract_version != native_port_platform_contract_version ||
            config.content_root.empty() || config.user_data_root.empty() ||
            !valid_path_identifier(config.project_id,
                                   maximum_platform_identifier_bytes) ||
            config.maximum_content_file_bytes == 0u ||
            config.maximum_content_file_bytes > maximum_content_budget_bytes ||
            config.maximum_save_payload_bytes == 0u ||
            config.maximum_save_payload_bytes > maximum_save_budget_bytes)
            fail_platform(NativePortPlatformFailure::InvalidConfig,
                          ERROR_INVALID_PARAMETER,
                          "platform-config");
    }

    static void validate_save_key(const NativePortSaveKey& key) {
        if (!valid_path_identifier(key.slot_id, maximum_save_slot_bytes) ||
            key.schema_version == 0u)
            fail_platform(NativePortPlatformFailure::InvalidConfig,
                          ERROR_INVALID_PARAMETER,
                          "save-key");
    }

    void open_save_lock() {
        const auto path = save_root_ / ".katana-save.lock";
        require_safe_optional_regular_file(
            path, NativePortPlatformFailure::SaveBoundary, "save-lock-path");
        const auto handle = CreateFileW(
            extended_path(path).c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0u,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            fail_platform(NativePortPlatformFailure::SaveConflict,
                          GetLastError(),
                          "save-lock-open");
        save_lock_.reset(handle);
        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(save_lock_.get(), &information) ==
                FALSE ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            fail_platform(NativePortPlatformFailure::SaveBoundary,
                          GetLastError(),
                          "save-lock-handle");
        const auto final_path = final_path_from_handle(
            save_lock_.get(),
            NativePortPlatformFailure::SaveBoundary,
            "save-lock-final");
        if (!path_is_within_case_insensitive(final_path, save_root_))
            fail_platform(NativePortPlatformFailure::SaveBoundary,
                          ERROR_ACCESS_DENIED,
                          "save-lock-boundary");
    }

    [[nodiscard]] InternalSaveLoad load_save_internal(
        const NativePortSaveKey& key) const {
        validate_save_key(key);
        const auto primary_path =
            save_file_path(save_root_, key.slot_id, L".ksave");
        const auto backup_path =
            save_file_path(save_root_, key.slot_id, L".ksave.bak");
        auto primary = probe_save_file(primary_path,
                                       save_root_,
                                       project_id_,
                                       key,
                                       maximum_save_payload_bytes_);
        InternalSaveLoad result;
        result.bytes_read = primary.bytes_read;
        if (primary.status == NativePortSaveLoadStatus::Loaded) {
            result.public_result = {
                NativePortSaveLoadStatus::Loaded,
                primary.stored_schema_version,
                primary.generation,
                std::move(primary.payload)};
            return result;
        }
        // A valid primary record from a different schema is authoritative.
        // Falling back to an older, schema-compatible backup would silently
        // roll title state backwards across a product update.
        if (primary.status ==
            NativePortSaveLoadStatus::IncompatibleSchema) {
            result.public_result.status =
                NativePortSaveLoadStatus::IncompatibleSchema;
            result.public_result.stored_schema_version =
                primary.stored_schema_version;
            result.public_result.generation = primary.generation;
            return result;
        }
        auto backup = probe_save_file(backup_path,
                                      save_root_,
                                      project_id_,
                                      key,
                                      maximum_save_payload_bytes_);
        saturating_add(result.bytes_read, backup.bytes_read);
        if (backup.status == NativePortSaveLoadStatus::Loaded) {
            result.public_result = {
                NativePortSaveLoadStatus::RecoveredFromBackup,
                backup.stored_schema_version,
                backup.generation,
                std::move(backup.payload)};
            return result;
        }
        if (backup.status ==
            NativePortSaveLoadStatus::IncompatibleSchema) {
            result.public_result.status =
                NativePortSaveLoadStatus::IncompatibleSchema;
            result.public_result.stored_schema_version =
                backup.stored_schema_version;
            result.public_result.generation = backup.generation;
            return result;
        }
        if (primary.status == NativePortSaveLoadStatus::Corrupt ||
            backup.status == NativePortSaveLoadStatus::Corrupt) {
            result.public_result.status = NativePortSaveLoadStatus::Corrupt;
            return result;
        }
        return result;
    }

    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_)
            fail_platform(NativePortPlatformFailure::ThreadViolation,
                          ERROR_INVALID_THREAD_ID,
                          "platform-thread");
    }

    std::string project_id_;
    std::uint64_t maximum_content_file_bytes_ = 0u;
    std::uint32_t maximum_save_payload_bytes_ = 0u;
    std::thread::id owner_thread_;
    std::shared_ptr<PlatformTelemetry> telemetry_;
    std::filesystem::path content_root_;
    std::filesystem::path user_data_root_;
    std::filesystem::path save_root_;
    UniqueHandle content_root_handle_;
    UniqueHandle user_data_root_handle_;
    UniqueHandle save_root_handle_;
    UniqueHandle save_lock_;
    XInputApi xinput_;
    NativePortInputSnapshot input_snapshot_;
    std::array<bool, native_port_gamepad_count> vibration_active_{};
};

#else

class NativePortReadOnlyFile::Impl final {
  public:
    [[nodiscard]] std::string_view logical_id() const noexcept { return {}; }
    [[nodiscard]] std::uint64_t byte_size() const noexcept { return 0u; }
    void read_at(std::uint64_t, std::span<std::byte>) {
        throw NativePortPlatformError(
            NativePortPlatformFailure::UnsupportedHost, 1u, "unsupported-host");
    }
    [[nodiscard]] NativePortContentFileSnapshot snapshot() const { return {}; }
};

class NativePortPlatformServices::Impl final {
  public:
    explicit Impl(const NativePortPlatformConfig&) {
        throw NativePortPlatformError(
            NativePortPlatformFailure::UnsupportedHost, 1u, "unsupported-host");
    }
    [[nodiscard]] const std::filesystem::path& content_root() const {
        return empty_;
    }
    [[nodiscard]] const std::filesystem::path& user_data_root() const {
        return empty_;
    }
    [[nodiscard]] std::unique_ptr<NativePortReadOnlyFile> open_content_file(
        const NativePortContentFileBinding&) {
        throw NativePortPlatformError(
            NativePortPlatformFailure::UnsupportedHost, 1u, "unsupported-host");
    }
    [[nodiscard]] NativePortInputSnapshot poll_gamepads() { return {}; }
    [[nodiscard]] bool set_gamepad_vibration(
        std::uint32_t, const NativePortGamepadVibration&) { return false; }
    [[nodiscard]] NativePortSaveLoadResult load_save(
        const NativePortSaveKey&) { return {}; }
    [[nodiscard]] std::uint64_t store_save(
        const NativePortSaveKey&, std::span<const std::byte>) { return 0u; }
    [[nodiscard]] NativePortPlatformSnapshot snapshot() const { return {}; }

  private:
    std::filesystem::path empty_;
};

#endif

NativePortReadOnlyFile::NativePortReadOnlyFile(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

NativePortReadOnlyFile::~NativePortReadOnlyFile() = default;

std::string_view NativePortReadOnlyFile::logical_id() const noexcept {
    return impl_->logical_id();
}

std::uint64_t NativePortReadOnlyFile::byte_size() const noexcept {
    return impl_->byte_size();
}

void NativePortReadOnlyFile::read_at(
    const std::uint64_t offset,
    const std::span<std::byte> destination) {
    impl_->read_at(offset, destination);
}

NativePortContentFileSnapshot NativePortReadOnlyFile::snapshot() const {
    return impl_->snapshot();
}

NativePortPlatformServices::NativePortPlatformServices(
    const NativePortPlatformConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

NativePortPlatformServices::~NativePortPlatformServices() = default;

const std::filesystem::path& NativePortPlatformServices::content_root() const {
    return impl_->content_root();
}

const std::filesystem::path& NativePortPlatformServices::user_data_root() const {
    return impl_->user_data_root();
}

std::unique_ptr<NativePortReadOnlyFile>
NativePortPlatformServices::open_content_file(
    const NativePortContentFileBinding& binding) {
    return impl_->open_content_file(binding);
}

NativePortInputSnapshot NativePortPlatformServices::poll_gamepads() {
    return impl_->poll_gamepads();
}

bool NativePortPlatformServices::set_gamepad_vibration(
    const std::uint32_t controller_index,
    const NativePortGamepadVibration& vibration) {
    return impl_->set_gamepad_vibration(controller_index, vibration);
}

NativePortSaveLoadResult NativePortPlatformServices::load_save(
    const NativePortSaveKey& key) {
    return impl_->load_save(key);
}

std::uint64_t NativePortPlatformServices::store_save(
    const NativePortSaveKey& key,
    const std::span<const std::byte> payload) {
    return impl_->store_save(key, payload);
}

NativePortPlatformSnapshot NativePortPlatformServices::snapshot() const {
    return impl_->snapshot();
}

NativePortPlatformError::NativePortPlatformError(
    const NativePortPlatformFailure failure,
    const std::uint32_t platform_error_code,
    const std::string_view operation)
    : std::runtime_error(
          "native-port-platform-" + std::string(operation) + ":" +
          std::to_string(platform_error_code)),
      failure_(failure), platform_error_code_(platform_error_code) {}

NativePortPlatformFailure NativePortPlatformError::failure() const noexcept {
    return failure_;
}

std::uint32_t NativePortPlatformError::platform_error_code() const noexcept {
    return platform_error_code_;
}

} // namespace katana::runtime
