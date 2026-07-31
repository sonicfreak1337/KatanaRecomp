#include "katana/runtime/host_video.hpp"
#include "host_video_d3d11.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

enum class FrameDescriptionDetail : std::uint8_t {
    None,
    Basic,
    Detailed,
};

} // namespace
} // namespace katana::runtime

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <string_view>
#include <vector>

namespace katana::runtime {
namespace {

constexpr wchar_t window_class_name[] = L"KatanaRecompNativeVideoV1";

std::wstring utf8_to_wide(const std::string_view value) {
    if (value.empty()) return {};
    const auto size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::invalid_argument("Videofenstertitel ist kein gueltiges UTF-8.");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            size) != size) {
        throw std::runtime_error("Videofenstertitel konnte nicht konvertiert werden.");
    }
    return result;
}

class Win32VideoOutput final : public NativeVideoOutput {
  public:
    explicit Win32VideoOutput(const NativeVideoConfig& config) {
        if (config.contract_version != native_video_contract_version) {
            throw std::invalid_argument("Nicht unterstuetzte native Videovertragsversion.");
        }
        if (config.title.empty() || config.client_width == 0u || config.client_height == 0u ||
            config.client_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            config.client_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("Ungueltige native Videofensterkonfiguration.");
        }
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW existing{};
        if (GetClassInfoW(instance, window_class_name, &existing) == FALSE) {
            WNDCLASSW window_class{};
            window_class.lpfnWndProc = &Win32VideoOutput::window_proc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            window_class.lpszClassName = window_class_name;
            if (RegisterClassW(&window_class) == 0u &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                throw std::runtime_error(
                    "Native Videofensterklasse konnte nicht registriert werden.");
            }
        }
        RECT bounds{
            0, 0, static_cast<LONG>(config.client_width), static_cast<LONG>(config.client_height)};
        if (AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE) == FALSE)
            throw std::runtime_error("Native Videofenstergeometrie konnte nicht berechnet werden.");
        const auto title = utf8_to_wide(config.title);
        window_ = CreateWindowExW(0,
                                  window_class_name,
                                  title.c_str(),
                                  WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  bounds.right - bounds.left,
                                  bounds.bottom - bounds.top,
                                  nullptr,
                                  nullptr,
                                  instance,
                                  this);
        if (window_ == nullptr)
            throw std::runtime_error("Natives Videofenster konnte nicht erstellt werden.");
        client_width_ = config.client_width;
        client_height_ = config.client_height;
        d3d11_presenter_ = detail::try_create_win32_d3d11_presenter(
            window_, client_width_, client_height_);
        if (d3d11_presenter_)
            backend_ = NativeVideoBackend::Win32D3d11Hardware;
        else {
            backend_fallbacks_ = 1u;
            d3d11_recovery_pending_ = true;
            next_d3d11_recovery_ms_ =
                detail::win32_d3d11_deadline_after(
                    GetTickCount64(),
                    detail::win32_d3d11_recovery_delay_ms(0u));
        }
        if (config.initially_visible) show();
    }

    ~Win32VideoOutput() override {
        d3d11_presenter_.reset();
        if (window_ != nullptr) DestroyWindow(window_);
    }

    void show() override {
        if (window_ == nullptr) throw std::logic_error("Natives Videofenster ist geschlossen.");
        ShowWindow(window_, SW_SHOWNORMAL);
        // The first ShowWindow call may be overridden by STARTUPINFO when the
        // host was launched from a hidden build/test helper. A second call is
        // authoritative and keeps the public show() contract independent of
        // the parent process' console-window policy.
        if (IsWindowVisible(window_) == FALSE)
            ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
        visible_ = IsWindowVisible(window_) != FALSE;
    }

    void poll_events() override {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0u, 0u, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        throw_pending_paint_failure();
    }

    std::vector<NativeHostEvent> drain_events() override {
        auto result = std::move(events_);
        events_.clear();
        return result;
    }

    void resize(const std::uint32_t width, const std::uint32_t height) override {
        if (width == 0u || height == 0u ||
            width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("Ungueltige native Videofenstergroesse.");
        }
        RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        if (AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE) == FALSE ||
            SetWindowPos(window_,
                         nullptr,
                         0,
                         0,
                         bounds.right - bounds.left,
                         bounds.bottom - bounds.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) == FALSE) {
            throw std::runtime_error("Natives Videofenster konnte nicht skaliert werden.");
        }
        poll_events();
    }

    void present(const PvrFrame& frame) override {
        if (frame.width == 0u || frame.height == 0u ||
            static_cast<std::uint64_t>(frame.width) * frame.height >
                std::numeric_limits<std::size_t>::max() / 4u ||
            frame.rgba.size() != static_cast<std::size_t>(frame.width) * frame.height * 4u) {
            throw std::invalid_argument("Ungueltiger RGBA-Frame fuer native Videoausgabe.");
        }
        increment_counter(presentation_telemetry_.submitted_frames);
        frame_width_ = frame.width;
        frame_height_ = frame.height;
        try_recover_d3d11();
        if (d3d11_presenter_) {
            const auto result = d3d11_presenter_->present(
                frame.rgba, frame.width, frame.height, client_width_, client_height_);
            const auto decision =
                detail::classify_win32_d3d11_present_result(result);
            observe_presentation_outcome(decision.outcome);
            if (decision.outcome == NativeVideoPresentationOutcome::Presented)
                return;
            if (decision.retain_backend) return;
            abandon_d3d11();
        }
        if (!gdi_window_presentable()) {
            observe_presentation_outcome(
                NativeVideoPresentationOutcome::NotPresentable);
            return;
        }
        bgra_.resize(frame.rgba.size());
        for (std::size_t offset = 0u; offset < frame.rgba.size(); offset += 4u) {
            bgra_[offset] = frame.rgba[offset + 2u];
            bgra_[offset + 1u] = frame.rgba[offset + 1u];
            bgra_[offset + 2u] = frame.rgba[offset];
            bgra_[offset + 3u] = frame.rgba[offset + 3u];
        }
        if (submitted_frame_serial_ == std::numeric_limits<std::uint64_t>::max()) {
            submitted_frame_serial_ = 1u;
            painted_frame_serial_ = 0u;
        } else {
            ++submitted_frame_serial_;
        }
        if (InvalidateRect(window_, nullptr, FALSE) == FALSE) {
            record_paint_failure(PaintFailure::InvalidateRect);
            return;
        }
        UpdateWindow(window_);
    }

    void request_close() noexcept override {
        if (!close_requested_) push_event(NativeHostEventKind::Close);
        close_requested_ = true;
    }
    [[nodiscard]] bool close_requested() const noexcept override {
        return close_requested_;
    }
    [[nodiscard]] std::uint32_t client_width() const noexcept override {
        return client_width_;
    }
    [[nodiscard]] std::uint32_t client_height() const noexcept override {
        return client_height_;
    }
    [[nodiscard]] std::uint64_t presented_frames() const noexcept override {
        return presentation_telemetry_.presented_frames;
    }
    [[nodiscard]] NativeVideoBackend backend() const noexcept override {
        return backend_;
    }
    [[nodiscard]] bool hardware_accelerated() const noexcept override {
        return backend_ == NativeVideoBackend::Win32D3d11Hardware;
    }
    [[nodiscard]] std::uint64_t backend_fallbacks() const noexcept override {
        return backend_fallbacks_;
    }
    [[nodiscard]] NativeVideoPresentationTelemetry
    presentation_telemetry() const noexcept override {
        return presentation_telemetry_;
    }

  private:
    enum class PaintFailure : std::uint8_t {
        None,
        InvalidateRect,
        BeginPaint,
        FillRect,
        StretchDibitsGdiError,
        StretchDibitsNoScanlines,
        StretchDibitsPartialScanlines,
        EndPaint,
    };

    [[nodiscard]] static const char* paint_failure_name(const PaintFailure failure) noexcept {
        switch (failure) {
        case PaintFailure::None:
            return "none";
        case PaintFailure::InvalidateRect:
            return "invalidate-rect";
        case PaintFailure::BeginPaint:
            return "begin-paint";
        case PaintFailure::FillRect:
            return "fill-rect";
        case PaintFailure::StretchDibitsGdiError:
            return "stretch-dibits-gdi-error";
        case PaintFailure::StretchDibitsNoScanlines:
            return "stretch-dibits-zero-scanlines";
        case PaintFailure::StretchDibitsPartialScanlines:
            return "stretch-dibits-partial-scanlines";
        case PaintFailure::EndPaint:
            return "end-paint";
        }
        return "unknown";
    }

    static void increment_counter(std::uint64_t& value) noexcept {
        if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
    }

    void observe_presentation_outcome(
        const NativeVideoPresentationOutcome outcome) noexcept {
        presentation_telemetry_.last_outcome = outcome;
        presentation_telemetry_.currently_occluded =
            outcome == NativeVideoPresentationOutcome::Occluded;
        switch (outcome) {
        case NativeVideoPresentationOutcome::None:
            break;
        case NativeVideoPresentationOutcome::Presented:
            increment_counter(presentation_telemetry_.presented_frames);
            break;
        case NativeVideoPresentationOutcome::Occluded:
            increment_counter(presentation_telemetry_.occluded_frames);
            break;
        case NativeVideoPresentationOutcome::NotPresentable:
            increment_counter(
                presentation_telemetry_.not_presentable_frames);
            break;
        case NativeVideoPresentationOutcome::BackendFailure:
            increment_counter(presentation_telemetry_.backend_failures);
            break;
        }
    }

    void record_paint_failure(const PaintFailure failure) noexcept {
        if (paint_failure_ != PaintFailure::None) return;
        observe_presentation_outcome(
            NativeVideoPresentationOutcome::BackendFailure);
        paint_failure_ = failure;
        paint_failure_last_error_ = GetLastError();
    }

    void throw_pending_paint_failure() {
        if (paint_failure_ == PaintFailure::None) return;
        const auto failure = paint_failure_;
        const auto last_error = paint_failure_last_error_;
        paint_failure_ = PaintFailure::None;
        paint_failure_last_error_ = ERROR_SUCCESS;
        throw std::runtime_error(std::string("native-video-paint-") +
                                 paint_failure_name(failure) +
                                 " win32-error=" + std::to_string(last_error));
    }

    void abandon_d3d11() noexcept {
        if (!d3d11_presenter_) return;
        d3d11_presenter_.reset();
        presentation_telemetry_.currently_occluded = false;
        backend_ = NativeVideoBackend::Win32Gdi;
        d3d11_recovery_pending_ = true;
        d3d11_recovery_failed_attempts_ = 0u;
        next_d3d11_recovery_ms_ = detail::win32_d3d11_deadline_after(
            GetTickCount64(),
            detail::win32_d3d11_recovery_delay_ms(0u));
        if (backend_fallbacks_ != std::numeric_limits<std::uint64_t>::max())
            ++backend_fallbacks_;
    }

    void try_recover_d3d11() noexcept {
        if (!d3d11_recovery_pending_ || window_ == nullptr ||
            close_requested_ || !visible_ ||
            IsWindowVisible(window_) == FALSE ||
            IsIconic(window_) != FALSE)
            return;
        const auto now_ms = GetTickCount64();
        if (!detail::win32_d3d11_deadline_reached(
                now_ms, next_d3d11_recovery_ms_))
            return;
        RECT client{};
        if (GetClientRect(window_, &client) == FALSE ||
            client.right <= client.left || client.bottom <= client.top)
            return;
        client_width_ =
            static_cast<std::uint32_t>(client.right - client.left);
        client_height_ =
            static_cast<std::uint32_t>(client.bottom - client.top);
        auto recovered = detail::try_create_win32_d3d11_presenter(
            window_, client_width_, client_height_);
        if (recovered) {
            d3d11_presenter_ = std::move(recovered);
            backend_ = NativeVideoBackend::Win32D3d11Hardware;
            d3d11_recovery_pending_ = false;
            d3d11_recovery_failed_attempts_ = 0u;
            next_d3d11_recovery_ms_ = 0u;
            return;
        }
        ++d3d11_recovery_failed_attempts_;
        if (d3d11_recovery_failed_attempts_ >=
            detail::win32_d3d11_recovery_maximum_attempts) {
            d3d11_recovery_pending_ = false;
            return;
        }
        next_d3d11_recovery_ms_ = detail::win32_d3d11_deadline_after(
            now_ms,
            detail::win32_d3d11_recovery_delay_ms(
                d3d11_recovery_failed_attempts_));
    }

    [[nodiscard]] bool gdi_window_presentable() noexcept {
        if (window_ == nullptr || !visible_ ||
            IsWindowVisible(window_) == FALSE ||
            IsIconic(window_) != FALSE)
            return false;
        RECT client{};
        if (GetClientRect(window_, &client) == FALSE ||
            client.right <= client.left || client.bottom <= client.top)
            return false;
        client_width_ =
            static_cast<std::uint32_t>(client.right - client.left);
        client_height_ =
            static_cast<std::uint32_t>(client.bottom - client.top);
        return true;
    }

    static LRESULT CALLBACK window_proc(const HWND window,
                                        const UINT message,
                                        const WPARAM wparam,
                                        const LPARAM lparam) {
        auto* self = reinterpret_cast<Win32VideoOutput*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<Win32VideoOutput*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self == nullptr ? DefWindowProcW(window, message, wparam, lparam)
                               : self->handle_message(window, message, wparam, lparam);
    }

    LRESULT handle_message(const HWND window,
                           const UINT message,
                           const WPARAM wparam,
                           const LPARAM lparam) {
        if (message == WM_CLOSE) {
            close_requested_ = true;
            push_event(NativeHostEventKind::Close);
            return 0;
        }
        if (message == WM_SETFOCUS) {
            push_event(NativeHostEventKind::FocusGained);
            return 0;
        }
        if (message == WM_KILLFOCUS) {
            push_event(NativeHostEventKind::FocusLost);
            return 0;
        }
        if (message == WM_KEYDOWN || message == WM_KEYUP) {
            if ((lparam & (1ll << 30ll)) == 0ll || message == WM_KEYUP) {
                push_event(message == WM_KEYDOWN ? NativeHostEventKind::KeyDown
                                                 : NativeHostEventKind::KeyUp,
                           map_key(wparam));
            }
            return 0;
        }
        if (message == WM_SHOWWINDOW) {
            visible_ = wparam != FALSE;
            return DefWindowProcW(window, message, wparam, lparam);
        }
        if (message == WM_SIZE && wparam == SIZE_MINIMIZED) {
            client_width_ = 0u;
            client_height_ = 0u;
            return 0;
        }
        if (message == WM_SIZE) {
            client_width_ = static_cast<std::uint32_t>(LOWORD(lparam));
            client_height_ = static_cast<std::uint32_t>(HIWORD(lparam));
            if (d3d11_presenter_ &&
                !d3d11_presenter_->resize(client_width_, client_height_)) {
                observe_presentation_outcome(
                    NativeVideoPresentationOutcome::BackendFailure);
                abandon_d3d11();
            }
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            if (dc == nullptr) {
                record_paint_failure(PaintFailure::BeginPaint);
                return 0;
            }
            bool paint_succeeded = true;
            bool submitted_frame_drawn = false;
            const bool d3d11_frame_visible =
                d3d11_presenter_ &&
                presentation_telemetry_.presented_frames != 0u;
            RECT client{};
            GetClientRect(window, &client);
            if (!d3d11_frame_visible &&
                FillRect(dc,
                         &client,
                         static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))) == 0) {
                record_paint_failure(PaintFailure::FillRect);
                paint_succeeded = false;
            }
            if (!d3d11_presenter_ && gdi_window_presentable() &&
                !bgra_.empty() &&
                client_width_ != 0u && client_height_ != 0u) {
                const auto viewport = calculate_native_video_viewport(
                    frame_width_, frame_height_, client_width_, client_height_);
                const auto width = static_cast<int>(viewport.width);
                const auto height = static_cast<int>(viewport.height);
                const auto x = static_cast<int>(viewport.x);
                const auto y = static_cast<int>(viewport.y);
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth = static_cast<LONG>(frame_width_);
                bitmap.bmiHeader.biHeight = -static_cast<LONG>(frame_height_);
                bitmap.bmiHeader.biPlanes = 1u;
                bitmap.bmiHeader.biBitCount = 32u;
                bitmap.bmiHeader.biCompression = BI_RGB;
                SetLastError(ERROR_SUCCESS);
                const auto copied_scanlines = StretchDIBits(dc,
                                                            x,
                                                            y,
                                                            width,
                                                            height,
                                                            0,
                                                            0,
                                                            static_cast<int>(frame_width_),
                                                            static_cast<int>(frame_height_),
                                                            bgra_.data(),
                                                            &bitmap,
                                                            DIB_RGB_COLORS,
                                                            SRCCOPY);
                if (copied_scanlines == GDI_ERROR) {
                    record_paint_failure(PaintFailure::StretchDibitsGdiError);
                    paint_succeeded = false;
                } else if (copied_scanlines == 0) {
                    record_paint_failure(PaintFailure::StretchDibitsNoScanlines);
                    paint_succeeded = false;
                } else if (copied_scanlines != static_cast<int>(frame_height_)) {
                    record_paint_failure(PaintFailure::StretchDibitsPartialScanlines);
                    paint_succeeded = false;
                } else {
                    submitted_frame_drawn = true;
                }
            }
            if (EndPaint(window, &paint) == FALSE) {
                record_paint_failure(PaintFailure::EndPaint);
                paint_succeeded = false;
            }
            if (paint_succeeded && submitted_frame_drawn &&
                submitted_frame_serial_ != painted_frame_serial_) {
                painted_frame_serial_ = submitted_frame_serial_;
                observe_presentation_outcome(
                    NativeVideoPresentationOutcome::Presented);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static NativeHostKey map_key(const WPARAM key) noexcept {
        switch (key) {
        case VK_RETURN:
            return NativeHostKey::Start;
        case 'Z':
            return NativeHostKey::A;
        case 'X':
            return NativeHostKey::B;
        case 'A':
            return NativeHostKey::X;
        case 'S':
            return NativeHostKey::Y;
        case VK_UP:
            return NativeHostKey::Up;
        case VK_DOWN:
            return NativeHostKey::Down;
        case VK_LEFT:
            return NativeHostKey::Left;
        case VK_RIGHT:
            return NativeHostKey::Right;
        default:
            return NativeHostKey::Unknown;
        }
    }

    void push_event(const NativeHostEventKind kind,
                    const NativeHostKey key = NativeHostKey::Unknown) {
        events_.push_back({next_event_sequence_++, kind, key});
    }

    HWND window_ = nullptr;
    std::unique_ptr<detail::Win32D3d11Presenter> d3d11_presenter_;
    std::vector<std::uint8_t> bgra_;
    std::vector<NativeHostEvent> events_;
    std::uint32_t frame_width_ = 0u;
    std::uint32_t frame_height_ = 0u;
    std::uint32_t client_width_ = 0u;
    std::uint32_t client_height_ = 0u;
    std::uint64_t submitted_frame_serial_ = 0u;
    std::uint64_t painted_frame_serial_ = 0u;
    std::uint64_t backend_fallbacks_ = 0u;
    std::uint64_t next_event_sequence_ = 1u;
    std::uint64_t next_d3d11_recovery_ms_ = 0u;
    std::uint32_t d3d11_recovery_failed_attempts_ = 0u;
    PaintFailure paint_failure_ = PaintFailure::None;
    DWORD paint_failure_last_error_ = ERROR_SUCCESS;
    NativeVideoPresentationTelemetry presentation_telemetry_;
    NativeVideoBackend backend_ = NativeVideoBackend::Win32Gdi;
    bool d3d11_recovery_pending_ = false;
    bool visible_ = false;
    bool close_requested_ = false;
};

} // namespace

