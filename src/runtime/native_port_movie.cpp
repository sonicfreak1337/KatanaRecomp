#include "katana/runtime/native_port_movie.hpp"

#include "katana/runtime/native_port.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::uint32_t maximum_audio_queue_budget_frames = 192'000u * 60u;
constexpr std::uint32_t maximum_video_queue_budget_frames = 256u;
constexpr std::uint64_t maximum_video_frame_budget_bytes = 128u * 1024u * 1024u;
constexpr std::uint64_t maximum_video_queue_budget_bytes = 128u * 1024u * 1024u;
constexpr std::uint32_t maximum_display_aspect_component = 65'535u;
constexpr std::uint32_t movie_audio_batch_frames = 1'024u;
constexpr std::uint64_t movie_audio_timestamp_tolerance_nanoseconds = 1'000'000u;

struct DisplayAspect final {
    std::uint32_t numerator = 0u;
    std::uint32_t denominator = 0u;
};

[[nodiscard]] DisplayAspect reduced_display_aspect(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t pixel_numerator,
    const std::uint32_t pixel_denominator) noexcept {
    if (width == 0u || height == 0u || pixel_numerator == 0u ||
        pixel_denominator == 0u)
        return {};
    std::uint64_t numerator =
        static_cast<std::uint64_t>(width) * pixel_numerator;
    std::uint64_t denominator =
        static_cast<std::uint64_t>(height) * pixel_denominator;
    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    while (numerator > maximum_display_aspect_component ||
           denominator > maximum_display_aspect_component) {
        numerator = (numerator + 1u) / 2u;
        denominator = (denominator + 1u) / 2u;
    }
    return {static_cast<std::uint32_t>(numerator),
            static_cast<std::uint32_t>(denominator)};
}

[[nodiscard]] bool valid_display_aspect(
    const std::uint32_t numerator,
    const std::uint32_t denominator) noexcept {
    return numerator != 0u && denominator != 0u &&
           numerator <= maximum_display_aspect_component &&
           denominator <= maximum_display_aspect_component;
}

[[nodiscard]] bool path_is_within(const std::filesystem::path& path,
                                  const std::filesystem::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

[[nodiscard]] bool valid_content_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) return false;
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") return false;
    }
    return true;
}

void saturating_add_counter(std::uint64_t& destination, const std::uint64_t value) noexcept {
    destination = value > std::numeric_limits<std::uint64_t>::max() - destination
                      ? std::numeric_limits<std::uint64_t>::max()
                      : destination + value;
}

void saturating_increment_counter(std::uint64_t& destination) noexcept {
    saturating_add_counter(destination, 1u);
}

[[nodiscard]] std::uint64_t audio_frames_to_nanoseconds(
    const std::uint64_t frames,
    const std::uint32_t sample_rate) noexcept {
    if (sample_rate == 0u) return 0u;
    constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000u;
    const auto seconds = frames / sample_rate;
    const auto remainder = frames % sample_rate;
    if (seconds > std::numeric_limits<std::uint64_t>::max() /
                      nanoseconds_per_second)
        return std::numeric_limits<std::uint64_t>::max();
    auto result = seconds * nanoseconds_per_second;
    const auto fraction = remainder * nanoseconds_per_second / sample_rate;
    saturating_add_counter(result, fraction);
    return result;
}

void require_safe_existing_path(const std::filesystem::path& path) {
    const auto normalized = std::filesystem::absolute(path).lexically_normal();
    auto current = normalized.root_path();
    for (const auto& component : normalized.relative_path()) {
        if (component.empty() || component == ".") continue;
        if (component == "..") throw std::runtime_error("native-port-movie-content-parent");
        current /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error || std::filesystem::is_symlink(status))
            throw std::runtime_error("native-port-movie-content-link");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            throw std::runtime_error("native-port-movie-content-reparse");
#endif
    }
}

struct VerifiedMediaPath final {
    std::filesystem::path root;
    std::filesystem::path file;
};

[[nodiscard]] VerifiedMediaPath verified_media_path(const NativePortMovieSource& source) {
    if (!valid_native_port_sha256_identity(source.byte_identity) || source.content_root.empty() ||
        !valid_content_relative_path(source.content_relative_path))
        throw std::invalid_argument("native-port-movie-source");
    const auto unresolved_root = std::filesystem::absolute(source.content_root).lexically_normal();
    require_safe_existing_path(unresolved_root);
    const auto root = std::filesystem::canonical(unresolved_root);
    if (!std::filesystem::is_directory(root))
        throw std::runtime_error("native-port-movie-content-root");
    const auto unresolved = (root / source.content_relative_path).lexically_normal();
    require_safe_existing_path(unresolved);
    const auto path = std::filesystem::canonical(unresolved);
    const auto status = std::filesystem::symlink_status(path);
    if (!path_is_within(path, root) || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        throw std::runtime_error("native-port-movie-content-boundary");
#ifdef _WIN32
    const auto root_attributes = GetFileAttributesW(root.c_str());
    const auto file_attributes = GetFileAttributesW(path.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES || file_attributes == INVALID_FILE_ATTRIBUTES ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
        (file_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        throw std::runtime_error("native-port-movie-content-reparse");
#endif
    return {root, path};
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

class PlatformError final : public std::runtime_error {
  public:
    PlatformError(const char* operation, const std::uint32_t code)
        : std::runtime_error(std::string("native-port-movie-") + operation + ":" +
                             std::to_string(code == 0u ? 1u : code)),
          code_(code == 0u ? 1u : code),
          content_operation_(std::string_view(operation).starts_with("content-") ||
                             std::string_view(operation).starts_with("sha256-")) {}
    [[nodiscard]] std::uint32_t code() const noexcept {
        return code_;
    }
    [[nodiscard]] bool content_operation() const noexcept {
        return content_operation_;
    }

  private:
    std::uint32_t code_;
    bool content_operation_;
};

[[noreturn]] void throw_platform(const char* operation, const std::uint32_t code) {
    throw PlatformError(operation, code);
}

void require_hresult(const HRESULT result, const char* operation) {
    if (FAILED(result)) throw_platform(operation, static_cast<std::uint32_t>(result));
}

[[nodiscard]] std::filesystem::path final_path_from_handle(const HANDLE file) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const auto required = GetFinalPathNameByHandleW(file, nullptr, 0u, flags);
    if (required == 0u) throw_platform("content-final-path-size", GetLastError());
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1u);
    const auto copied =
        GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (copied == 0u || copied >= buffer.size())
        throw_platform("content-final-path", GetLastError());
    std::wstring value(buffer.data(), copied);
    constexpr std::wstring_view unc_prefix = LR"(\\?\UNC\)";
    constexpr std::wstring_view local_prefix = LR"(\\?\)";
    if (value.starts_with(unc_prefix))
        value = LR"(\\)" + value.substr(unc_prefix.size());
    else if (value.starts_with(local_prefix))
        value.erase(0u, local_prefix.size());
    return std::filesystem::path(std::move(value)).lexically_normal();
}

[[nodiscard]] bool path_is_within_case_insensitive(const std::filesystem::path& path,
                                                   const std::filesystem::path& root) noexcept {
    auto path_component = path.begin();
    for (auto root_component = root.begin(); root_component != root.end();
         ++root_component, ++path_component) {
        if (path_component == path.end()) return false;
        const auto left = path_component->native();
        const auto right = root_component->native();
        if (CompareStringOrdinal(left.c_str(),
                                 static_cast<int>(left.size()),
                                 right.c_str(),
                                 static_cast<int>(right.size()),
                                 TRUE) != CSTR_EQUAL)
            return false;
    }
    return path_component != path.end();
}

[[nodiscard]] NativePortMovieFailure
map_media_foundation_failure(const std::uint32_t code) noexcept {
    switch (static_cast<HRESULT>(code)) {
    case MF_E_UNSUPPORTED_BYTESTREAM_TYPE:
    case MF_E_UNSUPPORTED_SCHEME:
    case MF_E_UNSUPPORTED_REPRESENTATION:
    case MF_E_UNSUPPORTED_SERVICE:
    case MF_E_UNSUPPORTED_FORMAT:
    case MF_E_FORMAT_CHANGE_NOT_SUPPORTED:
    case MF_E_CANNOT_PARSE_BYTESTREAM:
    case MF_E_MP3_NOTSUPPORTED:
    case MF_E_TOPO_CODEC_NOT_FOUND:
    case MF_E_TOPO_UNSUPPORTED:
    case MF_E_INVALIDMEDIATYPE:
    case MF_E_INVALIDTYPE:
    case MF_E_NO_MORE_TYPES:
    case MF_E_INVALID_FILE_FORMAT:
    case MF_E_TRANSFORM_NOT_POSSIBLE_FOR_CURRENT_OUTPUT_MEDIATYPE:
    case MF_E_TRANSFORM_NOT_POSSIBLE_FOR_CURRENT_INPUT_MEDIATYPE:
    case MF_E_TRANSFORM_NOT_POSSIBLE_FOR_CURRENT_MEDIATYPE_COMBINATION:
        return NativePortMovieFailure::DecoderUnavailable;
    default:
        return NativePortMovieFailure::HostMediaFailure;
    }
}

[[nodiscard]] std::string sha256_file_handle(const HANDLE file) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, 32u> digest{};
    auto require_status = [](const NTSTATUS status, const char* operation) {
        if (status < 0) throw_platform(operation, static_cast<std::uint32_t>(status));
    };
    try {
        require_status(
            BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0u),
            "sha256-open");
        DWORD object_size = 0u;
        DWORD copied = 0u;
        require_status(BCryptGetProperty(algorithm,
                                         BCRYPT_OBJECT_LENGTH,
                                         reinterpret_cast<PUCHAR>(&object_size),
                                         sizeof(object_size),
                                         &copied,
                                         0u),
                       "sha256-object-size");
        object.resize(object_size);
        require_status(BCryptCreateHash(algorithm,
                                        &hash,
                                        object.data(),
                                        static_cast<ULONG>(object.size()),
                                        nullptr,
                                        0u,
                                        0u),
                       "sha256-create");
        LARGE_INTEGER zero{};
        if (SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) == FALSE)
            throw_platform("content-seek", GetLastError());
        std::array<std::uint8_t, 256u * 1024u> buffer{};
        for (;;) {
            DWORD bytes = 0u;
            if (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr) ==
                FALSE)
                throw_platform("content-read", GetLastError());
            if (bytes != 0u)
                require_status(BCryptHashData(hash, buffer.data(), bytes, 0u), "sha256-update");
            if (bytes == 0u) break;
        }
        require_status(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0u),
                       "sha256-finish");
        if (SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) == FALSE)
            throw_platform("content-rewind", GetLastError());
    } catch (...) {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0u);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0u);
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(7u + digest.size() * 2u);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4u]);
        result.push_back(hex[byte & 0x0fu]);
    }
    return result;
}

