#include "katana/runtime/host_video.hpp"

#include <limits>
#include <stdexcept>

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
        if (config.initially_visible) show();
    }

    ~Win32VideoOutput() override {
        if (window_ != nullptr) DestroyWindow(window_);
    }

    void show() override {
        if (window_ == nullptr) throw std::logic_error("Natives Videofenster ist geschlossen.");
        ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
        visible_ = true;
    }

    void poll_events() override {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0u, 0u, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!pending_error_.empty()) {
            const auto error = std::move(pending_error_);
            pending_error_.clear();
            throw std::runtime_error(error);
        }
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
        frame_width_ = frame.width;
        frame_height_ = frame.height;
        bgra_.resize(frame.rgba.size());
        for (std::size_t offset = 0u; offset < frame.rgba.size(); offset += 4u) {
            bgra_[offset] = frame.rgba[offset + 2u];
            bgra_[offset + 1u] = frame.rgba[offset + 1u];
            bgra_[offset + 2u] = frame.rgba[offset];
            bgra_[offset + 3u] = frame.rgba[offset + 3u];
        }
        ++presented_frames_;
        InvalidateRect(window_, nullptr, FALSE);
        if (visible_) UpdateWindow(window_);
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
        return presented_frames_;
    }

  private:
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
        if (message == WM_SIZE && wparam != SIZE_MINIMIZED) {
            client_width_ = static_cast<std::uint32_t>(LOWORD(lparam));
            client_height_ = static_cast<std::uint32_t>(HIWORD(lparam));
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            if (dc == nullptr) {
                pending_error_ = "Natives Videofenster konnte nicht gezeichnet werden.";
                return 0;
            }
            RECT client{};
            GetClientRect(window, &client);
            if (FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))) == 0) {
                pending_error_ = "Natives Videofenster konnte nicht gezeichnet werden.";
            }
            if (!bgra_.empty() && client_width_ != 0u && client_height_ != 0u) {
                const auto scale = std::min(static_cast<double>(client_width_) / frame_width_,
                                            static_cast<double>(client_height_) / frame_height_);
                const auto width = static_cast<int>(frame_width_ * scale);
                const auto height = static_cast<int>(frame_height_ * scale);
                const auto x = (static_cast<int>(client_width_) - width) / 2;
                const auto y = (static_cast<int>(client_height_) - height) / 2;
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth = static_cast<LONG>(frame_width_);
                bitmap.bmiHeader.biHeight = -static_cast<LONG>(frame_height_);
                bitmap.bmiHeader.biPlanes = 1u;
                bitmap.bmiHeader.biBitCount = 32u;
                bitmap.bmiHeader.biCompression = BI_RGB;
                if (StretchDIBits(dc,
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
                                  SRCCOPY) == GDI_ERROR) {
                    pending_error_ = "Nativer Videoframe konnte nicht praesentiert werden.";
                }
            }
            EndPaint(window, &paint);
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
    std::vector<std::uint8_t> bgra_;
    std::vector<NativeHostEvent> events_;
    std::string pending_error_;
    std::uint32_t frame_width_ = 0u;
    std::uint32_t frame_height_ = 0u;
    std::uint32_t client_width_ = 0u;
    std::uint32_t client_height_ = 0u;
    std::uint64_t presented_frames_ = 0u;
    std::uint64_t next_event_sequence_ = 1u;
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

bool present_guest_frame_proof(NativeVideoOutput& output, const PvrGuestFrameProof& proof) {
    const auto presented_before = output.presented_frames();
    output.present(proof.frame);
    return output.presented_frames() > presented_before;
}

GuestFramePumpResult pump_guest_frame_proof(PvrSoftwareRenderer& renderer,
                                            NativeVideoOutput* const output) {
    auto proof = renderer.take_guest_frame_proof();
    if (!proof) return {};
    GuestFramePumpResult result;
    result.guest_frame_proven = true;
    if (output != nullptr) result.frame_presented = present_guest_frame_proof(*output, *proof);
    return result;
}

} // namespace katana::runtime