bool native_video_available() noexcept {
    return true;
}

std::unique_ptr<NativeVideoOutput> create_native_video_output(const NativeVideoConfig& config) {
    return std::make_unique<Win32VideoOutput>(config);
}

} // namespace katana::runtime

#else

namespace katana::runtime {

bool native_video_available() noexcept {
    return false;
}

std::unique_ptr<NativeVideoOutput> create_native_video_output(const NativeVideoConfig&) {
    throw std::runtime_error("Native Videoausgabe ist auf diesem Host nicht verfuegbar.");
}

} // namespace katana::runtime

#endif

namespace katana::runtime {

NativeVideoViewport
calculate_native_video_viewport(const std::uint32_t frame_width,
                                const std::uint32_t frame_height,
                                const std::uint32_t client_width,
                                const std::uint32_t client_height) noexcept {
    if (frame_width == 0u || frame_height == 0u ||
        client_width == 0u || client_height == 0u)
        return {};
    NativeVideoViewport result;
    if (static_cast<std::uint64_t>(client_width) * frame_height <=
        static_cast<std::uint64_t>(client_height) * frame_width) {
        result.width = client_width;
        result.height = std::max<std::uint32_t>(
            1u,
            static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(client_width) * frame_height /
                frame_width));
    } else {
        result.height = client_height;
        result.width = std::max<std::uint32_t>(
            1u,
            static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(client_height) * frame_width /
                frame_height));
    }
    result.x = (client_width - result.width) / 2u;
    result.y = (client_height - result.height) / 2u;
    return result;
}