[[nodiscard]] std::uint64_t hundred_ns_to_ns(const LONGLONG value) noexcept {
    if (value <= 0) return 0u;
    const auto positive = static_cast<std::uint64_t>(value);
    return positive > std::numeric_limits<std::uint64_t>::max() / 100u
               ? std::numeric_limits<std::uint64_t>::max()
               : positive * 100u;
}

class ReadOnlyByteStream final : public IStream {
  public:
    explicit ReadOnlyByteStream(const NativePortCodecByteSource source) : source_(source) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (id == IID_IUnknown || id == IID_ISequentialStream || id == IID_IStream) {
            *object = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0u) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Read(void* destination, ULONG requested, ULONG* bytes_read) override {
        if (bytes_read != nullptr) *bytes_read = 0u;
        if (requested != 0u && destination == nullptr) return STG_E_INVALIDPOINTER;
        const std::scoped_lock lock(mutex_);
        if (position_ > source_.byte_size) return STG_E_SEEKERROR;
        const auto available = source_.byte_size - position_;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(available, requested));
        if (count != 0u && !source_.read_at(source_.user, position_, destination, count))
            return STG_E_READFAULT;
        position_ += count;
        if (bytes_read != nullptr) *bytes_read = static_cast<ULONG>(count);
        return count == requested ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER displacement,
                                   DWORD origin,
                                   ULARGE_INTEGER* new_position) override {
        const std::scoped_lock lock(mutex_);
        std::uint64_t base = 0u;
        switch (origin) {
        case STREAM_SEEK_SET:
            base = 0u;
            break;
        case STREAM_SEEK_CUR:
            base = position_;
            break;
        case STREAM_SEEK_END:
            base = source_.byte_size;
            break;
        default:
            return STG_E_INVALIDFUNCTION;
        }
        std::uint64_t result = 0u;
        if (displacement.QuadPart < 0) {
            const auto magnitude = static_cast<std::uint64_t>(-(displacement.QuadPart + 1)) + 1u;
            if (magnitude > base) return STG_E_SEEKERROR;
            result = base - magnitude;
        } else {
            const auto add = static_cast<std::uint64_t>(displacement.QuadPart);
            if (add > std::numeric_limits<std::uint64_t>::max() - base) return STG_E_SEEKERROR;
            result = base + add;
        }
        position_ = result;
        if (new_position != nullptr) new_position->QuadPart = result;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
        return STG_E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*,
                                     ULARGE_INTEGER,
                                     ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Revert() override {
        return STG_E_REVERTED;
    }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* status, DWORD flags) override {
        if (status == nullptr) return STG_E_INVALIDPOINTER;
        *status = {};
        status->type = STGTY_STREAM;
        status->cbSize.QuadPart = source_.byte_size;
        status->grfMode = STGM_READ;
        if ((flags & STATFLAG_NONAME) == 0u) status->pwcsName = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** clone) override {
        if (clone == nullptr) return E_POINTER;
        *clone = nullptr;
        auto* result = new (std::nothrow) ReadOnlyByteStream(source_);
        if (result == nullptr) return E_OUTOFMEMORY;
        {
            const std::scoped_lock lock(mutex_);
            result->position_ = position_;
        }
        *clone = result;
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1u};
    NativePortCodecByteSource source_;
    std::mutex mutex_;
    std::uint64_t position_ = 0u;
};
#endif

} // namespace

NativePortMovieError::NativePortMovieError(const NativePortMovieFailure failure,
                                           const std::uint32_t platform_error_code,
                                           const std::string_view operation)
    : std::runtime_error("native-port-movie-" + std::string(operation) + ":" +
                         std::to_string(static_cast<std::uint32_t>(failure)) + ":" +
                         std::to_string(platform_error_code)),
      failure_(failure), platform_error_code_(platform_error_code) {}

NativePortMovieFailure NativePortMovieError::failure() const noexcept {
    return failure_;
}

std::uint32_t NativePortMovieError::platform_error_code() const noexcept {
    return platform_error_code_;
}

class NativePortMovieSession::Impl final {
  public:
    ~Impl() {
        close_backend();
    }

