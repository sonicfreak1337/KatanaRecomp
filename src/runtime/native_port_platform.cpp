#include "katana/runtime/native_port_platform.hpp"

#include "native_port_input_policy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>

#include <bcrypt.h>
#include <dinput.h>
#include <mmsystem.h>
#include <xinput.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::size_t maximum_platform_identifier_bytes = 128u;
constexpr std::size_t maximum_save_slot_bytes =
    native_port_save_slot_id_maximum_bytes;
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

[[nodiscard]] float normalized_axis(const std::int16_t value) noexcept {
    return value < 0
               ? std::max(-1.0f, static_cast<float>(value) / 32'768.0f)
               : std::min(1.0f, static_cast<float>(value) / 32'767.0f);
}

[[nodiscard]] std::int16_t inverted_axis(
    const std::int16_t value) noexcept {
    const auto inverted = -static_cast<std::int32_t>(value);
    return static_cast<std::int16_t>(std::clamp(
        inverted,
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max())));
}

[[nodiscard]] std::int16_t joystick_axis(
    const DWORD value,
    const UINT minimum,
    const UINT maximum) noexcept {
    if (maximum <= minimum) return 0;
    const auto clamped =
        std::clamp<std::uint64_t>(value, minimum, maximum) - minimum;
    const auto range = static_cast<std::uint64_t>(maximum) - minimum;
    const auto scaled = (clamped * 65'535u) / range;
    return static_cast<std::int16_t>(
        static_cast<std::int32_t>(scaled) - 32'768);
}

[[nodiscard]] std::uint32_t xinput_buttons(const WORD value) noexcept {
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

enum class NativeGamepadSourceKind : std::uint8_t {
    XInput,
    DualSense,
    DualShock,
    Keyboard,
};

using detail::input_device_domain_mask;
using detail::joystick_device_domain;
using detail::keyboard_device_domain;
using detail::xinput_device_domain;
constexpr std::uint64_t input_device_slot_mask = 0x00000000FFFFFFFFull;

struct NativeGamepadCandidate final {
    std::uint64_t device_id = 0u;
    NativeGamepadSourceKind kind = NativeGamepadSourceKind::XInput;
    NativePortGamepadState state;
};

struct NativeJoystickIdentity final {
    UINT joystick_id = 0u;
    std::uint64_t device_identity = 0u;
    NativeGamepadSourceKind kind = NativeGamepadSourceKind::DualSense;
};

struct CrossBackendCorrelationEvidence final {
    std::uint64_t sony_device_id = 0u;
    std::uint64_t xinput_device_id = 0u;
    unsigned consecutive_samples = 0u;
    bool observed = false;
};

[[nodiscard]] std::optional<NativeGamepadSourceKind> sony_gamepad_kind(
    const WORD vendor_id,
    const WORD product_id) noexcept {
    constexpr WORD sony_vendor_id = 0x054Cu;
    if (vendor_id != sony_vendor_id) return std::nullopt;
    switch (product_id) {
    case 0x0CE6u:
    case 0x0DF2u:
        return NativeGamepadSourceKind::DualSense;
    case 0x05C4u:
    case 0x09CCu:
    case 0x0BA0u:
        return NativeGamepadSourceKind::DualShock;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::uint64_t direct_input_device_identity(
    const GUID& instance) noexcept {
    constexpr std::uint64_t fnv_offset = 14'695'981'039'346'656'037ull;
    constexpr std::uint64_t fnv_prime = 1'099'511'628'211ull;
    std::uint64_t hash = fnv_offset;
    const auto* const bytes = reinterpret_cast<const unsigned char*>(&instance);
    for (std::size_t index = 0u; index < sizeof(instance); ++index) {
        hash ^= bytes[index];
        hash *= fnv_prime;
    }
    hash &= ~input_device_domain_mask;
    return hash == 0u ? 1u : hash;
}

struct DirectInputEnumerationContext final {
    IDirectInput8W* input = nullptr;
    std::vector<NativeJoystickIdentity>* identities = nullptr;
};

BOOL CALLBACK enumerate_direct_input_gamepad(
    const DIDEVICEINSTANCEW* const instance,
    void* const opaque_context) {
    auto& context =
        *static_cast<DirectInputEnumerationContext*>(opaque_context);
    IDirectInputDevice8W* device = nullptr;
    if (FAILED(context.input->CreateDevice(
            instance->guidInstance, &device, nullptr)))
        return DIENUM_CONTINUE;

    DIPROPDWORD vid_pid{};
    vid_pid.diph.dwSize = sizeof(vid_pid);
    vid_pid.diph.dwHeaderSize = sizeof(vid_pid.diph);
    vid_pid.diph.dwHow = DIPH_DEVICE;
    DIPROPDWORD joystick_id{};
    joystick_id.diph.dwSize = sizeof(joystick_id);
    joystick_id.diph.dwHeaderSize = sizeof(joystick_id.diph);
    joystick_id.diph.dwHow = DIPH_DEVICE;
    const auto identity_status =
        device->GetProperty(DIPROP_VIDPID, &vid_pid.diph);
    const auto joystick_status =
        device->GetProperty(DIPROP_JOYSTICKID, &joystick_id.diph);
    device->Release();
    if (FAILED(identity_status) || FAILED(joystick_status))
        return DIENUM_CONTINUE;

    const auto kind = sony_gamepad_kind(
        LOWORD(vid_pid.dwData), HIWORD(vid_pid.dwData));
    if (!kind.has_value()) return DIENUM_CONTINUE;

    context.identities->push_back(
        {static_cast<UINT>(joystick_id.dwData),
         direct_input_device_identity(instance->guidInstance),
         *kind});
    return DIENUM_CONTINUE;
}

[[nodiscard]] std::optional<std::vector<NativeJoystickIdentity>>
enumerate_native_joystick_identities() {
    IDirectInput8W* input = nullptr;
    const auto create_status = DirectInput8Create(
        GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION,
        IID_IDirectInput8W,
        reinterpret_cast<void**>(&input),
        nullptr);
    if (FAILED(create_status) || input == nullptr) return std::nullopt;

    std::vector<NativeJoystickIdentity> identities;
    DirectInputEnumerationContext context{input, &identities};
    const auto enumeration_status = input->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        enumerate_direct_input_gamepad,
        &context,
        DIEDFL_ATTACHEDONLY);
    input->Release();
    if (FAILED(enumeration_status)) return std::nullopt;

    std::ranges::sort(identities, {}, &NativeJoystickIdentity::joystick_id);
    const auto duplicate = std::ranges::adjacent_find(
        identities, {}, &NativeJoystickIdentity::joystick_id);
    if (duplicate != identities.end()) return std::nullopt;
    std::vector<std::uint64_t> device_identities;
    device_identities.reserve(identities.size());
    for (const auto& identity : identities)
        device_identities.push_back(identity.device_identity);
    std::ranges::sort(device_identities);
    if (std::ranges::adjacent_find(device_identities) !=
        device_identities.end())
        return std::nullopt;
    return identities;
}

[[nodiscard]] unsigned source_priority(
    const NativeGamepadSourceKind kind) noexcept {
    switch (kind) {
    case NativeGamepadSourceKind::DualSense:
        return 0u;
    case NativeGamepadSourceKind::DualShock:
        return 1u;
    case NativeGamepadSourceKind::XInput:
        return 2u;
    case NativeGamepadSourceKind::Keyboard:
        return 3u;
    }
    return 3u;
}

void add_button(std::uint32_t& buttons,
                const bool pressed,
                const NativePortGamepadButton destination) noexcept {
    if (pressed)
        buttons |= native_port_gamepad_button_mask(destination);
}

[[nodiscard]] bool foreground_process_is_current() noexcept {
    const auto foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    DWORD process_id = 0u;
    static_cast<void>(GetWindowThreadProcessId(foreground, &process_id));
    return process_id == GetCurrentProcessId();
}

[[nodiscard]] NativePortGamepadState keyboard_gamepad_state() noexcept {
    NativePortGamepadState result;
    result.connected = true;
    if (!foreground_process_is_current()) return result;
    const auto pressed = [](const int virtual_key) noexcept {
        return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
    };
    add_button(result.buttons,
               pressed(VK_UP),
               NativePortGamepadButton::DpadUp);
    add_button(result.buttons,
               pressed(VK_DOWN),
               NativePortGamepadButton::DpadDown);
    add_button(result.buttons,
               pressed(VK_LEFT),
               NativePortGamepadButton::DpadLeft);
    add_button(result.buttons,
               pressed(VK_RIGHT),
               NativePortGamepadButton::DpadRight);
    add_button(result.buttons,
               pressed(VK_RETURN),
               NativePortGamepadButton::Menu);
    add_button(result.buttons,
               pressed(VK_BACK),
               NativePortGamepadButton::View);
    // Keyboard compatibility mapping is deliberately distinct from the
    // physical XInput labels: Z=A, X=B, A=X and S=Y. XInput A/X already map
    // to NativePortGamepadButton::A/X in xinput_buttons() above.
    add_button(result.buttons, pressed('Z'), NativePortGamepadButton::A);
    add_button(result.buttons, pressed('X'), NativePortGamepadButton::B);
    add_button(result.buttons, pressed('A'), NativePortGamepadButton::X);
    add_button(result.buttons, pressed('S'), NativePortGamepadButton::Y);
    add_button(result.buttons,
               pressed('Q'),
               NativePortGamepadButton::LeftShoulder);
    add_button(result.buttons,
               pressed('W'),
               NativePortGamepadButton::RightShoulder);
    return result;
}

void merge_keyboard_gamepad(NativePortGamepadState& destination,
                            const NativePortGamepadState& keyboard) noexcept {
    destination.connected = destination.connected || keyboard.connected;
    destination.buttons |= keyboard.buttons;
}

void add_pov_buttons(std::uint32_t& buttons, const DWORD pov) noexcept {
    if (pov == JOY_POVCENTERED || pov > 35'999u) return;
    add_button(buttons,
               pov >= 31'500u || pov < 4'500u,
               NativePortGamepadButton::DpadUp);
    add_button(buttons,
               pov >= 4'500u && pov < 13'500u,
               NativePortGamepadButton::DpadRight);
    add_button(buttons,
               pov >= 13'500u && pov < 22'500u,
               NativePortGamepadButton::DpadDown);
    add_button(buttons,
               pov >= 22'500u && pov < 31'500u,
               NativePortGamepadButton::DpadLeft);
}

void add_joystick_buttons(NativePortGamepadState& state,
                          const DWORD value) noexcept {
    const auto pressed = [&](const unsigned index) {
        return (value & (DWORD{1u} << index)) != 0u;
    };
    // WinMM is admitted only for the explicitly identified Sony layouts
    // above. Unknown HID button ordinals are not a semantic gamepad mapping.
    add_button(state.buttons, pressed(0u), NativePortGamepadButton::X);
    add_button(state.buttons, pressed(1u), NativePortGamepadButton::A);
    add_button(state.buttons, pressed(2u), NativePortGamepadButton::B);
    add_button(state.buttons, pressed(3u), NativePortGamepadButton::Y);
    add_button(state.buttons, pressed(8u), NativePortGamepadButton::View);
    add_button(state.buttons, pressed(9u), NativePortGamepadButton::Menu);
    add_button(state.buttons,
               pressed(4u),
               NativePortGamepadButton::LeftShoulder);
    add_button(state.buttons,
               pressed(5u),
               NativePortGamepadButton::RightShoulder);
    add_button(state.buttons,
               pressed(10u),
               NativePortGamepadButton::LeftStick);
    add_button(state.buttons,
               pressed(11u),
               NativePortGamepadButton::RightStick);
}

[[nodiscard]] std::uint8_t joystick_half_trigger(
    const std::int16_t axis) noexcept {
    if (axis < 0) {
        const auto magnitude = static_cast<std::uint32_t>(
            -static_cast<std::int32_t>(axis));
        return static_cast<std::uint8_t>(
            (magnitude * 255u + 16'384u) / 32'768u);
    }
    const auto magnitude = static_cast<std::uint32_t>(axis);
    return static_cast<std::uint8_t>(
        (magnitude * 255u + 16'383u) / 32'767u);
}

[[nodiscard]] bool gamepad_has_identity_activity(
    const NativePortGamepadState& state) noexcept {
    constexpr std::int32_t stick_threshold = 2'048;
    constexpr std::uint8_t trigger_threshold = 8u;
    return state.buttons != 0u ||
           std::abs(static_cast<std::int32_t>(state.left_stick_x_raw)) >
               stick_threshold ||
           std::abs(static_cast<std::int32_t>(state.left_stick_y_raw)) >
               stick_threshold ||
           std::abs(static_cast<std::int32_t>(state.right_stick_x_raw)) >
               stick_threshold ||
           std::abs(static_cast<std::int32_t>(state.right_stick_y_raw)) >
               stick_threshold ||
           state.left_trigger_raw > trigger_threshold ||
           state.right_trigger_raw > trigger_threshold;
}

[[nodiscard]] bool correlated_gamepad_payload(
    const NativePortGamepadState& left,
    const NativePortGamepadState& right) noexcept {
    constexpr std::int32_t stick_tolerance = 2'048;
    constexpr std::int32_t trigger_tolerance = 8;
    const auto close = [](const auto lhs, const auto rhs, const auto tolerance) {
        return std::abs(static_cast<std::int32_t>(lhs) -
                        static_cast<std::int32_t>(rhs)) <= tolerance;
    };
    return left.buttons == right.buttons &&
           close(left.left_stick_x_raw, right.left_stick_x_raw,
                 stick_tolerance) &&
           close(left.left_stick_y_raw, right.left_stick_y_raw,
                 stick_tolerance) &&
           close(left.right_stick_x_raw, right.right_stick_x_raw,
                 stick_tolerance) &&
           close(left.right_stick_y_raw, right.right_stick_y_raw,
                 stick_tolerance) &&
           close(left.left_trigger_raw, right.left_trigger_raw,
                 trigger_tolerance) &&
           close(left.right_trigger_raw, right.right_trigger_raw,
                 trigger_tolerance);
}

[[nodiscard]] bool same_gamepad_payload(
    const NativePortGamepadState& left,
    const NativePortGamepadState& right) noexcept {
    return left.buttons == right.buttons &&
           left.left_stick_x_raw == right.left_stick_x_raw &&
           left.left_stick_y_raw == right.left_stick_y_raw &&
           left.right_stick_x_raw == right.right_stick_x_raw &&
           left.right_stick_y_raw == right.right_stick_y_raw &&
           left.left_trigger_raw == right.left_trigger_raw &&
           left.right_trigger_raw == right.right_trigger_raw;
}

using detail::NativeGamepadButtonStability;
using detail::stabilize_gamepad_buttons;

static_assert([] {
    NativeGamepadButtonStability stability;
    constexpr auto button = native_port_gamepad_button_mask(
        NativePortGamepadButton::A);
    return stabilize_gamepad_buttons(stability, button) == button &&
           stabilize_gamepad_buttons(stability, 0u) == button &&
           stabilize_gamepad_buttons(stability, button) == button &&
           stabilize_gamepad_buttons(stability, 0u) == button &&
           stabilize_gamepad_buttons(stability, 0u) == button &&
           stabilize_gamepad_buttons(stability, 0u) == 0u;
}());

[[nodiscard]] std::optional<DWORD> xinput_slot_from_device(
    const std::uint64_t device_id) noexcept {
    if ((device_id & input_device_domain_mask) != xinput_device_domain)
        return std::nullopt;
    const auto encoded = device_id & input_device_slot_mask;
    if (encoded == 0u || encoded > XUSER_MAX_COUNT) return std::nullopt;
    return static_cast<DWORD>(encoded - 1u);
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

// The trace is deliberately a private, fixed-format provider behind the
// platform boundary.  NativePortPlatformServices owns it for the complete
// run, so poll_gamepads() only copies an already materialised snapshot and
// never performs I/O, locking, or allocation in replay mode.  Recording
// reserves both its memory and mapped-journal capacity before the first poll.
class NativePortInputTrace final {
  public:
    static constexpr std::size_t header_bytes = 48u;
    static constexpr std::size_t frame_count_offset = 24u;
    static constexpr std::size_t encoded_gamepad_bytes = 52u;
    static constexpr std::size_t encoded_frame_bytes =
        16u + native_port_gamepad_count * encoded_gamepad_bytes;
    static constexpr std::size_t journal_flush_frame_interval = 256u;
    static constexpr std::uint32_t incomplete_capacity = 1u << 0u;
    static constexpr std::uint32_t incomplete_journal_failure = 1u << 1u;

    NativePortInputTrace(std::string identity,
                         const std::size_t maximum_frames,
                         const bool replay,
                         const bool stop_at_capacity = false,
                         const std::filesystem::path& record_path = {})
        : identity_(std::move(identity)),
          maximum_frames_(maximum_frames),
          replay_(replay),
          stop_at_capacity_(stop_at_capacity) {
        frames_.reserve(maximum_frames_);
        if (!replay_ && !record_path.empty()) open_live_journal(record_path);
    }

    ~NativePortInputTrace() { close_live_journal(); }

    NativePortInputTrace(const NativePortInputTrace&) = delete;
    NativePortInputTrace& operator=(const NativePortInputTrace&) = delete;

    [[nodiscard]] static std::unique_ptr<NativePortInputTrace> load(
        const std::filesystem::path& path,
        const std::string_view expected_identity,
        const std::size_t maximum_frames) {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_FILE_NOT_FOUND,
                          "input-replay-open");

        std::array<char, 8u> magic{};
        input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!input || magic != std::array<char, 8u>{'K', 'A', 'T', 'A',
                                                     'N', 'A', 'I', 'R'})
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_INVALID_DATA,
                          "input-replay-header");
        const auto version = read_u32(input, "input-replay-version");
        const auto contract = read_u32(input, "input-replay-contract");
        const auto slot_count = read_u32(input, "input-replay-slots");
        const auto identity_bytes = read_u32(input, "input-replay-identity-size");
        const auto frame_count = read_u64(input, "input-replay-frame-count");
        const auto storage_frame_count =
            read_u64(input, "input-replay-storage-frame-count");
        const auto frame_bytes = read_u32(input, "input-replay-frame-bytes");
        const auto flags = read_u32(input, "input-replay-flags");
        if (version != native_port_input_recording_version ||
            contract != native_port_input_recording_contract_version ||
            slot_count != native_port_gamepad_count ||
            identity_bytes == 0u ||
            identity_bytes > maximum_platform_identifier_bytes ||
            frame_count > storage_frame_count ||
            storage_frame_count > maximum_frames ||
            frame_bytes != encoded_frame_bytes || flags != 0u)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_REVISION_MISMATCH,
                          "input-replay-contract");
        std::string identity(identity_bytes, '\0');
        input.read(identity.data(), static_cast<std::streamsize>(identity.size()));
        if (!input || identity != expected_identity)
            fail_platform(NativePortPlatformFailure::ContentIdentity,
                          ERROR_INVALID_DATA,
                          "input-replay-identity");

        auto result = std::make_unique<NativePortInputTrace>(
            std::move(identity), maximum_frames, true, false);
        result->frames_.reserve(static_cast<std::size_t>(frame_count));
        std::uint64_t previous_sequence = 0u;
        for (std::uint64_t index = 0u; index < frame_count; ++index) {
            NativePortInputSnapshot snapshot;
            snapshot.poll_sequence = read_u64(input, "input-replay-sequence");
            snapshot.connection_generation =
                read_u64(input, "input-replay-generation");
            if (snapshot.poll_sequence == 0u ||
                (index != 0u && snapshot.poll_sequence <= previous_sequence))
                fail_platform(NativePortPlatformFailure::InputBackend,
                              ERROR_INVALID_DATA,
                              "input-replay-sequence-order");
            previous_sequence = snapshot.poll_sequence;
            for (auto& gamepad : snapshot.gamepads)
                read_gamepad(input, gamepad);
            result->frames_.push_back(snapshot);
        }
        input.seekg(0, std::ios::end);
        const auto actual_bytes = input.tellg();
        const auto expected_bytes =
            static_cast<std::uint64_t>(header_bytes) + identity_bytes +
            storage_frame_count * encoded_frame_bytes;
        if (!input || actual_bytes < 0 ||
            static_cast<std::uint64_t>(actual_bytes) != expected_bytes)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_INVALID_DATA,
                          "input-replay-storage-size");
        return result;
    }

    void append(const NativePortInputSnapshot& snapshot) {
        if (replay_ || recording_stopped_) return;
        if (observe_journal_flush_failure()) return;
        if (frames_.size() == maximum_frames_) {
            if (stop_at_capacity_) {
                mark_recording_incomplete(incomplete_capacity);
                return;
            }
            fail_platform(NativePortPlatformFailure::ResourceLimit,
                          ERROR_BUFFER_OVERFLOW,
                          "input-record-frame-budget");
        }
        frames_.push_back(snapshot);
        if (journal_view_ != nullptr) {
            const auto index = frames_.size() - 1u;
            const auto destination_offset =
                journal_data_offset_ + index * encoded_frame_bytes;
            encode_frame(std::span<std::byte>(
                             journal_view_ + destination_offset,
                             encoded_frame_bytes),
                         snapshot);
            if (frames_.size() % journal_flush_frame_interval == 0u) {
                const auto committed_bytes =
                    journal_data_offset_ +
                    frames_.size() * encoded_frame_bytes;
                request_journal_flush(
                    static_cast<std::uint64_t>(frames_.size()),
                    committed_bytes);
            }
        }
        // Explicit captures remain strict. Automatic crash diagnostics stop
        // themselves and mark the trace non-replayable instead of replacing
        // the title failure with a diagnostic-I/O failure.
        static_cast<void>(observe_journal_flush_failure());
    }

    [[nodiscard]] NativePortInputSnapshot next() {
        if (!replay_ || cursor_ >= frames_.size())
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_HANDLE_EOF,
                          "input-replay-exhausted");
        return frames_[cursor_++];
    }

    void save_atomic(const std::filesystem::path& path) {
        if (replay_) return;
        close_live_journal();
        static_cast<void>(observe_journal_flush_failure());
        const auto temporary = std::filesystem::path(path.native() +
                                                     std::filesystem::path(
                                                         L".tmp").native());
        try {
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
                fail_platform(NativePortPlatformFailure::InputBackend,
                              ERROR_ACCESS_DENIED,
                              "input-record-open");
            output.write("KATANAIR", 8);
            write_u32(output, native_port_input_recording_version);
            write_u32(output, native_port_input_recording_contract_version);
            write_u32(output, native_port_gamepad_count);
            write_u32(output, static_cast<std::uint32_t>(identity_.size()));
            write_u64(output, frames_.size());
            write_u64(output, frames_.size());
            write_u32(output, static_cast<std::uint32_t>(encoded_frame_bytes));
            write_u32(output, incomplete_flags_);
            output.write(identity_.data(),
                         static_cast<std::streamsize>(identity_.size()));
            for (const auto& snapshot : frames_) {
                write_u64(output, snapshot.poll_sequence);
                write_u64(output, snapshot.connection_generation);
                for (const auto& gamepad : snapshot.gamepads)
                    write_gamepad(output, gamepad);
            }
            output.flush();
            if (!output)
                fail_platform(NativePortPlatformFailure::InputBackend,
                              ERROR_WRITE_FAULT,
                              "input-record-write");
            output.close();
            // Never remove an existing trace before publication. Windows'
            // native replace operation preserves same-volume atomicity.
            if (MoveFileExW(
                    extended_path(temporary).c_str(),
                    extended_path(path).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return;
            std::error_code remove_error;
            std::filesystem::remove(temporary, remove_error);
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_WRITE_FAULT,
                          "input-record-atomic-rename");
        } catch (...) {
            std::error_code remove_error;
            std::filesystem::remove(temporary, remove_error);
            if (!stop_at_capacity_) throw;
            mark_recording_incomplete(incomplete_journal_failure);
        }
    }

  private:
    static void encode_frame(std::span<std::byte> output,
                             const NativePortInputSnapshot& snapshot) {
        if (output.size() != encoded_frame_bytes)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_INSUFFICIENT_BUFFER,
                          "input-record-frame-buffer");
        std::size_t offset = 0u;
        const auto append_u16 = [&](const std::uint16_t value) {
            write_little<std::uint16_t>(output, offset, value);
            offset += sizeof(value);
        };
        const auto append_u32 = [&](const std::uint32_t value) {
            write_little<std::uint32_t>(output, offset, value);
            offset += sizeof(value);
        };
        const auto append_u64 = [&](const std::uint64_t value) {
            write_little<std::uint64_t>(output, offset, value);
            offset += sizeof(value);
        };
        const auto append_float = [&](const float value) {
            append_u32(std::bit_cast<std::uint32_t>(value));
        };

        append_u64(snapshot.poll_sequence);
        append_u64(snapshot.connection_generation);
        for (const auto& gamepad : snapshot.gamepads) {
            append_u32(gamepad.connected ? 1u : 0u);
            append_u32(gamepad.packet_number);
            append_u32(gamepad.buttons);
            append_u16(
                static_cast<std::uint16_t>(gamepad.left_stick_x_raw));
            append_u16(
                static_cast<std::uint16_t>(gamepad.left_stick_y_raw));
            append_u16(
                static_cast<std::uint16_t>(gamepad.right_stick_x_raw));
            append_u16(
                static_cast<std::uint16_t>(gamepad.right_stick_y_raw));
            append_u32(gamepad.left_trigger_raw);
            append_u32(gamepad.right_trigger_raw);
            append_float(gamepad.left_stick_x);
            append_float(gamepad.left_stick_y);
            append_float(gamepad.right_stick_x);
            append_float(gamepad.right_stick_y);
            append_float(gamepad.left_trigger);
            append_float(gamepad.right_trigger);
        }
        if (offset != output.size())
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_INVALID_DATA,
                          "input-record-frame-size");
    }

    void open_live_journal(const std::filesystem::path& path) {
        const auto maximum_payload =
            maximum_frames_ * encoded_frame_bytes;
        if (maximum_frames_ != 0u &&
            maximum_payload / maximum_frames_ != encoded_frame_bytes)
            fail_platform(NativePortPlatformFailure::ResourceLimit,
                          ERROR_ARITHMETIC_OVERFLOW,
                          "input-record-storage-size");
        const auto total_bytes =
            static_cast<std::uint64_t>(header_bytes) + identity_.size() +
            maximum_payload;
        if (total_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<SIZE_T>::max()))
            fail_platform(NativePortPlatformFailure::ResourceLimit,
                          ERROR_ARITHMETIC_OVERFLOW,
                          "input-record-storage-size");

        require_safe_optional_regular_file(
            path, NativePortPlatformFailure::InputBackend,
            "input-record-path");
        UniqueHandle file(CreateFileW(
            extended_path(path).c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_RANDOM_ACCESS,
            nullptr));
        if (!file)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          GetLastError(),
                          "input-record-open");
        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(file.get(), &information) == FALSE ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0u)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          GetLastError(),
                          "input-record-handle");

        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(total_bytes);
        if (SetFilePointerEx(file.get(), end, nullptr, FILE_BEGIN) == FALSE ||
            SetEndOfFile(file.get()) == FALSE)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          GetLastError(),
                          "input-record-reserve");
        UniqueHandle mapping(CreateFileMappingW(
            file.get(),
            nullptr,
            PAGE_READWRITE,
            static_cast<DWORD>(total_bytes >> 32u),
            static_cast<DWORD>(total_bytes),
            nullptr));
        if (!mapping)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          GetLastError(),
                          "input-record-map");
        auto* const view = static_cast<std::byte*>(MapViewOfFile(
            mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0u, 0u, 0u));
        if (view == nullptr)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          GetLastError(),
                          "input-record-view");

        const auto unmap_on_failure = [&]() noexcept {
            static_cast<void>(UnmapViewOfFile(view));
        };
        try {
            std::span<std::byte> header(view, header_bytes + identity_.size());
            std::fill(header.begin(), header.end(), std::byte{0});
            std::memcpy(header.data(), "KATANAIR", 8u);
            write_little<std::uint32_t>(
                header, 8u, native_port_input_recording_version);
            write_little<std::uint32_t>(
                header, 12u, native_port_input_recording_contract_version);
            write_little<std::uint32_t>(
                header, 16u,
                static_cast<std::uint32_t>(native_port_gamepad_count));
            write_little<std::uint32_t>(
                header, 20u, static_cast<std::uint32_t>(identity_.size()));
            write_little<std::uint64_t>(header, frame_count_offset, 0u);
            write_little<std::uint64_t>(
                header, 32u, static_cast<std::uint64_t>(maximum_frames_));
            write_little<std::uint32_t>(
                header, 40u,
                static_cast<std::uint32_t>(encoded_frame_bytes));
            write_little<std::uint32_t>(header, 44u, 0u);
            std::memcpy(header.data() + header_bytes,
                        identity_.data(),
                        identity_.size());
            if (FlushViewOfFile(view, header.size()) == FALSE ||
                FlushFileBuffers(file.get()) == FALSE)
                fail_platform(NativePortPlatformFailure::InputBackend,
                              GetLastError(),
                              "input-record-header-flush");
        } catch (...) {
            unmap_on_failure();
            throw;
        }

        journal_file_ = std::move(file);
        journal_mapping_ = std::move(mapping);
        journal_view_ = view;
        journal_data_offset_ = header_bytes + identity_.size();
        start_journal_flusher();
    }

    void close_live_journal() noexcept {
        if (journal_view_ != nullptr) {
            stop_journal_flusher();
            if (stop_at_capacity_ &&
                journal_flush_error_code_.load(std::memory_order_acquire) !=
                    ERROR_SUCCESS)
                mark_recording_incomplete(incomplete_journal_failure);
            const auto committed_bytes =
                journal_data_offset_ + frames_.size() * encoded_frame_bytes;
            // The worker is joined before the final owner-thread commit, so
            // no flush can race the unmap/handle close.  Keep the old durable
            // frame count while flushing the complete payload, then publish
            // exactly the final count and flush the header.
            if (flush_journal_payload(committed_bytes))
                static_cast<void>(publish_journal_frame_count(
                    static_cast<std::uint64_t>(frames_.size()),
                    committed_bytes));
            static_cast<void>(UnmapViewOfFile(journal_view_));
            journal_view_ = nullptr;
        }
        journal_mapping_.reset();
        journal_file_.reset();
        journal_data_offset_ = 0u;
    }

    void start_journal_flusher() {
        journal_flush_stop_.store(false, std::memory_order_relaxed);
        {
            std::scoped_lock lock(journal_flush_wait_mutex_);
            journal_flush_requested_frames_ = 0u;
            journal_flush_requested_bytes_ = 0u;
        }
        journal_flush_error_code_.store(ERROR_SUCCESS,
                                        std::memory_order_relaxed);
        try {
            journal_flush_worker_ = std::thread([this]() noexcept {
                journal_flush_loop();
            });
        } catch (...) {
            // The mapped journal is still owned by this object, but no
            // worker exists to join.  Restore the pre-open state before the
            // constructor propagates the thread-creation failure.
            static_cast<void>(UnmapViewOfFile(journal_view_));
            journal_view_ = nullptr;
            journal_mapping_.reset();
            journal_file_.reset();
            journal_data_offset_ = 0u;
            throw;
        }
    }

    void stop_journal_flusher() noexcept {
        if (!journal_flush_worker_.joinable()) return;
        {
            std::scoped_lock lock(journal_flush_wait_mutex_);
            journal_flush_stop_.store(true, std::memory_order_release);
        }
        journal_flush_wakeup_.notify_one();
        journal_flush_worker_.join();
    }

    void request_journal_flush(const std::uint64_t frame_count,
                               const std::size_t committed_bytes) noexcept {
        {
            // A single monotonic (frame-count, byte-prefix) target is the
            // bounded queue: repeated requests collapse into the newest
            // committed range and never allocate one work item per frame.
            std::scoped_lock lock(journal_flush_wait_mutex_);
            if (frame_count > journal_flush_requested_frames_) {
                journal_flush_requested_frames_ = frame_count;
                journal_flush_requested_bytes_ =
                    static_cast<std::uint64_t>(committed_bytes);
            }
        }
        journal_flush_wakeup_.notify_one();
    }

    [[nodiscard]] bool flush_journal_payload(
        const std::uint64_t committed_bytes) noexcept {
        if (journal_view_ == nullptr || !journal_file_) {
            record_journal_flush_failure(ERROR_INVALID_HANDLE);
            return false;
        }
        if (committed_bytes < journal_data_offset_) {
            record_journal_flush_failure(ERROR_INVALID_DATA);
            return false;
        }
        const auto payload_bytes =
            committed_bytes - static_cast<std::uint64_t>(journal_data_offset_);
        if (payload_bytes == 0u) return true;
        if (committed_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<SIZE_T>::max())) {
            record_journal_flush_failure(ERROR_ARITHMETIC_OVERFLOW);
            return false;
        }
        if (FlushViewOfFile(
                journal_view_ + journal_data_offset_,
                static_cast<SIZE_T>(payload_bytes)) == FALSE) {
            record_journal_flush_failure(GetLastError());
            return false;
        }
        if (FlushFileBuffers(journal_file_.get()) == FALSE) {
            record_journal_flush_failure(GetLastError());
            return false;
        }
        return true;
    }

    [[nodiscard]] bool publish_journal_frame_count(
        const std::uint64_t frame_count,
        const std::uint64_t committed_bytes) noexcept {
        if (frame_count > maximum_frames_ ||
            frame_count >
                (std::numeric_limits<std::size_t>::max() -
                 journal_data_offset_) /
                    encoded_frame_bytes ||
            committed_bytes !=
                static_cast<std::uint64_t>(journal_data_offset_ +
                                           frame_count * encoded_frame_bytes)) {
            record_journal_flush_failure(ERROR_INVALID_DATA);
            return false;
        }
        // Payload durability is established before this release publication;
        // the header count is the journal's crash-consistency commit marker.
        std::atomic_thread_fence(std::memory_order_release);
        static_assert(frame_count_offset % alignof(LONG64) == 0u);
        InterlockedExchange64(
            reinterpret_cast<volatile LONG64*>(journal_view_ +
                                                frame_count_offset),
            static_cast<LONG64>(frame_count));
        if (FlushViewOfFile(
                journal_view_, static_cast<SIZE_T>(journal_data_offset_)) ==
            FALSE) {
            record_journal_flush_failure(GetLastError());
            return false;
        }
        if (!journal_file_ || FlushFileBuffers(journal_file_.get()) == FALSE) {
            record_journal_flush_failure(
                journal_file_ ? GetLastError() : ERROR_INVALID_HANDLE);
            return false;
        }
        return true;
    }

    void record_journal_flush_failure(const DWORD error) noexcept {
        auto code = error;
        if (code == ERROR_SUCCESS) code = ERROR_WRITE_FAULT;
        auto expected = static_cast<DWORD>(ERROR_SUCCESS);
        static_cast<void>(journal_flush_error_code_.compare_exchange_strong(
            expected, code, std::memory_order_release,
            std::memory_order_relaxed));
    }

    void mark_recording_incomplete(const std::uint32_t flag) noexcept {
        incomplete_flags_ |= flag;
        recording_stopped_ = true;
        if (journal_view_ == nullptr) return;
        static_assert(44u % alignof(LONG) == 0u);
        static_cast<void>(InterlockedOr(
            reinterpret_cast<volatile LONG*>(journal_view_ + 44u),
            static_cast<LONG>(flag)));
        // Best effort only: the automatic diagnostic must not replace the
        // product failure if the storage device is already failing.
        static_cast<void>(FlushViewOfFile(
            journal_view_, static_cast<SIZE_T>(journal_data_offset_)));
        if (journal_file_)
            static_cast<void>(FlushFileBuffers(journal_file_.get()));
    }

    [[nodiscard]] bool observe_journal_flush_failure() {
        const auto error = journal_flush_error_code_.load(
            std::memory_order_acquire);
        if (error == ERROR_SUCCESS) return false;
        if (stop_at_capacity_) {
            mark_recording_incomplete(incomplete_journal_failure);
            return true;
        }
        fail_platform(NativePortPlatformFailure::InputBackend,
                      error,
                      "input-record-journal-flush");
    }

    void journal_flush_loop() noexcept {
        try {
            std::uint64_t completed_frames = 0u;
            std::uint64_t completed_bytes = 0u;
            for (;;) {
                std::unique_lock lock(journal_flush_wait_mutex_);
                journal_flush_wakeup_.wait(lock, [this, completed_frames]() {
                    return journal_flush_stop_.load(
                               std::memory_order_acquire) ||
                           journal_flush_error_code_.load(
                               std::memory_order_acquire) != ERROR_SUCCESS ||
                           journal_flush_requested_frames_ > completed_frames;
                });
                const auto stopping = journal_flush_stop_.load(
                    std::memory_order_acquire);
                const auto requested_frames = journal_flush_requested_frames_;
                const auto requested_bytes = journal_flush_requested_bytes_;
                lock.unlock();

                if (journal_flush_error_code_.load(
                        std::memory_order_acquire) != ERROR_SUCCESS)
                    return;
                if (requested_frames > completed_frames) {
                    if (requested_bytes < completed_bytes ||
                        !flush_journal_payload(requested_bytes) ||
                        !publish_journal_frame_count(requested_frames,
                                                     requested_bytes))
                        return;
                    completed_frames = requested_frames;
                    completed_bytes = requested_bytes;
                }
                if (stopping) {
                    std::scoped_lock drain_lock(journal_flush_wait_mutex_);
                    if (completed_frames >= journal_flush_requested_frames_)
                        return;
                }
            }
        } catch (...) {
            // Keep all worker failures on the fixed, owner-observed channel;
            // an exception escaping a noexcept thread would terminate the
            // process before the native-port contract can report it.
            record_journal_flush_failure(ERROR_FUNCTION_FAILED);
        }
    }

    static std::uint32_t read_u32(std::ifstream& input, const char* operation) {
        std::array<unsigned char, 4u> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), 4);
        if (!input)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_HANDLE_EOF,
                          operation);
        return static_cast<std::uint32_t>(bytes[0]) |
               (static_cast<std::uint32_t>(bytes[1]) << 8u) |
               (static_cast<std::uint32_t>(bytes[2]) << 16u) |
               (static_cast<std::uint32_t>(bytes[3]) << 24u);
    }
    static std::uint64_t read_u64(std::ifstream& input, const char* operation) {
        const auto low = read_u32(input, operation);
        const auto high = read_u32(input, operation);
        return static_cast<std::uint64_t>(low) |
               (static_cast<std::uint64_t>(high) << 32u);
    }
    static void write_u32(std::ofstream& output, const std::uint32_t value) {
        const std::array<unsigned char, 4u> bytes{
            static_cast<unsigned char>(value),
            static_cast<unsigned char>(value >> 8u),
            static_cast<unsigned char>(value >> 16u),
            static_cast<unsigned char>(value >> 24u)};
        output.write(reinterpret_cast<const char*>(bytes.data()), 4);
    }
    static void write_u64(std::ofstream& output, const std::uint64_t value) {
        write_u32(output, static_cast<std::uint32_t>(value));
        write_u32(output, static_cast<std::uint32_t>(value >> 32u));
    }
    static std::uint16_t read_u16(std::ifstream& input, const char* operation) {
        std::array<unsigned char, 2u> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), 2);
        if (!input)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_HANDLE_EOF,
                          operation);
        return static_cast<std::uint16_t>(bytes[0]) |
               (static_cast<std::uint16_t>(bytes[1]) << 8u);
    }
    static void write_u16(std::ofstream& output, const std::uint16_t value) {
        const std::array<unsigned char, 2u> bytes{
            static_cast<unsigned char>(value),
            static_cast<unsigned char>(value >> 8u)};
        output.write(reinterpret_cast<const char*>(bytes.data()), 2);
    }
    static float read_float(std::ifstream& input, const char* operation) {
        return std::bit_cast<float>(read_u32(input, operation));
    }
    static void write_float(std::ofstream& output, const float value) {
        write_u32(output, std::bit_cast<std::uint32_t>(value));
    }
    static void read_gamepad(std::ifstream& input,
                             NativePortGamepadState& gamepad) {
        const auto connected = read_u32(input, "input-replay-connected");
        if (connected > 1u)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_INVALID_DATA,
                          "input-replay-connected");
        gamepad.connected = connected != 0u;
        gamepad.packet_number = read_u32(input, "input-replay-packet");
        gamepad.buttons = read_u32(input, "input-replay-buttons");
        gamepad.left_stick_x_raw = static_cast<std::int16_t>(
            read_u16(input, "input-replay-axis"));
        gamepad.left_stick_y_raw = static_cast<std::int16_t>(
            read_u16(input, "input-replay-axis"));
        gamepad.right_stick_x_raw = static_cast<std::int16_t>(
            read_u16(input, "input-replay-axis"));
        gamepad.right_stick_y_raw = static_cast<std::int16_t>(
            read_u16(input, "input-replay-axis"));
        const auto left_trigger = read_u32(input, "input-replay-trigger");
        const auto right_trigger = read_u32(input, "input-replay-trigger");
        if (left_trigger > 255u || right_trigger > 255u)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_INVALID_DATA,
                          "input-replay-trigger-range");
        gamepad.left_trigger_raw = static_cast<std::uint8_t>(left_trigger);
        gamepad.right_trigger_raw = static_cast<std::uint8_t>(right_trigger);
        gamepad.left_stick_x = read_float(input, "input-replay-float");
        gamepad.left_stick_y = read_float(input, "input-replay-float");
        gamepad.right_stick_x = read_float(input, "input-replay-float");
        gamepad.right_stick_y = read_float(input, "input-replay-float");
        gamepad.left_trigger = read_float(input, "input-replay-float");
        gamepad.right_trigger = read_float(input, "input-replay-float");
        const auto axes = {gamepad.left_stick_x, gamepad.left_stick_y,
                           gamepad.right_stick_x, gamepad.right_stick_y};
        for (const auto value : axes)
            if (!std::isfinite(value) || value < -1.0f || value > 1.0f)
                fail_platform(NativePortPlatformFailure::InputBackend,
                              ERROR_INVALID_DATA,
                              "input-replay-axis-range");
        if (!std::isfinite(gamepad.left_trigger) ||
            !std::isfinite(gamepad.right_trigger) ||
            gamepad.left_trigger < 0.0f || gamepad.left_trigger > 1.0f ||
            gamepad.right_trigger < 0.0f || gamepad.right_trigger > 1.0f)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_INVALID_DATA,
                          "input-replay-trigger-range");
    }
    static void write_gamepad(std::ofstream& output,
                              const NativePortGamepadState& gamepad) {
        write_u32(output, gamepad.connected ? 1u : 0u);
        write_u32(output, gamepad.packet_number);
        write_u32(output, gamepad.buttons);
        write_u16(output, static_cast<std::uint16_t>(gamepad.left_stick_x_raw));
        write_u16(output, static_cast<std::uint16_t>(gamepad.left_stick_y_raw));
        write_u16(output, static_cast<std::uint16_t>(gamepad.right_stick_x_raw));
        write_u16(output, static_cast<std::uint16_t>(gamepad.right_stick_y_raw));
        write_u32(output, gamepad.left_trigger_raw);
        write_u32(output, gamepad.right_trigger_raw);
        write_float(output, gamepad.left_stick_x);
        write_float(output, gamepad.left_stick_y);
        write_float(output, gamepad.right_stick_x);
        write_float(output, gamepad.right_stick_y);
        write_float(output, gamepad.left_trigger);
        write_float(output, gamepad.right_trigger);
    }

    std::string identity_;
    std::size_t maximum_frames_ = 0u;
    bool replay_ = false;
    bool stop_at_capacity_ = false;
    bool recording_stopped_ = false;
    std::uint32_t incomplete_flags_ = 0u;
    std::vector<NativePortInputSnapshot> frames_;
    std::size_t cursor_ = 0u;
    std::thread journal_flush_worker_;
    std::atomic<bool> journal_flush_stop_ = false;
    std::uint64_t journal_flush_requested_frames_ = 0u;
    std::uint64_t journal_flush_requested_bytes_ = 0u;
    std::atomic<DWORD> journal_flush_error_code_ = ERROR_SUCCESS;
    std::mutex journal_flush_wait_mutex_;
    std::condition_variable journal_flush_wakeup_;
    UniqueHandle journal_file_;
    UniqueHandle journal_mapping_;
    std::byte* journal_view_ = nullptr;
    std::size_t journal_data_offset_ = 0u;
};

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

        const auto& record_path = config.input_record_path;
        const auto& replay_path = config.input_replay_path;
        if (!record_path.empty() && !replay_path.empty())
            fail_platform(NativePortPlatformFailure::InvalidConfig,
                          ERROR_INVALID_PARAMETER,
                          "input-record-replay-exclusive");
        if (!record_path.empty() || !replay_path.empty()) {
            const auto validated_identity = copy_validated_identifier(
                config.input_identity,
                maximum_platform_identifier_bytes,
                "input-identity");
            if (!replay_path.empty()) {
                input_trace_ = NativePortInputTrace::load(
                    replay_path,
                    validated_identity,
                    config.maximum_input_record_frames);
                input_replay_mode_ = true;
            } else {
                input_record_path_ = record_path;
                input_trace_ = std::make_unique<NativePortInputTrace>(
                    validated_identity,
                    config.maximum_input_record_frames,
                    false,
                    config.stop_input_recording_at_capacity,
                    input_record_path_);
            }
        }

        xinput_ = load_xinput();
        // A loaded XInput API or at least one configured WinMM driver slot is
        // independent backend evidence. Merely linking winmm.lib does not
        // prove that this machine has a joystick driver.
        const bool gamepad_backend_available =
            xinput_.get_state != nullptr || joyGetNumDevs() != 0u;
        if (config.require_gamepad_backend && !gamepad_backend_available &&
            !input_replay_mode_)
            fail_platform(NativePortPlatformFailure::InputBackend,
                          ERROR_MOD_NOT_FOUND,
                          "gamepad-backend-load");
        telemetry_->snapshot.native_file_backend = true;
        telemetry_->snapshot.native_gamepad_backend =
            gamepad_backend_available || input_replay_mode_;
        telemetry_->snapshot.native_save_backend = true;
        telemetry_->snapshot.maximum_save_payload_bytes =
            maximum_save_payload_bytes_;
        if (!input_replay_mode_) {
            last_joystick_count_ = joyGetNumDevs();
            const auto identities = enumerate_native_joystick_identities();
            joystick_identities_ = identities.has_value()
                                       ? std::move(*identities)
                                       : std::vector<NativeJoystickIdentity>{};
            start_joystick_identity_worker(!identities.has_value());
        }
    }

    ~Impl() noexcept {
        if (std::this_thread::get_id() != owner_thread_) std::terminate();
        stop_joystick_identity_worker();
        // A typed native-port stop or an exception still unwinds this owner-
        // thread object. Preserve that exact input prefix for the next
        // diagnostic replay; explicit clean finalization remains authoritative
        // and idempotently suppresses this fallback. Hard process termination
        // is outside the guarantees of C++ destruction.
        if (input_trace_ != nullptr && !input_replay_mode_ &&
            !input_recording_finalized_) {
            try {
                input_trace_->save_atomic(input_record_path_);
                input_recording_finalized_ = true;
            } catch (...) {
                // Destructors may not replace the original product failure.
                // The explicit clean path below continues to report I/O
                // failures normally.
            }
        }
        if (xinput_.set_state != nullptr) {
            XINPUT_VIBRATION stopped{};
            for (std::size_t index = 0u;
                 index < vibration_active_.size();
                 ++index) {
                const auto source =
                    xinput_slot_from_device(vibration_device_ids_[index]);
                if (vibration_active_[index] && source.has_value())
                    static_cast<void>(xinput_.set_state(
                        *source, &stopped));
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
        if (input_replay_mode_) {
            input_snapshot_ = input_trace_->next();
            saturating_increment(telemetry_->snapshot.input_polls);
            telemetry_->snapshot.input_connection_generation =
                input_snapshot_.connection_generation;
            return input_snapshot_;
        }
        std::vector<NativeGamepadCandidate> candidates;
        const auto joystick_count = joyGetNumDevs();
        candidates.reserve(XUSER_MAX_COUNT + joystick_count);
        consume_joystick_identity_refresh();
        if (joystick_count != last_joystick_count_) {
            last_joystick_count_ = joystick_count;
            request_joystick_identity_refresh();
        }

        if (xinput_.get_state != nullptr) {
            for (DWORD source_index = 0u;
                 source_index < XUSER_MAX_COUNT;
                 ++source_index) {
                XINPUT_STATE state{};
                const auto status = xinput_.get_state(source_index, &state);
                if (status == ERROR_DEVICE_NOT_CONNECTED) continue;
                if (status != ERROR_SUCCESS) {
                    telemetry_->snapshot.last_platform_error_code = status;
                    fail_platform(NativePortPlatformFailure::InputBackend,
                                  status,
                                  "xinput-poll");
                }
                const auto& source = state.Gamepad;
                NativeGamepadCandidate candidate;
                candidate.device_id = xinput_device_domain |
                                      (static_cast<std::uint64_t>(
                                           source_index) +
                                       1u);
                candidate.kind = NativeGamepadSourceKind::XInput;
                candidate.state.connected = true;
                candidate.state.buttons = xinput_buttons(source.wButtons);
                candidate.state.left_stick_x_raw = source.sThumbLX;
                candidate.state.left_stick_y_raw = source.sThumbLY;
                candidate.state.right_stick_x_raw = source.sThumbRX;
                candidate.state.right_stick_y_raw = source.sThumbRY;
                candidate.state.left_trigger_raw = source.bLeftTrigger;
                candidate.state.right_trigger_raw = source.bRightTrigger;
                candidate.state.left_stick_x =
                    normalized_axis(source.sThumbLX);
                candidate.state.left_stick_y =
                    normalized_axis(source.sThumbLY);
                candidate.state.right_stick_x =
                    normalized_axis(source.sThumbRX);
                candidate.state.right_stick_y =
                    normalized_axis(source.sThumbRY);
                candidate.state.left_trigger =
                    static_cast<float>(source.bLeftTrigger) / 255.0f;
                candidate.state.right_trigger =
                    static_cast<float>(source.bRightTrigger) / 255.0f;
                candidates.push_back(candidate);
            }
        }

        current_connected_joystick_ids_.clear();
        for (UINT source_index = 0u;
             source_index < joystick_count;
             ++source_index) {
            JOYCAPSW capabilities{};
            if (joyGetDevCapsW(source_index,
                               &capabilities,
                               sizeof(capabilities)) != JOYERR_NOERROR)
                continue;
            JOYINFOEX info{};
            info.dwSize = sizeof(info);
            info.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(source_index, &info) != JOYERR_NOERROR) continue;
            current_connected_joystick_ids_.push_back(source_index);
            const auto identity = std::ranges::find(
                joystick_identities_,
                source_index,
                &NativeJoystickIdentity::joystick_id);
            // JOYCAPS describes ranges and button counts, not the semantic
            // layout of an arbitrary HID report. Only identity-bound Sony
            // layouts are admitted through this backend.
            if (identity == joystick_identities_.end()) continue;

            NativeGamepadCandidate candidate;
            candidate.device_id =
                joystick_device_domain | identity->device_identity;
            candidate.kind = identity->kind;
            auto& destination = candidate.state;
            destination.connected = true;
            add_joystick_buttons(destination, info.dwButtons);
            add_pov_buttons(destination.buttons, info.dwPOV);
            destination.left_stick_x_raw = joystick_axis(
                info.dwXpos, capabilities.wXmin, capabilities.wXmax);
            destination.left_stick_y_raw = inverted_axis(joystick_axis(
                info.dwYpos, capabilities.wYmin, capabilities.wYmax));
            if ((capabilities.wCaps & JOYCAPS_HASR) != 0u)
                destination.right_stick_x_raw = joystick_axis(
                    info.dwRpos, capabilities.wRmin, capabilities.wRmax);
            if ((capabilities.wCaps & JOYCAPS_HASU) != 0u)
                destination.right_stick_y_raw = inverted_axis(joystick_axis(
                    info.dwUpos, capabilities.wUmin, capabilities.wUmax));
            else if ((capabilities.wCaps & JOYCAPS_HASV) != 0u)
                destination.right_stick_y_raw = inverted_axis(joystick_axis(
                    info.dwVpos, capabilities.wVmin, capabilities.wVmax));
            if ((capabilities.wCaps & JOYCAPS_HASZ) != 0u &&
                capabilities.wZmax > capabilities.wZmin) {
                const auto trigger_axis = joystick_axis(
                    info.dwZpos, capabilities.wZmin, capabilities.wZmax);
                if (trigger_axis < 0)
                    destination.left_trigger_raw =
                        joystick_half_trigger(trigger_axis);
                else
                    destination.right_trigger_raw =
                        joystick_half_trigger(trigger_axis);
            }
            destination.left_stick_x =
                normalized_axis(destination.left_stick_x_raw);
            destination.left_stick_y =
                normalized_axis(destination.left_stick_y_raw);
            destination.right_stick_x =
                normalized_axis(destination.right_stick_x_raw);
            destination.right_stick_y =
                normalized_axis(destination.right_stick_y_raw);
            destination.left_trigger =
                static_cast<float>(destination.left_trigger_raw) / 255.0f;
            destination.right_trigger =
                static_cast<float>(destination.right_trigger_raw) / 255.0f;
            candidates.push_back(candidate);
        }
        if (current_connected_joystick_ids_ != connected_joystick_ids_) {
            connected_joystick_ids_ = current_connected_joystick_ids_;
            request_joystick_identity_refresh();
        }

        const auto has_device = [&](const std::uint64_t device_id) {
            return std::ranges::any_of(candidates, [&](const auto& candidate) {
                return candidate.device_id == device_id;
            });
        };
        std::vector<std::uint64_t> current_sony_devices;
        for (const auto& candidate : candidates) {
            if (candidate.kind == NativeGamepadSourceKind::DualSense ||
                candidate.kind == NativeGamepadSourceKind::DualShock)
                current_sony_devices.push_back(candidate.device_id);
        }
        std::ranges::sort(current_sony_devices);
        if (current_sony_devices != last_sony_devices_) {
            // Independence is evidence relative to the currently visible
            // Sony endpoint set. A disconnect/reconnect must permit a fresh
            // correlation with an XInput compatibility endpoint.
            last_sony_devices_ = current_sony_devices;
            independent_xinput_devices_.clear();
            cross_backend_correlation_.clear();
        }
        const auto is_assigned_device = [&](const std::uint64_t device_id) {
            return std::ranges::find(input_device_ids_, device_id) !=
                   input_device_ids_.end();
        };
        std::erase_if(cross_backend_aliases_, [&](const auto& alias) {
            return !detail::retain_cross_backend_alias(
                has_device(alias.first), has_device(alias.second),
                is_assigned_device(alias.first),
                is_assigned_device(alias.second));
        });
        std::erase_if(independent_xinput_devices_, [&](const auto device_id) {
            return !has_device(device_id);
        });

        const auto already_paired = [&](const std::uint64_t device_id) {
            return std::ranges::any_of(
                cross_backend_aliases_, [&](const auto& alias) {
                    return alias.first == device_id ||
                           alias.second == device_id;
                });
        };
        const auto known_independent_xinput =
            [&](const std::uint64_t device_id) {
                return std::ranges::find(independent_xinput_devices_,
                                         device_id) !=
                       independent_xinput_devices_.end();
            };
        for (auto& evidence : cross_backend_correlation_)
            evidence.observed = false;
        // A Sony HID plus a remapper-created XInput endpoint can expose one
        // physical pad twice. Bind an alias only after three consecutive,
        // unique, non-neutral correlated observations. Independence never
        // excludes later correlation and is reset when the Sony set changes.
        for (const auto& sony : candidates) {
            if (sony.kind != NativeGamepadSourceKind::DualSense &&
                sony.kind != NativeGamepadSourceKind::DualShock)
                continue;
            if (already_paired(sony.device_id) ||
                !gamepad_has_identity_activity(sony.state))
                continue;
            const NativeGamepadCandidate* matched = nullptr;
            for (const auto& xinput : candidates) {
                if (xinput.kind != NativeGamepadSourceKind::XInput ||
                    already_paired(xinput.device_id) ||
                    !gamepad_has_identity_activity(xinput.state) ||
                    !correlated_gamepad_payload(sony.state, xinput.state))
                    continue;
                if (matched != nullptr) {
                    matched = nullptr;
                    break;
                }
                matched = &xinput;
            }
            if (matched == nullptr) continue;
            std::size_t matching_sony = 0u;
            for (const auto& other : candidates) {
                if ((other.kind == NativeGamepadSourceKind::DualSense ||
                     other.kind == NativeGamepadSourceKind::DualShock) &&
                    !already_paired(other.device_id) &&
                    gamepad_has_identity_activity(other.state) &&
                    correlated_gamepad_payload(other.state, matched->state))
                    ++matching_sony;
            }
            if (matching_sony != 1u) continue;

            std::erase(independent_xinput_devices_, matched->device_id);
            auto evidence = std::ranges::find_if(
                cross_backend_correlation_, [&](const auto& candidate) {
                    return candidate.sony_device_id == sony.device_id &&
                           candidate.xinput_device_id == matched->device_id;
                });
            if (evidence == cross_backend_correlation_.end()) {
                cross_backend_correlation_.push_back(
                    {sony.device_id, matched->device_id, 1u, true});
                evidence = std::prev(cross_backend_correlation_.end());
            } else {
                evidence->observed = true;
                if (evidence->consecutive_samples < 3u)
                    ++evidence->consecutive_samples;
            }
            if (evidence->consecutive_samples >= 3u)
                cross_backend_aliases_.emplace_back(sony.device_id,
                                                    matched->device_id);
        }
        std::erase_if(cross_backend_correlation_, [&](const auto& evidence) {
            return !evidence.observed ||
                   already_paired(evidence.sony_device_id) ||
                   already_paired(evidence.xinput_device_id);
        });

        // While a physical Sony endpoint is present, an otherwise unknown
        // XInput endpoint is withheld from title slots until one active
        // sample proves it independent. This prevents an idle compatibility
        // wrapper from occupying a second controller slot before correlation
        // has any input evidence.
        const auto has_unpaired_sony = std::ranges::any_of(
            candidates, [&](const auto& candidate) {
                return (candidate.kind == NativeGamepadSourceKind::DualSense ||
                        candidate.kind == NativeGamepadSourceKind::DualShock) &&
                       !already_paired(candidate.device_id);
            });
        for (const auto& xinput : candidates) {
            if (xinput.kind != NativeGamepadSourceKind::XInput ||
                already_paired(xinput.device_id) ||
                known_independent_xinput(xinput.device_id))
                continue;
            if (!has_unpaired_sony) {
                independent_xinput_devices_.push_back(xinput.device_id);
                continue;
            }
            if (!gamepad_has_identity_activity(xinput.state)) continue;
            const bool has_correlated_sony = std::ranges::any_of(
                candidates, [&](const auto& sony) {
                    return (sony.kind == NativeGamepadSourceKind::DualSense ||
                            sony.kind == NativeGamepadSourceKind::DualShock) &&
                           !already_paired(sony.device_id) &&
                           gamepad_has_identity_activity(sony.state) &&
                           correlated_gamepad_payload(sony.state,
                                                      xinput.state);
                });
            if (!has_correlated_sony)
                independent_xinput_devices_.push_back(xinput.device_id);
        }
        std::erase_if(candidates, [&](const auto& candidate) {
            if (candidate.kind != NativeGamepadSourceKind::XInput)
                return false;
            const bool aliases_visible_sony = std::ranges::any_of(
                cross_backend_aliases_, [&](const auto& alias) {
                    return alias.second == candidate.device_id &&
                           has_device(alias.first);
                });
            if (aliases_visible_sony) return true;
            return has_unpaired_sony &&
                   !known_independent_xinput(candidate.device_id);
        });

        const auto keyboard = keyboard_gamepad_state();
        if (candidates.empty()) {
            NativeGamepadCandidate candidate;
            candidate.device_id = keyboard_device_domain | 1u;
            candidate.kind = NativeGamepadSourceKind::Keyboard;
            candidate.state = keyboard;
            candidates.push_back(candidate);
        }

        std::ranges::sort(candidates, [](const auto& left, const auto& right) {
            const auto left_priority = source_priority(left.kind);
            const auto right_priority = source_priority(right.kind);
            if (left_priority != right_priority)
                return left_priority < right_priority;
            return left.device_id < right.device_id;
        });

        NativePortInputSnapshot result = input_snapshot_;
        saturating_increment(result.poll_sequence);
        std::array<std::optional<std::size_t>, native_port_gamepad_count>
            placement{};
        std::vector<bool> candidate_used(candidates.size(), false);

        // Keep a connected device in its previous title-visible slot. New
        // devices then fill free slots by backend priority, with physical Sony
        // controllers ahead of possible XInput compatibility wrappers.
        for (std::size_t slot = 0u; slot < placement.size(); ++slot) {
            if (input_device_ids_[slot] == 0u) continue;
            for (std::size_t candidate_index = 0u;
                 candidate_index < candidates.size();
                 ++candidate_index) {
                if (!candidate_used[candidate_index] &&
                    candidates[candidate_index].device_id ==
                        input_device_ids_[slot]) {
                    placement[slot] = candidate_index;
                    candidate_used[candidate_index] = true;
                    break;
                }
            }
        }

        std::size_t free_slot = 0u;
        for (std::size_t candidate_index = 0u;
             candidate_index < candidates.size();
             ++candidate_index) {
            if (candidate_used[candidate_index]) continue;
            while (free_slot < placement.size() &&
                   placement[free_slot].has_value())
                ++free_slot;
            if (free_slot == placement.size()) break;
            placement[free_slot] = candidate_index;
            candidate_used[candidate_index] = true;
        }

        XINPUT_VIBRATION stopped{};
        for (std::size_t slot = 0u; slot < result.gamepads.size(); ++slot) {
            const auto old_device_id = input_device_ids_[slot];
            const auto new_device_id = placement[slot].has_value()
                                           ? candidates[*placement[slot]]
                                                  .device_id
                                           : 0u;
            const bool correlated_handoff =
                detail::is_correlated_backend_handoff(
                    old_device_id, new_device_id, cross_backend_aliases_);
            // A proven Sony<->XInput alias is a source/backend handoff for the
            // same physical pad, not a new title-visible controller. Preserve
            // button debounce, packet continuity and connection generation
            // across that one bounded transition. Every other id change keeps
            // the disconnect/reconnect reset and remains fail-closed.
            const bool same_logical_controller =
                old_device_id == new_device_id || correlated_handoff;
            if (old_device_id != new_device_id) {
                if (vibration_active_[slot] && xinput_.set_state != nullptr) {
                    const auto old_xinput =
                        xinput_slot_from_device(vibration_device_ids_[slot]);
                    if (old_xinput.has_value())
                        static_cast<void>(
                            xinput_.set_state(*old_xinput, &stopped));
                }
                vibration_active_[slot] = false;
                vibration_device_ids_[slot] = 0u;
                if (!correlated_handoff)
                    saturating_increment(result.connection_generation);
            }

            if (!placement[slot].has_value()) {
                result.gamepads[slot] = {};
                input_device_ids_[slot] = 0u;
                input_button_stability_[slot] = {};
                continue;
            }

            auto destination = candidates[*placement[slot]].state;
            if (slot == 0u)
                merge_keyboard_gamepad(destination, keyboard);
            if (!same_logical_controller)
                input_button_stability_[slot] = {
                    destination.buttons, {}};
            else
                destination.buttons = stabilize_gamepad_buttons(
                    input_button_stability_[slot], destination.buttons);
            const auto& previous = input_snapshot_.gamepads[slot];
            if (same_logical_controller && previous.connected) {
                destination.packet_number = previous.packet_number;
                if (!same_gamepad_payload(previous, destination) &&
                    destination.packet_number !=
                        std::numeric_limits<std::uint32_t>::max())
                    ++destination.packet_number;
            } else {
                destination.packet_number = 1u;
            }
            result.gamepads[slot] = destination;
            input_device_ids_[slot] = new_device_id;
        }
        input_snapshot_ = result;
        saturating_increment(telemetry_->snapshot.input_polls);
        telemetry_->snapshot.input_connection_generation =
            input_snapshot_.connection_generation;
        if (input_trace_ != nullptr) input_trace_->append(input_snapshot_);
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
        if (input_replay_mode_) return false;
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
        const auto source = vibration_xinput_slot(
            input_device_ids_[controller_index]);
        if (!source.has_value()) return false;
        XINPUT_VIBRATION state{};
        state.wLeftMotorSpeed = static_cast<WORD>(std::lround(
            vibration.low_frequency * 65'535.0f));
        state.wRightMotorSpeed = static_cast<WORD>(std::lround(
            vibration.high_frequency * 65'535.0f));
        const auto status = xinput_.set_state(*source, &state);
        if (status == ERROR_DEVICE_NOT_CONNECTED) {
            vibration_active_[controller_index] = false;
            vibration_device_ids_[controller_index] = 0u;
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
        vibration_device_ids_[controller_index] =
            xinput_device_domain | (static_cast<std::uint64_t>(*source) + 1u);
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

    void finalize_clean_shutdown() {
        require_owner_thread();
        if (input_trace_ != nullptr && !input_replay_mode_ &&
            !input_recording_finalized_) {
            input_trace_->save_atomic(input_record_path_);
            input_recording_finalized_ = true;
        }
    }

  private:
    struct InternalSaveLoad final {
        NativePortSaveLoadResult public_result;
        std::uint64_t bytes_read = 0u;
    };

    [[nodiscard]] std::optional<DWORD> vibration_xinput_slot(
        const std::uint64_t device_id) const noexcept {
        if (const auto direct = xinput_slot_from_device(device_id);
            direct.has_value())
            return direct;
        const auto alias = std::ranges::find_if(
            cross_backend_aliases_, [&](const auto& binding) {
                return binding.first == device_id;
            });
        if (alias == cross_backend_aliases_.end()) return std::nullopt;
        return xinput_slot_from_device(alias->second);
    }

    static void validate_config(const NativePortPlatformConfig& config) {
        if (config.contract_version != native_port_platform_contract_version ||
            config.content_root.empty() || config.user_data_root.empty() ||
            !valid_path_identifier(config.project_id,
                                   maximum_platform_identifier_bytes) ||
            config.maximum_content_file_bytes == 0u ||
            config.maximum_content_file_bytes > maximum_content_budget_bytes ||
            config.maximum_save_payload_bytes == 0u ||
            config.maximum_save_payload_bytes > maximum_save_budget_bytes ||
            config.maximum_input_record_frames == 0u ||
            config.maximum_input_record_frames >
                native_port_input_recording_maximum_frames)
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

    void start_joystick_identity_worker(const bool retry_immediately) {
        joystick_identity_stop_.store(false, std::memory_order_relaxed);
        joystick_identity_refresh_requested_.store(
            retry_immediately, std::memory_order_relaxed);
        joystick_identity_worker_ = std::thread([this]() noexcept {
            // DirectInput device construction can take tens of milliseconds
            // on Windows.  It is discovery work, never part of title input
            // sampling, so keep it below the timing-critical owner thread.
            static_cast<void>(SetThreadPriority(
                GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));
            auto refresh_delay = std::chrono::seconds(10);
            for (;;) {
                {
                    std::unique_lock lock(joystick_identity_wait_mutex_);
                    joystick_identity_wakeup_.wait_for(
                        lock, refresh_delay, [this]() noexcept {
                            return joystick_identity_stop_.load(
                                       std::memory_order_acquire) ||
                                   joystick_identity_refresh_requested_.load(
                                       std::memory_order_acquire);
                        });
                }
                if (joystick_identity_stop_.load(std::memory_order_acquire))
                    return;
                // A request arriving before this exchange is satisfied by the
                // enumeration below; one arriving afterwards remains set for
                // the next loop and cannot be lost.
                static_cast<void>(joystick_identity_refresh_requested_.exchange(
                    false, std::memory_order_acq_rel));

                std::optional<std::vector<NativeJoystickIdentity>> identities;
                try {
                    identities = enumerate_native_joystick_identities();
                } catch (...) {
                    identities = std::nullopt;
                }
                if (joystick_identity_stop_.load(std::memory_order_acquire))
                    return;
                {
                    std::scoped_lock lock(joystick_identity_pending_mutex_);
                    joystick_identity_pending_ =
                        identities.has_value()
                            ? std::move(*identities)
                            : std::vector<NativeJoystickIdentity>{};
                    joystick_identity_pending_ready_ = true;
                }
                // Periodic refresh covers driver-side identity changes that
                // WinMM does not expose through joyGetNumDevs().  Connected
                // endpoint changes request an immediate refresh from poll().
                refresh_delay = identities.has_value()
                                    ? std::chrono::seconds(10)
                                    : std::chrono::seconds(5);
            }
        });
    }

    void stop_joystick_identity_worker() noexcept {
        {
            // The state transition shares the wait mutex with the predicate
            // check so notify cannot land between that check and sleeping.
            std::scoped_lock lock(joystick_identity_wait_mutex_);
            joystick_identity_stop_.store(true, std::memory_order_release);
        }
        joystick_identity_wakeup_.notify_one();
        if (joystick_identity_worker_.joinable())
            joystick_identity_worker_.join();
    }

    void request_joystick_identity_refresh() noexcept {
        if (!joystick_identity_worker_.joinable()) return;
        {
            std::scoped_lock lock(joystick_identity_wait_mutex_);
            joystick_identity_refresh_requested_.store(
                true, std::memory_order_release);
        }
        joystick_identity_wakeup_.notify_one();
    }

    void consume_joystick_identity_refresh() {
        std::unique_lock lock(joystick_identity_pending_mutex_,
                              std::try_to_lock);
        if (!lock.owns_lock() || !joystick_identity_pending_ready_) return;
        // Swapping keeps destruction/allocation of the previous catalog on
        // the discovery worker's next publication, not on the frame thread.
        joystick_identities_.swap(joystick_identity_pending_);
        joystick_identity_pending_ready_ = false;
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
    std::vector<NativeJoystickIdentity> joystick_identities_;
    UINT last_joystick_count_ = 0u;
    std::vector<UINT> connected_joystick_ids_;
    std::vector<UINT> current_connected_joystick_ids_;
    std::thread joystick_identity_worker_;
    std::atomic<bool> joystick_identity_stop_ = false;
    std::atomic<bool> joystick_identity_refresh_requested_ = false;
    std::mutex joystick_identity_wait_mutex_;
    std::condition_variable joystick_identity_wakeup_;
    std::mutex joystick_identity_pending_mutex_;
    std::vector<NativeJoystickIdentity> joystick_identity_pending_;
    bool joystick_identity_pending_ready_ = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>>
        cross_backend_aliases_;
    std::vector<CrossBackendCorrelationEvidence>
        cross_backend_correlation_;
    std::vector<std::uint64_t> independent_xinput_devices_;
    std::vector<std::uint64_t> last_sony_devices_;
    std::unique_ptr<NativePortInputTrace> input_trace_;
    std::filesystem::path input_record_path_;
    bool input_replay_mode_ = false;
    bool input_recording_finalized_ = false;
    NativePortInputSnapshot input_snapshot_;
    std::array<std::uint64_t, native_port_gamepad_count> input_device_ids_{};
    std::array<NativeGamepadButtonStability, native_port_gamepad_count>
        input_button_stability_{};
    std::array<std::uint64_t, native_port_gamepad_count>
        vibration_device_ids_{};
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
    void finalize_clean_shutdown() {}

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

NativePortReadOnlyContentRange::NativePortReadOnlyContentRange(
    std::unique_ptr<NativePortReadOnlyFile> file,
    const std::uint64_t logical_offset,
    const std::uint64_t logical_byte_size,
    const std::byte trailing_fill)
    : file_(std::move(file)),
      logical_offset_(logical_offset),
      logical_byte_size_(logical_byte_size),
      trailing_fill_(trailing_fill) {}

NativePortReadOnlyContentRange::~NativePortReadOnlyContentRange() = default;

std::string_view NativePortReadOnlyContentRange::logical_id() const noexcept {
    return file_->logical_id();
}

std::uint64_t NativePortReadOnlyContentRange::logical_offset() const noexcept {
    return logical_offset_;
}

std::uint64_t
NativePortReadOnlyContentRange::logical_byte_size() const noexcept {
    return logical_byte_size_;
}

bool NativePortReadOnlyContentRange::contains(
    const std::uint64_t offset,
    const std::uint64_t byte_size) const noexcept {
    return offset >= logical_offset_ && offset - logical_offset_ <=
               logical_byte_size_ &&
           byte_size <= logical_byte_size_ - (offset - logical_offset_);
}

void NativePortReadOnlyContentRange::read_at(
    const std::uint64_t offset,
    const std::span<std::byte> destination) {
    if (!contains(offset, destination.size()))
        fail_platform(NativePortPlatformFailure::ContentRead,
                      0u,
                      "content-logical-range");
    const auto relative = offset - logical_offset_;
    const auto exact_bytes = file_->byte_size();
    if (relative < exact_bytes) {
        const auto readable = static_cast<std::size_t>(
            std::min<std::uint64_t>(destination.size(),
                                    exact_bytes - relative));
        file_->read_at(relative, destination.first(readable));
        std::fill(destination.begin() + readable,
                  destination.end(),
                  trailing_fill_);
    } else {
        std::fill(destination.begin(), destination.end(), trailing_fill_);
    }
}

NativePortContentFileSnapshot
NativePortReadOnlyContentRange::snapshot() const {
    return file_->snapshot();
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

std::unique_ptr<NativePortReadOnlyContentRange>
NativePortPlatformServices::open_content_range(
    const NativePortContentRangeBinding& binding) {
    if (binding.logical_byte_size < binding.file.byte_size ||
        binding.logical_byte_size - binding.file.byte_size >
            native_port_content_range_maximum_trailing_fill_bytes ||
        binding.logical_offset >
            std::numeric_limits<std::uint64_t>::max() -
                binding.logical_byte_size)
        fail_platform(NativePortPlatformFailure::InvalidConfig,
                      0u,
                      "content-logical-binding");
    auto file = impl_->open_content_file(binding.file);
    return std::unique_ptr<NativePortReadOnlyContentRange>(
        new NativePortReadOnlyContentRange(std::move(file),
                                           binding.logical_offset,
                                           binding.logical_byte_size,
                                           binding.trailing_fill));
}

NativePortInputSnapshot NativePortPlatformServices::poll_gamepads() {
    return impl_->poll_gamepads();
}

void NativePortPlatformServices::finalize_clean_shutdown() {
    impl_->finalize_clean_shutdown();
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