bool present_guest_frame_proof(NativeVideoOutput& output, const PvrGuestFrameProof& proof) {
    const auto presented_before = output.presented_frames();
    output.present(proof.frame);
    return output.presented_frames() > presented_before;
}

GuestFrameEvidenceObservation
GuestFrameEvidenceTracker::observe(const GuestFramePumpResult& frame,
                                   const bool guest_program_progressed) noexcept {
    GuestFrameEvidenceObservation observation;
    if (!frame.guest_frame_proven || !frame.proof_source || frame.render_generation == 0u)
        return observation;
    if (*frame.proof_source == PvrGuestFrameProofSource::DirectFramebuffer &&
        (frame.write_generation_first == 0u ||
         frame.write_generation_last < frame.write_generation_first ||
         frame.render_generation != frame.write_generation_last))
        return observation;

    observation.proof_source = frame.proof_source;
    observation.render_generation = frame.render_generation;
    observation.write_generation_first = frame.write_generation_first;
    observation.write_generation_last = frame.write_generation_last;
    const auto add_marker = [&](const GuestFrameEvidenceMarker marker) {
        observation.markers |= guest_frame_evidence_marker(marker);
    };
    const bool prior_guest_scanout = guest_scanout_seen_;
    if (!guest_scanout_seen_) {
        guest_scanout_seen_ = true;
        add_marker(GuestFrameEvidenceMarker::FirstGuestScanout);
    }
    if (!guest_program_progressed) bootstrap_scanout_seen_ = true;

    const bool ta_frame = *frame.proof_source == PvrGuestFrameProofSource::TaRender;
    if (ta_frame && !ta_frame_seen_) {
        ta_frame_seen_ = true;
        add_marker(GuestFrameEvidenceMarker::FirstTaFrame);
    }
    if (ta_frame && guest_program_progressed && !post_bootstrap_ta_frame_seen_ &&
        (bootstrap_scanout_seen_ || prior_guest_scanout)) {
        post_bootstrap_ta_frame_seen_ = true;
        add_marker(GuestFrameEvidenceMarker::FirstPostBootstrapTaFrame);
    }
    return observation;
}