    void open(const NativePortMovieConfig& config) {
        bind_or_require_owner_thread();
        require_not_callback();
        if (state_ != NativePortMovieState::Closed && state_ != NativePortMovieState::Stopped)
            throw std::logic_error("native-port-movie-already-open");
        reset_for_open();
        config_ = config;
        content_identity_.assign(config.source.byte_identity);
        config_.source.byte_identity = content_identity_;
        callbacks_ = config.callbacks;
        if (config.audio_sample_rate < 8'000u || config.audio_sample_rate > 192'000u ||
            config.maximum_audio_queue_frames == 0u ||
            config.maximum_audio_queue_frames > maximum_audio_queue_budget_frames ||
            config.maximum_video_queue_frames == 0u ||
            config.maximum_video_queue_frames > maximum_video_queue_budget_frames ||
            config.maximum_video_frame_bytes < 4u ||
            config.maximum_video_frame_bytes > maximum_video_frame_budget_bytes ||
            config.maximum_video_queue_bytes < 4u ||
            config.maximum_video_queue_bytes > maximum_video_queue_budget_bytes ||
            config.maximum_video_frame_bytes > config.maximum_video_queue_bytes ||
            !((config.video_display_aspect_numerator == 0u &&
               config.video_display_aspect_denominator == 0u) ||
              valid_display_aspect(config.video_display_aspect_numerator,
                                   config.video_display_aspect_denominator)) ||
            (!config.require_audio && !config.require_video))
            return fail_and_throw(NativePortMovieFailure::InvalidConfig, "config");
        try {
            const auto verified = verified_media_path(config.source);
            content_root_path_ = verified.root;
            path_ = verified.file;
#ifdef _WIN32
            open_identity_locked_content();
            if (sha256_file_handle(content_handle_) != content_identity_)
                return fail_and_throw(NativePortMovieFailure::ContentIdentity, "content-identity");
            if (config.codec_provider != nullptr)
                initialize_codec_provider(*config.codec_provider);
            else
                initialize_backend();
#else
            return fail_and_throw(NativePortMovieFailure::UnsupportedHost, "unsupported-host");
#endif
            transition(NativePortMovieState::Ready);
            apply_deferred_stop();
#ifdef _WIN32
        } catch (const PlatformError& error) {
            transition(NativePortMovieState::Failed,
                       error.content_operation() ? NativePortMovieFailure::ContentLoad
                                                 : map_media_foundation_failure(error.code()),
                       error.code());
            close_backend(false);
            throw NativePortMovieError(failure_, error.code(), "media-foundation-open");
#endif
        } catch (const NativePortMovieError&) {
            close_backend(false);
            throw;
        } catch (...) {
            if (state_ != NativePortMovieState::Failed)
                transition(NativePortMovieState::Failed, NativePortMovieFailure::ContentLoad);
            close_backend(false);
            throw NativePortMovieError(failure_, platform_error_code_, "open");
        }
    }

    void play(const std::uint64_t host_time) {
        require_owner_thread();
        require_not_callback();
        if (state_ == NativePortMovieState::Ready) {
            anchor_host_time_ = host_time;
            last_host_time_ = host_time;
            position_ = 0u;
        } else if (state_ == NativePortMovieState::Paused) {
            if (host_time < pause_host_time_)
                return fail_and_close(NativePortMovieFailure::HostTimeRegression,
                                      "host-time-regression");
            const auto paused_duration = host_time - pause_host_time_;
            anchor_host_time_ += paused_duration;
            if (audio_tail_clock_started_)
                audio_tail_anchor_host_time_ += paused_duration;
            last_host_time_ = host_time;
            if (audio_) {
                try {
                    audio_->resume();
                } catch (...) {
                    return fail_and_close(NativePortMovieFailure::HostAudioFailure, "audio-resume");
                }
            }
        } else {
            throw std::logic_error("native-port-movie-not-playable");
        }
        transition(NativePortMovieState::Playing);
        apply_deferred_stop();
    }

    void pump(const std::uint64_t host_time) {
        require_owner_thread();
        require_not_callback();
        if (state_ == NativePortMovieState::Completed || state_ == NativePortMovieState::Stopped)
            return;
        if (state_ != NativePortMovieState::Playing)
            throw std::logic_error("native-port-movie-not-playing");
        if (host_time < last_host_time_)
            return fail_and_close(NativePortMovieFailure::HostTimeRegression,
                                  "host-time-regression");
        last_host_time_ = host_time;
        try {
            if (audio_) {
                audio_->poll();
                audio_->refresh_playback_position();
            }
            update_playback_position(host_time);
#ifdef _WIN32
            if (codec_provider_ != nullptr)
                decode_provider_until_position();
            else
                decode_until_position();
#endif
            if (state_ == NativePortMovieState::Stopped) return;
            complete_if_drained();
            apply_deferred_stop();
#ifdef _WIN32
        } catch (const PlatformError& error) {
            transition(NativePortMovieState::Failed,
                       map_media_foundation_failure(error.code()),
                       error.code());
            close_backend(false);
            throw NativePortMovieError(failure_, error.code(), "media-foundation-pump");
#endif
        } catch (const NativePortMovieError&) {
            close_backend(false);
            throw;
        } catch (...) {
            if (state_ != NativePortMovieState::Failed)
                transition(NativePortMovieState::Failed,
                           audio_ != nullptr &&
                                   audio_->snapshot().state == NativePortAudioState::Failed
                               ? NativePortMovieFailure::HostAudioFailure
                               : NativePortMovieFailure::HostMediaFailure);
            close_backend(false);
            throw NativePortMovieError(failure_, platform_error_code_, "pump");
        }
    }

    void pause(const std::uint64_t host_time) {
        require_owner_thread();
        require_not_callback();
        if (state_ != NativePortMovieState::Playing)
            throw std::logic_error("native-port-movie-not-playing");
        pump(host_time);
        if (state_ != NativePortMovieState::Playing) return;
        pause_host_time_ = host_time;
        if (audio_) {
            try {
                audio_->pause();
            } catch (...) {
                return fail_and_close(NativePortMovieFailure::HostAudioFailure, "audio-pause");
            }
        }
        transition(NativePortMovieState::Paused);
        apply_deferred_stop();
    }

    void stop() {
        require_owner_thread();
        if (callback_active_) {
            deferred_stop_ = true;
            return;
        }
        if (state_ == NativePortMovieState::Closed || state_ == NativePortMovieState::Stopped)
            return;
        close_backend(false);
        transition(NativePortMovieState::Stopped);
    }

    [[nodiscard]] NativePortMovieSnapshot snapshot() const noexcept {
        return {state_,
                duration_,
                position_,
                decoded_audio_frames_,
                decoded_video_frames_,
                presented_video_frames_,
                failure_,
                platform_error_code_};
    }

  private:
#ifdef _WIN32
    struct PendingSample final {
        DWORD stream = 0u;
        DWORD flags = 0u;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        std::uint32_t video_width = 0u;
        std::uint32_t video_height = 0u;
        std::uint32_t video_stride = 0u;
        std::uint32_t video_display_aspect_numerator = 0u;
        std::uint32_t video_display_aspect_denominator = 0u;
        bool video_bottom_up = false;
        std::uint64_t video_storage_bytes = 0u;
    };

    struct ProviderSample final {
        NativePortCodecSampleKind kind = NativePortCodecSampleKind::AudioEnd;
        std::uint64_t timestamp = 0u;
        std::uint64_t duration = 0u;
        NativePortAudioFormat audio_format;
        std::vector<std::int16_t> audio_samples;
        std::uint32_t video_width = 0u;
        std::uint32_t video_height = 0u;
        std::uint32_t video_stride = 0u;
        std::uint32_t video_display_aspect_numerator = 0u;
        std::uint32_t video_display_aspect_denominator = 0u;
        bool video_bottom_up = false;
        std::vector<std::byte> video_pixels;
    };