bool GuestFrameEvidenceTracker::guest_scanout_seen() const noexcept {
    return guest_scanout_seen_;
}

bool GuestFrameEvidenceTracker::ta_frame_seen() const noexcept {
    return ta_frame_seen_;
}

bool GuestFrameEvidenceTracker::post_bootstrap_ta_frame_seen() const noexcept {
    return post_bootstrap_ta_frame_seen_;
}

bool GuestFrameEvidenceTracker::bootstrap_scanout_seen() const noexcept {
    return bootstrap_scanout_seen_;
}

namespace {

constexpr std::uint64_t frame_hash_offset_basis = 1469598103934665603ull;
constexpr std::uint64_t frame_hash_prime = 1099511628211ull;

void append_frame_hash_byte(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= frame_hash_prime;
}

void append_frame_hash_u32(std::uint64_t& hash, const std::uint32_t value) noexcept {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        append_frame_hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
}

bool valid_frame(const PvrFrame& frame) noexcept {
    if (frame.width == 0u || frame.height == 0u ||
        frame.width > std::numeric_limits<std::size_t>::max() / frame.height)
        return false;
    const auto pixels =
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    return pixels <= std::numeric_limits<std::size_t>::max() / 4u &&
           frame.rgba.size() == pixels * 4u;
}

std::size_t frame_tile_index(const std::uint32_t x,
                             const std::uint32_t y,
                             const std::uint32_t width,
                             const std::uint32_t height) noexcept {
    const auto column = std::min<std::size_t>(
        guest_frame_visibility_tile_columns - 1u,
        static_cast<std::size_t>(
            static_cast<std::uint64_t>(x) * guest_frame_visibility_tile_columns / width));
    const auto row = std::min<std::size_t>(
        guest_frame_visibility_tile_rows - 1u,
        static_cast<std::size_t>(
            static_cast<std::uint64_t>(y) * guest_frame_visibility_tile_rows / height));
    return row * guest_frame_visibility_tile_columns + column;
}

bool interior_frame_tile(const std::size_t index) noexcept {
    const auto column = index % guest_frame_visibility_tile_columns;
    const auto row = index / guest_frame_visibility_tile_columns;
    return column != 0u && column + 1u != guest_frame_visibility_tile_columns &&
           row != 0u && row + 1u != guest_frame_visibility_tile_rows;
}

void describe_selected_frame(GuestFramePumpResult& result,
                             const PvrFrame& frame,
                             const GuestFramePresentedSource source,
                             const std::optional<PvrGuestFrameProofSource> proof_source =
                                  std::nullopt,
                             const FrameDescriptionDetail detail =
                                 FrameDescriptionDetail::Detailed) noexcept {
    result.presented_source = source;
    result.presented_proof_source = proof_source;
    if (detail == FrameDescriptionDetail::None) return;
    if (detail == FrameDescriptionDetail::Detailed) {
        result.presented_frame_evidence =
            describe_guest_frame_presentation(frame);
        result.presented_frame_evidence_collected =
            result.presented_frame_evidence.valid;
        result.presented_pixel_count =
            result.presented_frame_evidence.pixel_count;
        result.presented_nonblack_pixels =
            result.presented_frame_evidence.nonblack_pixels;
        return;
    }
    if (!valid_frame(frame)) return;
    result.presented_pixel_count =
        static_cast<std::uint64_t>(frame.width) * frame.height;
    for (std::size_t offset = 0u;
         offset + 3u < frame.rgba.size();
         offset += 4u) {
        if (frame.rgba[offset] != 0u ||
            frame.rgba[offset + 1u] != 0u ||
            frame.rgba[offset + 2u] != 0u)
            ++result.presented_nonblack_pixels;
    }
    result.presented_frame_evidence_collected = true;
}

} // namespace

GuestFramePresentationEvidence
describe_guest_frame_presentation(const PvrFrame& frame) noexcept {
    GuestFramePresentationEvidence result;
    if (!valid_frame(frame)) return result;

    result.valid = true;
    result.width = frame.width;
    result.height = frame.height;
    result.pixel_count =
        static_cast<std::uint64_t>(frame.width) * frame.height;
    result.digest = frame_hash_offset_basis;
    append_frame_hash_u32(result.digest, frame.width);
    append_frame_hash_u32(result.digest, frame.height);

    std::array<std::uint64_t, 64u> color_class_counts{};
    std::array<std::uint64_t, 16u> luminance_class_counts{};
    std::uint64_t classified_pixels = 0u;
    const auto central_x_begin = frame.width / guest_frame_visibility_tile_columns;
    const auto central_x_end = frame.width - central_x_begin;
    const auto central_y_begin = frame.height / guest_frame_visibility_tile_rows;
    const auto central_y_end = frame.height - central_y_begin;

    for (std::uint32_t y = 0u; y < frame.height; ++y) {
        for (std::uint32_t x = 0u; x < frame.width; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * frame.width + x) * 4u;
            const auto red = frame.rgba[offset];
            const auto green = frame.rgba[offset + 1u];
            const auto blue = frame.rgba[offset + 2u];
            append_frame_hash_byte(result.digest, red);
            append_frame_hash_byte(result.digest, green);
            append_frame_hash_byte(result.digest, blue);
            if (red != 0u || green != 0u || blue != 0u)
                ++result.nonblack_pixels;

            if (x < central_x_begin || x >= central_x_end ||
                y < central_y_begin || y >= central_y_end)
                continue;
            const auto color_class =
                static_cast<std::size_t>((red >> 6u) << 4u) |
                static_cast<std::size_t>((green >> 6u) << 2u) |
                static_cast<std::size_t>(blue >> 6u);
            const auto luminance = static_cast<std::uint8_t>(
                (54u * red + 183u * green + 19u * blue) >> 8u);
            ++color_class_counts[color_class];
            ++luminance_class_counts[luminance >> 4u];
            ++classified_pixels;
        }
    }

    const auto relevant_class_pixels =
        std::max<std::uint64_t>(2u, classified_pixels / 4096u);
    for (std::size_t index = 0u; index < color_class_counts.size(); ++index) {
        if (color_class_counts[index] >= relevant_class_pixels)
            result.relevant_color_class_mask |= std::uint64_t{1u} << index;
    }
    for (std::size_t index = 0u; index < luminance_class_counts.size(); ++index) {
        if (luminance_class_counts[index] >= relevant_class_pixels)
            result.relevant_luminance_class_mask |=
                static_cast<std::uint16_t>(std::uint16_t{1u} << index);
    }
    return result;
}