    void open_identity_locked_content() {
        content_handle_ = CreateFileW(path_.c_str(),
                                      GENERIC_READ,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        if (content_handle_ == INVALID_HANDLE_VALUE) {
            content_handle_ = nullptr;
            throw_platform("content-open", GetLastError());
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (GetFileInformationByHandleEx(
                content_handle_, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE) {
            const auto error = GetLastError();
            static_cast<void>(CloseHandle(content_handle_));
            content_handle_ = nullptr;
            throw_platform("content-handle-info", error);
        }
        if ((attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0u) {
            static_cast<void>(CloseHandle(content_handle_));
            content_handle_ = nullptr;
            throw_platform("content-handle-type", ERROR_ACCESS_DENIED);
        }
        const auto final_path = final_path_from_handle(content_handle_);
        if (!path_is_within_case_insensitive(final_path, content_root_path_)) {
            static_cast<void>(CloseHandle(content_handle_));
            content_handle_ = nullptr;
            throw_platform("content-handle-boundary", ERROR_ACCESS_DENIED);
        }
        content_lock_ = {};
        if (LockFileEx(content_handle_,
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0u,
                       std::numeric_limits<DWORD>::max(),
                       std::numeric_limits<DWORD>::max(),
                       &content_lock_) == FALSE)
            throw_platform("content-lock", GetLastError());
        content_lock_held_ = true;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(content_handle_, &size) == FALSE || size.QuadPart < 0)
            throw_platform("content-size", GetLastError());
        content_size_ = static_cast<std::uint64_t>(size.QuadPart);
    }

    static std::uint32_t provider_read_at(void* const user,
                                          const std::uint64_t offset,
                                          void* const destination,
                                          const std::uint64_t byte_count) noexcept {
        auto& self = *static_cast<Impl*>(user);
        if (destination == nullptr || self.content_handle_ == nullptr ||
            offset > self.content_size_ || byte_count > self.content_size_ - offset)
            return false;
        try {
            const std::scoped_lock lock(self.content_mutex_);
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset);
            if (SetFilePointerEx(self.content_handle_, position, nullptr, FILE_BEGIN) == FALSE)
                return false;
            auto* cursor = static_cast<std::byte*>(destination);
            auto remaining = byte_count;
            while (remaining != 0u) {
                const auto chunk = static_cast<DWORD>(
                    std::min<std::uint64_t>(remaining, std::numeric_limits<DWORD>::max()));
                DWORD read = 0u;
                if (ReadFile(self.content_handle_, cursor, chunk, &read, nullptr) == FALSE ||
                    read != chunk)
                    return false;
                cursor += read;
                remaining -= read;
            }
            return 1u;
        } catch (...) {
            return 0u;
        }
    }

    [[nodiscard]] static NativePortMovieFailure
    map_codec_failure(const NativePortCodecFailure failure) noexcept {
        switch (failure) {
        case NativePortCodecFailure::UnsupportedContainer:
        case NativePortCodecFailure::UnsupportedCodec:
            return NativePortMovieFailure::DecoderUnavailable;
        case NativePortCodecFailure::InvalidContract:
            return NativePortMovieFailure::DecoderUnavailable;
        case NativePortCodecFailure::None:
        case NativePortCodecFailure::InvalidData:
        case NativePortCodecFailure::ContentRead:
        case NativePortCodecFailure::ResourceExhausted:
        case NativePortCodecFailure::Internal:
            return NativePortMovieFailure::CodecProviderFailure;
        }
        return NativePortMovieFailure::CodecProviderFailure;
    }

    void create_audio_stream(const NativePortAudioFormat format) {
        try {
            audio_ = std::make_unique<NativePortAudioStream>(
                NativePortAudioConfig{format, config_.maximum_audio_queue_frames});
        } catch (...) {
            transition(NativePortMovieState::Failed, NativePortMovieFailure::HostAudioFailure);
            throw NativePortMovieError(NativePortMovieFailure::HostAudioFailure, 0u, "audio-open");
        }
    }

    void update_playback_position(const std::uint64_t host_time) noexcept {
        const auto wall_position = host_time - anchor_host_time_;
        if (audio_ == nullptr || !audio_clock_started_) {
            position_ = wall_position;
            return;
        }

        const auto audio_snapshot = audio_->snapshot();
        auto audio_position = audio_clock_origin_timestamp_;
        saturating_add_counter(
            audio_position,
            audio_frames_to_nanoseconds(audio_->playback_position_frames(),
                                        provider_audio_format_.sample_rate));
        if (audio_eos_ && audio_snapshot.queued_frames == 0u) {
            if (!audio_tail_clock_started_) {
                audio_tail_clock_started_ = true;
                audio_tail_anchor_host_time_ = host_time;
                audio_tail_anchor_position_ = std::max(position_, audio_position);
            }
            auto tail_position = audio_tail_anchor_position_;
            saturating_add_counter(tail_position,
                                   host_time - audio_tail_anchor_host_time_);
            position_ = tail_position;
            return;
        }

        // Audio is the master while it is live. A host stall may therefore
        // delay both streams, but can never let video run ahead and leave
        // already-late PCM queued behind it.
        position_ = std::max(position_, audio_position);
    }

    void initialize_codec_provider(const NativePortCodecProvider& provider) {
        if (!valid_native_port_codec_provider(provider))
            return fail_and_throw(NativePortMovieFailure::DecoderUnavailable,
                                  "codec-provider-contract");
        const NativePortCodecOpenRequest request{{this, content_size_, &Impl::provider_read_at},
                                                 config_.audio_sample_rate,
                                                 config_.maximum_audio_queue_frames,
                                                 config_.maximum_video_queue_frames,
                                                 config_.maximum_video_frame_bytes,
                                                 config_.maximum_video_queue_bytes,
                                                 config_.require_audio ? 1u : 0u,
                                                 config_.require_video ? 1u : 0u};
        NativePortCodecOpenResult result;
        provider.open(provider.user, &request, &result);
        if (result.has_audio > 1u || result.has_video > 1u) {
            if (result.decoder != nullptr) provider.close(result.decoder);
            transition(NativePortMovieState::Failed,
                       NativePortMovieFailure::CodecProviderFailure,
                       result.provider_error_code);
            throw NativePortMovieError(
                failure_, result.provider_error_code, "codec-provider-open-flags");
        }
        if (result.failure != NativePortCodecFailure::None || result.decoder == nullptr) {
            if (result.decoder != nullptr) provider.close(result.decoder);
            transition(NativePortMovieState::Failed,
                       map_codec_failure(result.failure),
                       result.provider_error_code);
            throw NativePortMovieError(failure_, result.provider_error_code, "codec-provider-open");
        }
        if ((config_.require_audio && !result.has_audio) ||
            (config_.require_video && !result.has_video)) {
            provider.close(result.decoder);
            return fail_and_throw(NativePortMovieFailure::MissingRequiredStream,
                                  "codec-provider-streams");
        }
        codec_provider_ = &provider;
        codec_decoder_ = result.decoder;
        has_audio_ = result.has_audio;
        has_video_ = result.has_video;
        duration_ = result.duration_nanoseconds;
        provider_audio_format_ = {result.audio_sample_rate,
                                  static_cast<std::uint16_t>(result.audio_channels)};
        if (has_audio_) {
            if (result.audio_channels > std::numeric_limits<std::uint16_t>::max() ||
                provider_audio_format_.sample_rate < 8'000u ||
                provider_audio_format_.sample_rate > 192'000u ||
                (provider_audio_format_.channels != 1u && provider_audio_format_.channels != 2u))
                return fail_and_throw(NativePortMovieFailure::InvalidAudioBuffer,
                                      "codec-provider-audio-format");
            create_audio_stream(provider_audio_format_);
        }
    }

    void initialize_backend() {
        const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(com_result))
            com_owned_ = true;
        else if (com_result != RPC_E_CHANGED_MODE)
            require_hresult(com_result, "com-initialize");
        require_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL), "mf-startup");
        mf_started_ = true;
        ComPtr<IMFAttributes> attributes;
        require_hresult(MFCreateAttributes(&attributes, 3u), "attributes");
        require_hresult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE),
                        "hardware-transforms");
        require_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE),
                        "video-processing");
        const NativePortCodecByteSource source{this, content_size_, &Impl::provider_read_at};
        source_stream_.Attach(new ReadOnlyByteStream(source));
        require_hresult(MFCreateMFByteStreamOnStreamEx(source_stream_.Get(), &byte_stream_),
                        "byte-stream");
        require_hresult(
            MFCreateSourceReaderFromByteStream(byte_stream_.Get(), attributes.Get(), &reader_),
            "reader-open");
        require_hresult(
            reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE),
            "streams-clear");

        for (DWORD stream = 0u; stream < 128u; ++stream) {
            ComPtr<IMFMediaType> native_type;
            const auto result = reader_->GetNativeMediaType(stream, 0u, &native_type);
            if (result == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(result)) continue;
            GUID major{};
            if (FAILED(native_type->GetGUID(MF_MT_MAJOR_TYPE, &major))) continue;
            if (!has_audio_ && major == MFMediaType_Audio) {
                has_audio_ = true;
                audio_stream_ = stream;
            } else if (!has_video_ && major == MFMediaType_Video) {
                has_video_ = true;
                video_stream_ = stream;
            }
        }
        if ((config_.require_audio && !has_audio_) || (config_.require_video && !has_video_))
            return fail_and_throw(NativePortMovieFailure::MissingRequiredStream,
                                  "required-stream-missing");

        if (has_audio_) configure_audio_stream();
        if (has_video_) configure_video_stream();
        PROPVARIANT duration;
        PropVariantInit(&duration);
        const auto duration_result = reader_->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration);
        if (SUCCEEDED(duration_result) && duration.vt == VT_UI8)
            duration_ = hundred_ns_to_ns(static_cast<LONGLONG>(duration.uhVal.QuadPart));
        PropVariantClear(&duration);
    }

    void configure_audio_stream() {
        require_hresult(reader_->SetStreamSelection(audio_stream_, TRUE), "audio-select");
        ComPtr<IMFMediaType> type;
        require_hresult(MFCreateMediaType(&type), "audio-type");
        require_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "audio-major");
        require_hresult(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "audio-subtype");
        require_hresult(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16u), "audio-bits");
        require_hresult(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2u), "audio-channels");
        require_hresult(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, config_.audio_sample_rate),
                        "audio-rate");
        require_hresult(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4u), "audio-alignment");
        require_hresult(
            type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, config_.audio_sample_rate * 4u),
            "audio-byte-rate");
        require_hresult(reader_->SetCurrentMediaType(audio_stream_, nullptr, type.Get()),
                        "audio-decoder");
        create_audio_stream({config_.audio_sample_rate, 2u});
    }

    void configure_video_stream() {
        require_hresult(reader_->SetStreamSelection(video_stream_, TRUE), "video-select");
        ComPtr<IMFMediaType> type;
        require_hresult(MFCreateMediaType(&type), "video-type");
        require_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "video-major");
        require_hresult(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "video-subtype");
        require_hresult(reader_->SetCurrentMediaType(video_stream_, nullptr, type.Get()),
                        "video-decoder");
        refresh_video_format();
    }

    void refresh_video_format() {
        ComPtr<IMFMediaType> type;
        require_hresult(reader_->GetCurrentMediaType(video_stream_, &type), "video-current-type");
        require_hresult(
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &video_width_, &video_height_),
            "video-frame-size");
        if (video_width_ == 0u || video_height_ == 0u ||
            video_width_ > std::numeric_limits<std::uint32_t>::max() / 4u)
            return fail_and_throw(NativePortMovieFailure::InvalidVideoBuffer, "video-layout");
        const auto signed_stride = static_cast<std::int32_t>(
            MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, video_width_ * 4u));
        const auto absolute_stride =
            signed_stride < 0 ? -static_cast<std::int64_t>(signed_stride) : signed_stride;
        if (absolute_stride < static_cast<std::int64_t>(video_width_) * 4)
            return fail_and_throw(NativePortMovieFailure::InvalidVideoBuffer, "video-layout");
        video_stride_ = static_cast<std::uint32_t>(absolute_stride);
        video_bottom_up_ = signed_stride > 0;
        UINT32 pixel_numerator = 1u;
        UINT32 pixel_denominator = 1u;
        if (FAILED(MFGetAttributeRatio(type.Get(),
                                       MF_MT_PIXEL_ASPECT_RATIO,
                                       &pixel_numerator,
                                       &pixel_denominator)) ||
            pixel_numerator == 0u || pixel_denominator == 0u) {
            pixel_numerator = 1u;
            pixel_denominator = 1u;
        }
        const auto display = reduced_display_aspect(
            video_width_, video_height_, pixel_numerator, pixel_denominator);
        if (!valid_display_aspect(display.numerator, display.denominator))
            return fail_and_throw(NativePortMovieFailure::InvalidVideoBuffer,
                                  "video-display-aspect");
        video_display_aspect_numerator_ = display.numerator;
        video_display_aspect_denominator_ = display.denominator;
    }

    void decode_until_position() {
        while (!video_queue_.empty() &&
               hundred_ns_to_ns(video_queue_.front().timestamp) <= position_) {
            auto sample = std::move(video_queue_.front());
            video_queue_.pop_front();
            release_mf_video_budget(sample);
            present_video(sample);
            if (state_ == NativePortMovieState::Stopped) return;
        }
        constexpr std::uint64_t audio_lead_nanoseconds = 100'000'000u;
        const auto decode_horizon =
            position_ > std::numeric_limits<std::uint64_t>::max() - audio_lead_nanoseconds
                ? std::numeric_limits<std::uint64_t>::max()
                : position_ + audio_lead_nanoseconds;
        constexpr std::size_t maximum_samples_per_pump = 4096u;
        for (std::size_t work = 0u; work < maximum_samples_per_pump; ++work) {
            if (!pending_) read_pending();
            if (!pending_) break;
            const auto timestamp = hundred_ns_to_ns(pending_->timestamp);
            observe_flags(*pending_);
            if (has_audio_ && pending_->sample && pending_->stream == audio_stream_) {
                if (timestamp > decode_horizon) break;
                if (!submit_audio(*pending_)) break;
            } else if (has_video_ && pending_->sample && pending_->stream == video_stream_) {
                if (timestamp > decode_horizon) break;
                if (timestamp <= position_) {
                    release_mf_video_budget(*pending_);
                    present_video(*pending_);
                } else if (video_queue_.size() < config_.maximum_video_queue_frames)
                    video_queue_.push_back(std::move(*pending_));
                else
                    return;
            }
            if (state_ == NativePortMovieState::Stopped) return;
            pending_.reset();
        }
    }

    [[nodiscard]] bool submit_provider_audio_batch(const bool force) {
        if (provider_audio_batch_.empty()) return true;
        const auto frames = provider_audio_batch_.size() /
                            provider_audio_format_.channels;
        if (!force && frames < movie_audio_batch_frames) return true;
        if (!audio_->submit_pcm_s16(provider_audio_batch_)) return false;
        if (!audio_clock_started_) {
            audio_clock_started_ = true;
            audio_clock_origin_timestamp_ = provider_audio_batch_timestamp_;
        }
        provider_audio_batch_.clear();
        return true;
    }

    void append_provider_audio_sample(const ProviderSample& sample) {
        const auto frames = sample.audio_samples.size() /
                            sample.audio_format.channels;
        std::size_t silence_samples = 0u;
        auto append_timestamp = sample.timestamp;
        if (provider_audio_timeline_initialized_) {
            append_timestamp = provider_next_audio_timestamp_;
            if (provider_next_audio_timestamp_ > sample.timestamp &&
                provider_next_audio_timestamp_ - sample.timestamp >
                    movie_audio_timestamp_tolerance_nanoseconds)
                return fail_and_throw(NativePortMovieFailure::CodecProviderFailure,
                                      "codec-provider-audio-overlap");
            if (sample.timestamp > provider_next_audio_timestamp_ &&
                sample.timestamp - provider_next_audio_timestamp_ >
                    movie_audio_timestamp_tolerance_nanoseconds) {
                const auto gap_nanoseconds =
                    sample.timestamp - provider_next_audio_timestamp_;
                if (gap_nanoseconds > 1'000'000'000u)
                    return fail_and_throw(NativePortMovieFailure::CodecProviderFailure,
                                          "codec-provider-audio-gap");
                const auto gap_frames =
                    (gap_nanoseconds * sample.audio_format.sample_rate +
                     500'000'000u) /
                    1'000'000'000u;
                if (gap_frames > config_.maximum_audio_queue_frames)
                    return fail_and_throw(NativePortMovieFailure::InvalidAudioBuffer,
                                          "codec-provider-audio-gap-budget");
                silence_samples = static_cast<std::size_t>(gap_frames) *
                                  sample.audio_format.channels;
            }
        } else {
            provider_audio_timeline_initialized_ = true;
        }
        if (provider_audio_batch_.empty())
            provider_audio_batch_timestamp_ = append_timestamp;
        const auto maximum_batch_samples =
            static_cast<std::size_t>(config_.maximum_audio_queue_frames) *
            sample.audio_format.channels;
        if (provider_audio_batch_.size() > maximum_batch_samples ||
            silence_samples > maximum_batch_samples - provider_audio_batch_.size() ||
            sample.audio_samples.size() > maximum_batch_samples -
                                                   provider_audio_batch_.size() -
                                                   silence_samples)
            return fail_and_throw(NativePortMovieFailure::InvalidAudioBuffer,
                                  "codec-provider-audio-batch-budget");
        provider_audio_batch_.insert(provider_audio_batch_.end(),
                                     silence_samples,
                                     std::int16_t{0});
        provider_audio_batch_.insert(provider_audio_batch_.end(),
                                     sample.audio_samples.begin(),
                                     sample.audio_samples.end());
        provider_next_audio_timestamp_ = sample.timestamp;
        saturating_add_counter(
            provider_next_audio_timestamp_,
            audio_frames_to_nanoseconds(frames, sample.audio_format.sample_rate));
        saturating_add_counter(decoded_audio_frames_, frames);
    }

    void decode_provider_until_position() {
        if (!submit_provider_audio_batch(false)) return;
        while (!provider_video_queue_.empty() &&
               provider_video_queue_.front().timestamp <= position_) {
            auto sample = std::move(provider_video_queue_.front());
            provider_video_queue_bytes_ -= sample.video_pixels.size();
            provider_video_queue_.pop_front();
            present_provider_video(sample);
            if (state_ == NativePortMovieState::Stopped) return;
        }
        constexpr std::uint64_t audio_lead_nanoseconds = 100'000'000u;
        const auto decode_horizon =
            position_ > std::numeric_limits<std::uint64_t>::max() - audio_lead_nanoseconds
                ? std::numeric_limits<std::uint64_t>::max()
                : position_ + audio_lead_nanoseconds;
        constexpr std::size_t maximum_samples_per_pump = 4096u;
        for (std::size_t work = 0u; work < maximum_samples_per_pump; ++work) {
            if (provider_terminal_) break;
            if (!provider_pending_) read_provider_pending();
            if (!provider_pending_) {
                if (provider_terminal_) {
                    if (!submit_provider_audio_batch(true)) return;
                    audio_eos_ = true;
                    video_eos_ = true;
                }
                break;
            }
            auto& sample = *provider_pending_;
            switch (sample.kind) {
            case NativePortCodecSampleKind::AudioEnd:
                if (!submit_provider_audio_batch(true)) return;
                audio_eos_ = true;
                provider_pending_.reset();
                continue;
            case NativePortCodecSampleKind::VideoEnd:
                video_eos_ = true;
                provider_pending_.reset();
                continue;
            case NativePortCodecSampleKind::Audio:
                if (!provider_audio_timeline_initialized_ &&
                    sample.timestamp > position_)
                    return;
                if (sample.timestamp > decode_horizon) return;
                append_provider_audio_sample(sample);
                provider_pending_.reset();
                if (!submit_provider_audio_batch(false)) return;
                continue;
            case NativePortCodecSampleKind::Video:
                if (sample.timestamp > decode_horizon) return;
                if (sample.timestamp <= position_)
                    present_provider_video(sample);
                else if (provider_video_queue_.size() < config_.maximum_video_queue_frames &&
                         sample.video_pixels.size() <=
                             config_.maximum_video_queue_bytes - provider_video_queue_bytes_) {
                    provider_video_queue_bytes_ += sample.video_pixels.size();
                    provider_video_queue_.push_back(std::move(sample));
                } else
                    return;
                provider_pending_.reset();
                if (state_ == NativePortMovieState::Stopped) return;
                continue;
            }
        }
    }

    void read_provider_pending() {
        NativePortCodecReadResult result;
        codec_provider_->read_next(codec_decoder_, &result);
        if (result.status != NativePortCodecReadStatus::Sample &&
            result.status != NativePortCodecReadStatus::EndOfStream &&
            result.status != NativePortCodecReadStatus::Failure)
            return fail_and_throw(NativePortMovieFailure::CodecProviderFailure,
                                  "codec-provider-read-status");
        if (result.status == NativePortCodecReadStatus::Failure ||
            result.failure != NativePortCodecFailure::None) {
            transition(NativePortMovieState::Failed,
                       map_codec_failure(result.failure),
                       result.provider_error_code);
            throw NativePortMovieError(failure_, result.provider_error_code, "codec-provider-read");
        }
        if (result.status == NativePortCodecReadStatus::EndOfStream) {
            audio_eos_ = true;
            video_eos_ = true;
            provider_terminal_ = true;
            return;
        }
        const auto& source = result.sample;
        ProviderSample sample;
        sample.kind = source.kind;
        sample.timestamp = source.timestamp_nanoseconds;
        sample.duration = source.duration_nanoseconds;
        const bool audio_kind = source.kind == NativePortCodecSampleKind::Audio ||
                                source.kind == NativePortCodecSampleKind::AudioEnd;
        const bool video_kind = source.kind == NativePortCodecSampleKind::Video ||
                                source.kind == NativePortCodecSampleKind::VideoEnd;
        if ((audio_kind && audio_eos_) || (video_kind && video_eos_))
            return fail_and_throw(NativePortMovieFailure::CodecProviderFailure,
                                  "codec-provider-sample-after-eos");
        if ((source.kind == NativePortCodecSampleKind::Audio ||
             source.kind == NativePortCodecSampleKind::Video) &&
            provider_timestamp_initialized_ &&
            source.timestamp_nanoseconds < last_provider_timestamp_)
            return fail_and_throw(NativePortMovieFailure::CodecProviderFailure,
                                  "codec-provider-timestamp-regression");
        if (source.kind == NativePortCodecSampleKind::Audio ||
            source.kind == NativePortCodecSampleKind::Video) {
            provider_timestamp_initialized_ = true;
            last_provider_timestamp_ = source.timestamp_nanoseconds;
        }
        switch (source.kind) {
        case NativePortCodecSampleKind::AudioEnd:
            if (!has_audio_)
                return fail_and_throw(NativePortMovieFailure::CodecProviderFailure,
                                      "codec-provider-unadvertised-audio-eos");
            break;
        case NativePortCodecSampleKind::VideoEnd:
            if (!has_video_)
                return fail_and_throw(NativePortMovieFailure::CodecProviderFailure,
                                      "codec-provider-unadvertised-video-eos");
            break;
        case NativePortCodecSampleKind::Audio:
            if (!has_audio_ || source.audio_samples == nullptr || source.audio_sample_count == 0u ||
                source.audio_sample_rate != provider_audio_format_.sample_rate ||
                source.audio_channels != provider_audio_format_.channels ||
                source.audio_sample_count % source.audio_channels != 0u ||
                source.audio_sample_count >
                    static_cast<std::uint64_t>(config_.maximum_audio_queue_frames) *
                        source.audio_channels ||
                source.audio_sample_count > std::numeric_limits<std::size_t>::max())
                return fail_and_throw(NativePortMovieFailure::InvalidAudioBuffer,
                                      "codec-provider-audio-sample");
            sample.audio_format = {source.audio_sample_rate,
                                   static_cast<std::uint16_t>(source.audio_channels)};
            sample.audio_samples.assign(source.audio_samples,
                                        source.audio_samples +
                                            static_cast<std::size_t>(source.audio_sample_count));
            break;
        case NativePortCodecSampleKind::Video: {
            const auto required =
                static_cast<std::uint64_t>(source.video_stride_bytes) * source.video_height;
            if (!has_video_ || source.video_pixels == nullptr || source.video_width == 0u ||
                source.video_height == 0u || source.video_bottom_up > 1u ||
                !valid_display_aspect(
                    source.video_display_aspect_numerator,
                    source.video_display_aspect_denominator) ||
                static_cast<std::uint64_t>(source.video_stride_bytes) <
                    static_cast<std::uint64_t>(source.video_width) * 4u ||
                required == 0u || required > source.video_byte_count ||
                required > std::numeric_limits<std::size_t>::max() ||
                required > config_.maximum_video_frame_bytes ||
                required > config_.maximum_video_queue_bytes - provider_video_queue_bytes_)
                return fail_and_throw(NativePortMovieFailure::InvalidVideoBuffer,
                                      "codec-provider-video-sample");
            sample.video_width = source.video_width;
            sample.video_height = source.video_height;
            sample.video_stride = source.video_stride_bytes;
            sample.video_display_aspect_numerator =
                source.video_display_aspect_numerator;
            sample.video_display_aspect_denominator =
                source.video_display_aspect_denominator;
            sample.video_bottom_up = source.video_bottom_up != 0u;
            const auto* pixels = static_cast<const std::byte*>(source.video_pixels);
            sample.video_pixels.assign(pixels, pixels + static_cast<std::size_t>(required));
            break;
        }
        default:
            return fail_and_throw(NativePortMovieFailure::CodecProviderFailure,
                                  "codec-provider-sample-kind");
        }
        provider_pending_ = std::move(sample);
    }

    void present_provider_video(const ProviderSample& sample) {
        saturating_increment_counter(decoded_video_frames_);
        if (callbacks_.video == nullptr) return;
        const auto display_numerator =
            config_.video_display_aspect_numerator != 0u
                ? config_.video_display_aspect_numerator
                : sample.video_display_aspect_numerator;
        const auto display_denominator =
            config_.video_display_aspect_denominator != 0u
                ? config_.video_display_aspect_denominator
                : sample.video_display_aspect_denominator;
        const NativePortMovieVideoFrame frame{sample.timestamp,
                                              sample.duration,
                                              sample.video_width,
                                              sample.video_height,
                                              sample.video_stride,
                                              sample.video_bottom_up,
                                              sample.video_pixels,
                                              display_numerator,
                                              display_denominator};
        callback_active_ = true;
        callbacks_.video(callbacks_.user, frame);
        callback_active_ = false;
        saturating_increment_counter(presented_video_frames_);
        apply_deferred_stop();
    }

    void read_pending() {
        PendingSample next;
        IMFSample* sample = nullptr;
        const auto result = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM),
                                                0u,
                                                &next.stream,
                                                &next.flags,
                                                &next.timestamp,
                                                &sample);
        if (FAILED(result)) {
            platform_error_code_ = static_cast<std::uint32_t>(result);
            require_hresult(result, "decode");
        }
        next.sample.Attach(sample);
        if (has_video_ && (next.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0u &&
            next.stream == video_stream_)
            refresh_video_format();
        if (has_video_ && next.stream == video_stream_) {
            next.video_width = video_width_;
            next.video_height = video_height_;
            next.video_stride = video_stride_;
            next.video_display_aspect_numerator =
                video_display_aspect_numerator_;
            next.video_display_aspect_denominator =
                video_display_aspect_denominator_;
            next.video_bottom_up = video_bottom_up_;
            if (next.sample) {
                const auto required =
                    static_cast<std::uint64_t>(next.video_stride) * next.video_height;
                DWORD available = 0u;
                require_hresult(next.sample->GetTotalLength(&available), "video-total-length");
                if (required == 0u || required > available ||
                    required > config_.maximum_video_frame_bytes ||
                    required > config_.maximum_video_queue_bytes - mf_video_queue_bytes_)
                    return fail_and_throw(NativePortMovieFailure::InvalidVideoBuffer,
                                          "video-queue-budget");
                next.video_storage_bytes = required;
                mf_video_queue_bytes_ += required;
            }
        }
        if (!next.sample) {
            observe_flags(next);
            if ((next.flags & MF_SOURCE_READERF_ENDOFSTREAM) == 0u) return;
        }
        pending_ = std::move(next);
    }

    void release_mf_video_budget(PendingSample& sample) noexcept {
        if (sample.video_storage_bytes == 0u) return;
        mf_video_queue_bytes_ = sample.video_storage_bytes <= mf_video_queue_bytes_
                                    ? mf_video_queue_bytes_ - sample.video_storage_bytes
                                    : 0u;
        sample.video_storage_bytes = 0u;
    }

    [[nodiscard]] bool submit_audio(const PendingSample& pending) {
        ComPtr<IMFMediaBuffer> buffer;
        require_hresult(pending.sample->ConvertToContiguousBuffer(&buffer), "audio-buffer");
        BYTE* data = nullptr;
        DWORD bytes = 0u;
        require_hresult(buffer->Lock(&data, nullptr, &bytes), "audio-lock");
        bool submitted = false;
        try {
            if ((bytes & 3u) != 0u)
                fail_and_throw(NativePortMovieFailure::InvalidAudioBuffer, "audio-layout");
            const auto samples = std::span<const std::int16_t>(
                reinterpret_cast<const std::int16_t*>(data), bytes / 2u);
            submitted = audio_->submit_pcm_s16(samples);
            if (submitted) saturating_add_counter(decoded_audio_frames_, samples.size() / 2u);
        } catch (...) {
            static_cast<void>(buffer->Unlock());
            throw;
        }
        require_hresult(buffer->Unlock(), "audio-unlock");
        return submitted;
    }

    void present_video(const PendingSample& pending) {
        ComPtr<IMFMediaBuffer> buffer;
        require_hresult(pending.sample->ConvertToContiguousBuffer(&buffer), "video-buffer");
        BYTE* data = nullptr;
        DWORD bytes = 0u;
        require_hresult(buffer->Lock(&data, nullptr, &bytes), "video-lock");
        std::vector<std::byte> pixels;
        LONGLONG duration = 0;
        try {
            const auto required =
                static_cast<std::uint64_t>(pending.video_stride) * pending.video_height;
            if (required > bytes || required > config_.maximum_video_frame_bytes)
                return fail_and_throw(NativePortMovieFailure::InvalidVideoBuffer,
                                      "video-buffer-size");
            pixels.assign(reinterpret_cast<const std::byte*>(data),
                          reinterpret_cast<const std::byte*>(data) +
                              static_cast<std::size_t>(required));
            static_cast<void>(pending.sample->GetSampleDuration(&duration));
        } catch (...) {
            static_cast<void>(buffer->Unlock());
            throw;
        }
        require_hresult(buffer->Unlock(), "video-unlock");
        saturating_increment_counter(decoded_video_frames_);
        if (callbacks_.video != nullptr) {
            const auto display_numerator =
                config_.video_display_aspect_numerator != 0u
                    ? config_.video_display_aspect_numerator
                    : pending.video_display_aspect_numerator;
            const auto display_denominator =
                config_.video_display_aspect_denominator != 0u
                    ? config_.video_display_aspect_denominator
                    : pending.video_display_aspect_denominator;
            const NativePortMovieVideoFrame frame{hundred_ns_to_ns(pending.timestamp),
                                                  hundred_ns_to_ns(duration),
                                                  pending.video_width,
                                                  pending.video_height,
                                                  pending.video_stride,
                                                  pending.video_bottom_up,
                                                  pixels,
                                                  display_numerator,
                                                  display_denominator};
            callback_active_ = true;
            callbacks_.video(callbacks_.user, frame);
            callback_active_ = false;
            saturating_increment_counter(presented_video_frames_);
            apply_deferred_stop();
        }
    }

    void observe_flags(const PendingSample& sample) {
        if ((sample.flags & MF_SOURCE_READERF_ENDOFSTREAM) == 0u) return;
        if (has_audio_ && sample.stream == audio_stream_ && !audio_eos_) {
            audio_eos_ = true;
            require_hresult(reader_->SetStreamSelection(audio_stream_, FALSE),
                            "audio-eos-deselect");
        }
        if (has_video_ && sample.stream == video_stream_ && !video_eos_) {
            video_eos_ = true;
            require_hresult(reader_->SetStreamSelection(video_stream_, FALSE),
                            "video-eos-deselect");
        }
    }