bool has_relevant_guest_frame_content(
    const GuestFramePresentationEvidence& evidence) noexcept {
    return evidence.valid &&
           std::popcount(evidence.relevant_color_class_mask) >= 3 &&
           std::popcount(evidence.relevant_luminance_class_mask) >= 3;
}

std::uint64_t
minimum_visible_guest_frame_changed_pixels(const std::uint64_t pixel_count) noexcept {
    if (pixel_count == 0u) return 0u;
    const auto scaled = 1u + (pixel_count - 1u) / 200u;
    return std::min(
        pixel_count, std::max<std::uint64_t>(1024u, scaled));
}

void GuestFrameVisibilityClassifier::begin_product_interval(
    std::optional<PvrFrame> baseline) noexcept {
    baseline_.reset();
    baseline_evidence_ = {};
    if (!baseline) return;
    auto evidence = describe_guest_frame_presentation(*baseline);
    if (!evidence.valid) return;
    baseline_ = std::move(*baseline);
    baseline_evidence_ = std::move(evidence);
}

GuestFrameVisibilityObservation GuestFrameVisibilityClassifier::observe(
    const PvrFrame& frame,
    const GuestFramePresentationEvidence& evidence,
    const std::uint64_t proof_changed_pixels) {
    GuestFrameVisibilityObservation result;
    result.baseline_available =
        baseline_.has_value() && baseline_evidence_.valid;
    result.current_frame_valid =
        evidence.valid && valid_frame(frame) &&
        evidence.width == frame.width &&
        evidence.height == frame.height &&
        evidence.pixel_count ==
            static_cast<std::uint64_t>(frame.width) * frame.height;
    result.proof_changed_pixels = proof_changed_pixels;
    result.relevant_color_classes =
        static_cast<std::uint32_t>(
            std::popcount(evidence.relevant_color_class_mask));
    result.relevant_luminance_classes =
        static_cast<std::uint32_t>(
            std::popcount(evidence.relevant_luminance_class_mask));
    if (!result.current_frame_valid || proof_changed_pixels == 0u)
        return result;
    if (!result.baseline_available) {
        baseline_ = frame;
        baseline_evidence_ = evidence;
        return result;
    }

    result.geometry_matches =
        baseline_evidence_.width == evidence.width &&
        baseline_evidence_.height == evidence.height &&
        baseline_evidence_.pixel_count == evidence.pixel_count &&
        baseline_->width == frame.width &&
        baseline_->height == frame.height &&
        baseline_->rgba.size() == frame.rgba.size();
    if (!result.geometry_matches) {
        baseline_ = frame;
        baseline_evidence_ = evidence;
        return result;
    }

    result.digest_changed = baseline_evidence_.digest != evidence.digest;
    result.required_changed_pixels =
        minimum_visible_guest_frame_changed_pixels(evidence.pixel_count);
    if (!result.digest_changed ||
        proof_changed_pixels < result.required_changed_pixels)
        return result;
    result.required_changed_pixels_per_tile =
        std::max<std::uint64_t>(
            4u, (result.required_changed_pixels + 15u) / 16u);
    result.required_changed_pixels_per_class =
        std::max<std::uint64_t>(
            8u, (result.required_changed_pixels + 15u) / 16u);
    std::array<std::uint64_t, guest_frame_visibility_tile_count>
        changed_pixel_counts{};
    std::array<std::uint64_t, 64u> changed_color_class_counts{};
    std::array<std::uint64_t, 16u> changed_luminance_class_counts{};
    const auto central_x_begin =
        frame.width / guest_frame_visibility_tile_columns;
    const auto central_x_end = frame.width - central_x_begin;
    const auto central_y_begin =
        frame.height / guest_frame_visibility_tile_rows;
    const auto central_y_end = frame.height - central_y_begin;
    for (std::uint32_t y = 0u; y < frame.height; ++y) {
        for (std::uint32_t x = 0u; x < frame.width; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * frame.width + x) * 4u;
            if (baseline_->rgba[offset] == frame.rgba[offset] &&
                baseline_->rgba[offset + 1u] == frame.rgba[offset + 1u] &&
                baseline_->rgba[offset + 2u] == frame.rgba[offset + 2u])
                continue;
            ++result.changed_pixels;
            ++changed_pixel_counts[
                frame_tile_index(x, y, frame.width, frame.height)];
            if (x < central_x_begin || x >= central_x_end ||
                y < central_y_begin || y >= central_y_end)
                continue;
            const auto red = frame.rgba[offset];
            const auto green = frame.rgba[offset + 1u];
            const auto blue = frame.rgba[offset + 2u];
            const auto color_class =
                static_cast<std::size_t>((red >> 6u) << 4u) |
                static_cast<std::size_t>((green >> 6u) << 2u) |
                static_cast<std::size_t>(blue >> 6u);
            const auto luminance = static_cast<std::uint8_t>(
                (54u * red + 183u * green + 19u * blue) >> 8u);
            ++changed_color_class_counts[color_class];
            ++changed_luminance_class_counts[luminance >> 4u];
        }
    }
    for (std::size_t index = 0u;
         index < changed_pixel_counts.size();
         ++index) {
        if (changed_pixel_counts[index] <
            result.required_changed_pixels_per_tile)
            continue;
        ++result.changed_tiles;
        if (interior_frame_tile(index)) ++result.changed_interior_tiles;
    }
    for (const auto count : changed_color_class_counts) {
        if (count >= result.required_changed_pixels_per_class)
            ++result.changed_color_classes;
    }
    for (const auto count : changed_luminance_class_counts) {
        if (count >= result.required_changed_pixels_per_class)
            ++result.changed_luminance_classes;
    }

    result.visible_progress =
        result.digest_changed &&
        result.proof_changed_pixels >= result.required_changed_pixels &&
        result.changed_pixels >= result.required_changed_pixels &&
        result.changed_tiles >= guest_frame_minimum_changed_tiles &&
        result.changed_interior_tiles != 0u &&
        result.changed_color_classes >= 3u &&
        result.changed_luminance_classes >= 3u;
    return result;
}