#endif

    void complete_if_drained() {
#ifdef _WIN32
        const bool streams_complete = (!has_audio_ || audio_eos_) && (!has_video_ || video_eos_);
        const bool queued_samples =
            codec_provider_ != nullptr
                ? provider_pending_.has_value() || !provider_video_queue_.empty()
                : pending_.has_value() || !video_queue_.empty();
        if (!streams_complete || queued_samples) return;
        if (audio_) {
            audio_->poll();
            if (audio_->snapshot().queued_frames != 0u) return;
        }
        if (duration_ != 0u) position_ = std::max(position_, duration_);
        transition(NativePortMovieState::Completed);
#endif
    }

    void bind_or_require_owner_thread() {
        const auto current = std::this_thread::get_id();
        if (!owner_thread_bound_) {
            owner_thread_ = current;
            owner_thread_bound_ = true;
            return;
        }
        require_owner_thread();
    }

    void require_owner_thread() const {
        if (owner_thread_bound_ && std::this_thread::get_id() != owner_thread_)
            throw NativePortMovieError(
                NativePortMovieFailure::ThreadViolation, 0u, "thread-violation");
    }

    void require_not_callback() const {
        if (callback_active_)
            throw NativePortMovieError(
                NativePortMovieFailure::CallbackReentry, 0u, "callback-reentry");
    }

    void reset_for_open() noexcept {
        duration_ = 0u;
        position_ = 0u;
        anchor_host_time_ = 0u;
        pause_host_time_ = 0u;
        last_host_time_ = 0u;
        audio_clock_origin_timestamp_ = 0u;
        audio_tail_anchor_host_time_ = 0u;
        audio_tail_anchor_position_ = 0u;
        decoded_audio_frames_ = 0u;
        decoded_video_frames_ = 0u;
        presented_video_frames_ = 0u;
        failure_ = NativePortMovieFailure::None;
        platform_error_code_ = 0u;
        deferred_stop_ = false;
        audio_clock_started_ = false;
        audio_tail_clock_started_ = false;
#ifdef _WIN32
        pending_.reset();
        video_queue_.clear();
        mf_video_queue_bytes_ = 0u;
        provider_pending_.reset();
        provider_video_queue_.clear();
        provider_video_queue_bytes_ = 0u;
        provider_audio_batch_.clear();
        provider_audio_batch_timestamp_ = 0u;
        provider_next_audio_timestamp_ = 0u;
        provider_audio_timeline_initialized_ = false;
        has_audio_ = false;
        has_video_ = false;
        audio_eos_ = false;
        video_eos_ = false;
        provider_terminal_ = false;
        provider_timestamp_initialized_ = false;
        last_provider_timestamp_ = 0u;
        audio_stream_ = 0u;
        video_stream_ = 0u;
        video_width_ = 0u;
        video_height_ = 0u;
        video_stride_ = 0u;
        video_display_aspect_numerator_ = 0u;
        video_display_aspect_denominator_ = 0u;
        video_bottom_up_ = false;
        provider_audio_format_ = {};
#endif
    }

    void apply_deferred_stop() {
        if (!deferred_stop_ || state_ == NativePortMovieState::Stopped) return;
        deferred_stop_ = false;
        close_backend(false);
        transition(NativePortMovieState::Stopped);
    }

    void close_backend(const bool reset_state = true) noexcept {
        if (audio_) {
            audio_.reset();
        }
#ifdef _WIN32
        if (codec_provider_ != nullptr && codec_decoder_ != nullptr)
            codec_provider_->close(codec_decoder_);
        codec_provider_ = nullptr;
        codec_decoder_ = nullptr;
        provider_pending_.reset();
        provider_video_queue_.clear();
        provider_video_queue_bytes_ = 0u;
        provider_audio_batch_.clear();
        pending_.reset();
        video_queue_.clear();
        mf_video_queue_bytes_ = 0u;
        reader_.Reset();
        byte_stream_.Reset();
        source_stream_.Reset();
        if (mf_started_) {
            static_cast<void>(MFShutdown());
            mf_started_ = false;
        }
        if (com_owned_) {
            CoUninitialize();
            com_owned_ = false;
        }
        if (content_handle_ != nullptr) {
            if (content_lock_held_) {
                static_cast<void>(UnlockFileEx(content_handle_,
                                               0u,
                                               std::numeric_limits<DWORD>::max(),
                                               std::numeric_limits<DWORD>::max(),
                                               &content_lock_));
                content_lock_held_ = false;
                content_lock_ = {};
            }
            static_cast<void>(CloseHandle(content_handle_));
            content_handle_ = nullptr;
        }
        content_size_ = 0u;
#endif
        if (reset_state) state_ = NativePortMovieState::Closed;
    }

    void transition(const NativePortMovieState state,
                    const NativePortMovieFailure failure = NativePortMovieFailure::None,
                    const std::uint32_t platform_error = 0u) noexcept {
        state_ = state;
        failure_ = failure;
        platform_error_code_ = platform_error;
        if (callbacks_.state != nullptr) {
            callback_active_ = true;
            callbacks_.state(callbacks_.user, state_, failure_, platform_error_code_);
            callback_active_ = false;
        }
    }

    [[noreturn]] void fail_and_throw(const NativePortMovieFailure failure, const char* operation) {
        transition(NativePortMovieState::Failed, failure);
        throw NativePortMovieError(failure, platform_error_code_, operation);
    }

    [[noreturn]] void fail_and_close(const NativePortMovieFailure failure, const char* operation) {
        transition(NativePortMovieState::Failed, failure);
        close_backend(false);
        throw NativePortMovieError(failure, platform_error_code_, operation);
    }

    NativePortMovieConfig config_{};
    std::string content_identity_;
    NativePortMovieCallbacks callbacks_{};
    std::filesystem::path path_;
    std::filesystem::path content_root_path_;
    std::unique_ptr<NativePortAudioStream> audio_;
    NativePortMovieState state_ = NativePortMovieState::Closed;
    std::uint64_t duration_ = 0u;
    std::uint64_t position_ = 0u;
    std::uint64_t anchor_host_time_ = 0u;
    std::uint64_t pause_host_time_ = 0u;
    std::uint64_t last_host_time_ = 0u;
    std::uint64_t audio_clock_origin_timestamp_ = 0u;
    std::uint64_t audio_tail_anchor_host_time_ = 0u;
    std::uint64_t audio_tail_anchor_position_ = 0u;
    std::uint64_t decoded_audio_frames_ = 0u;
    std::uint64_t decoded_video_frames_ = 0u;
    std::uint64_t presented_video_frames_ = 0u;
    NativePortMovieFailure failure_ = NativePortMovieFailure::None;
    std::uint32_t platform_error_code_ = 0u;
    std::thread::id owner_thread_{};
    bool owner_thread_bound_ = false;
    bool callback_active_ = false;
    bool deferred_stop_ = false;
    bool audio_clock_started_ = false;
    bool audio_tail_clock_started_ = false;