bool GuestFrameVisibilityClassifier::baseline_available() const noexcept {
    return baseline_.has_value() && baseline_evidence_.valid;
}

GuestFramePumpResult pump_guest_frame_proof(
    PvrSoftwareRenderer& renderer,
    NativeVideoOutput* const output,
    const bool collect_detailed_presentation_evidence) {
    auto proof = renderer.take_guest_frame_proof();
    auto scanout = renderer.take_scanout_frame();
    GuestFramePumpResult result;
    if (proof) {
        result.guest_frame_proven = true;
        result.proof_source = proof->source;
        result.render_generation = proof->render_generation;
        result.write_generation_first = proof->write_generation_first;
        result.write_generation_last = proof->write_generation_last;
    }
    if (scanout) {
        // A simultaneously queued scanout was observed after the proof and is
        // therefore the newer physical presentation candidate. Pixel equality
        // cannot establish presentation identity: consecutive VBlanks may be
        // byte-identical. A rejected proof is retried only while no newer
        // scanout has arrived.
        result.presented_source = GuestFramePresentedSource::Scanout;
        if (output != nullptr) {
            describe_selected_frame(result,
                                    *scanout,
                                    GuestFramePresentedSource::Scanout,
                                    std::nullopt,
                                    FrameDescriptionDetail::None);
            const auto presented_before = output->presented_frames();
            output->present(*scanout);
            result.frame_presented = output->presented_frames() > presented_before;
            if (result.frame_presented)
                result.presented_frame = std::move(*scanout);
            else
                static_cast<void>(
                    renderer.retain_unpresented_scanout_frame(
                        std::move(*scanout)));
        }
    } else if (proof) {
        result.presented_source = GuestFramePresentedSource::GuestProof;
        result.presented_proof_source = proof->source;
        result.presented_changed_pixels = proof->changed_pixels;
        if (output != nullptr) {
            describe_selected_frame(result,
                                    proof->frame,
                                    GuestFramePresentedSource::GuestProof,
                                    proof->source,
                                    collect_detailed_presentation_evidence &&
                                            proof->changed_pixels != 0u
                                        ? FrameDescriptionDetail::Detailed
                                        : FrameDescriptionDetail::None);
            result.frame_presented = present_guest_frame_proof(*output, *proof);
            result.proven_frame_presented = result.frame_presented;
            if (result.frame_presented)
                result.presented_frame = std::move(proof->frame);
            else
                static_cast<void>(
                    renderer.retain_unpresented_guest_frame_proof(
                        std::move(*proof)));
        }
    }
    return result;
}

} // namespace katana::runtime