#ifdef _WIN32
    HANDLE content_handle_ = nullptr;
    OVERLAPPED content_lock_{};
    std::uint64_t content_size_ = 0u;
    std::mutex content_mutex_;
    const NativePortCodecProvider* codec_provider_ = nullptr;
    void* codec_decoder_ = nullptr;
    NativePortAudioFormat provider_audio_format_{};
    std::optional<ProviderSample> provider_pending_;
    std::deque<ProviderSample> provider_video_queue_;
    std::uint64_t provider_video_queue_bytes_ = 0u;
    std::vector<std::int16_t> provider_audio_batch_;
    std::uint64_t provider_audio_batch_timestamp_ = 0u;
    std::uint64_t provider_next_audio_timestamp_ = 0u;
    bool provider_terminal_ = false;
    bool provider_timestamp_initialized_ = false;
    bool provider_audio_timeline_initialized_ = false;
    std::uint64_t last_provider_timestamp_ = 0u;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFByteStream> byte_stream_;
    ComPtr<IStream> source_stream_;
    std::optional<PendingSample> pending_;
    std::deque<PendingSample> video_queue_;
    std::uint64_t mf_video_queue_bytes_ = 0u;
    std::uint32_t video_width_ = 0u;
    std::uint32_t video_height_ = 0u;
    std::uint32_t video_stride_ = 0u;
    std::uint32_t video_display_aspect_numerator_ = 0u;
    std::uint32_t video_display_aspect_denominator_ = 0u;
    bool video_bottom_up_ = false;
    DWORD audio_stream_ = 0u;
    DWORD video_stream_ = 0u;
    bool has_audio_ = false;
    bool has_video_ = false;
    bool audio_eos_ = false;
    bool video_eos_ = false;
    bool com_owned_ = false;
    bool mf_started_ = false;
    bool content_lock_held_ = false;
#endif
};

NativePortMovieSession::NativePortMovieSession() : impl_(std::make_unique<Impl>()) {}
NativePortMovieSession::~NativePortMovieSession() = default;

void NativePortMovieSession::open(const NativePortMovieConfig& config) {
    impl_->open(config);
}
void NativePortMovieSession::play(const std::uint64_t host_time) {
    impl_->play(host_time);
}
void NativePortMovieSession::pump(const std::uint64_t host_time) {
    impl_->pump(host_time);
}
void NativePortMovieSession::pause(const std::uint64_t host_time) {
    impl_->pause(host_time);
}
void NativePortMovieSession::stop() {
    impl_->stop();
}
NativePortMovieSnapshot NativePortMovieSession::snapshot() const noexcept {
    return impl_->snapshot();
}

} // namespace katana::runtime
