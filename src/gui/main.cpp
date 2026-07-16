#include "katana/gui/model.hpp"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
std::unique_ptr<katana::gui::Model> model;
}

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

namespace {

constexpr UINT job_finished_message = WM_APP + 1u;
std::jthread job_thread;
HWND content_label = nullptr;
HWND progress_label = nullptr;

std::filesystem::path settings_path() {
    wchar_t* local_app_data = nullptr;
    std::size_t length = 0u;
    if (_wdupenv_s(&local_app_data, &length, L"LOCALAPPDATA") == 0 && local_app_data != nullptr) {
        const auto result =
            std::filesystem::path(local_app_data) / "KatanaRecomp" / "settings-v1.conf";
        std::free(local_app_data);
        return result;
    }
    return std::filesystem::current_path() / ".katana-settings-v1.conf";
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
    result.pop_back();
    return result;
}

void refresh() {
    const auto state = model->snapshot();
    const auto summary = widen(model->accessible_summary());
    SetWindowTextW(content_label, summary.c_str());
    const auto progress = widen(state.job_active ? "Aktiver Job: " + state.job_stage + " (" +
                                                       std::to_string(state.job_progress) + " %)"
                                                 : "Bereit");
    SetWindowTextW(progress_label, progress.c_str());
}

void show_error(const std::exception& error) {
    const auto message = widen(katana::app::redact_sensitive_text(error.what()));
    MessageBoxW(
        nullptr, message.c_str(), L"KatanaRecomp - Wiederherstellung", MB_OK | MB_ICONERROR);
}

std::filesystem::path select_manifest(HWND window) {
    std::vector<wchar_t> path(32768u);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter =
        L"Katana-Projekte (*.katana;*.manifest)\0*.katana;*.manifest\0Alle Dateien\0*.*\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(path.data()) : std::filesystem::path{};
}

std::filesystem::path select_source(HWND window) {
    std::vector<wchar_t> path(32768u);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter =
        L"Katana-Quellen (*.gdi;*.elf;*.bin)\0*.gdi;*.elf;*.bin\0Alle Dateien\0*.*\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(path.data()) : std::filesystem::path{};
}

std::filesystem::path select_new_manifest(HWND window) {
    std::vector<wchar_t> path(32768u);
    const std::wstring default_name = L"project.katana";
    std::copy(default_name.begin(), default_name.end(), path.begin());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Katana-Projekt (*.katana)\0*.katana\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrDefExt = L"katana";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&dialog) ? std::filesystem::path(path.data()) : std::filesystem::path{};
}

void create_project(HWND window) {
    const auto source = select_source(window);
    if (source.empty()) return;
    const auto manifest = select_new_manifest(window);
    if (manifest.empty()) return;
    auto extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    const auto format = extension == L".gdi"   ? katana::io::ProjectInputFormat::DreamcastGdi
                        : extension == L".elf" ? katana::io::ProjectInputFormat::Elf32Sh
                                               : katana::io::ProjectInputFormat::RawBinary;
    model->new_project(manifest, manifest.stem().string(), format, source);
    model->save_project();
}

void start_job(HWND window, const katana::app::JobKind kind) {
    if (job_thread.joinable()) return;
    const auto output = std::filesystem::temp_directory_path() / "KatanaRecomp" / "gui-jobs";
    job_thread = std::jthread([window, kind, output] {
        try {
            static_cast<void>(model->run_job(kind, output, "gui-job", KATANA_RECOMP_VERSION));
        } catch (const std::exception& error) {
            show_error(error);
        }
        PostMessageW(window, job_finished_message, 0u, 0);
    });
}

LRESULT CALLBACK window_proc(HWND window, const UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        const wchar_t* button_class = L"BUTTON";
        const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
        CreateWindowW(button_class,
                      L"Neues Projekt",
                      style,
                      16,
                      16,
                      125,
                      34,
                      window,
                      reinterpret_cast<HMENU>(1005),
                      nullptr,
                      nullptr);
        CreateWindowW(button_class,
                      L"Projekt oeffnen",
                      style,
                      151,
                      16,
                      145,
                      34,
                      window,
                      reinterpret_cast<HMENU>(1001),
                      nullptr,
                      nullptr);
        CreateWindowW(button_class,
                      L"Analysieren",
                      style,
                      306,
                      16,
                      110,
                      34,
                      window,
                      reinterpret_cast<HMENU>(1002),
                      nullptr,
                      nullptr);
        CreateWindowW(button_class,
                      L"Build vorbereiten",
                      style,
                      426,
                      16,
                      145,
                      34,
                      window,
                      reinterpret_cast<HMENU>(1003),
                      nullptr,
                      nullptr);
        CreateWindowW(button_class,
                      L"Abbrechen",
                      style,
                      581,
                      16,
                      105,
                      34,
                      window,
                      reinterpret_cast<HMENU>(1004),
                      nullptr,
                      nullptr);
        content_label = CreateWindowW(L"STATIC",
                                      L"KatanaRecomp",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      18,
                                      76,
                                      740,
                                      180,
                                      window,
                                      nullptr,
                                      nullptr,
                                      nullptr);
        progress_label = CreateWindowW(L"STATIC",
                                       L"Bereit",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       18,
                                       280,
                                       740,
                                       30,
                                       window,
                                       nullptr,
                                       nullptr,
                                       nullptr);
        SetTimer(window, 1u, 100u, nullptr);
        refresh();
        return 0;
    }
    case WM_COMMAND:
        try {
            switch (LOWORD(wparam)) {
            case 1005:
                create_project(window);
                break;
            case 1001: {
                const auto path = select_manifest(window);
                if (!path.empty()) model->open_project(path);
                break;
            }
            case 1002:
                start_job(window, katana::app::JobKind::Analyze);
                break;
            case 1003:
                start_job(window, katana::app::JobKind::Build);
                break;
            case 1004:
                model->cancel_job();
                break;
            default:
                break;
            }
            refresh();
        } catch (const std::exception& error) {
            show_error(error);
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F6) {
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
                model->navigate_previous();
            else
                model->navigate_next();
            refresh();
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            model->cancel_job();
            return 0;
        }
        break;
    case WM_TIMER:
        refresh();
        return 0;
    case job_finished_message:
        if (job_thread.joinable()) job_thread.join();
        refresh();
        return 0;
    case WM_CLOSE:
        if (model->has_unsaved_changes() &&
            MessageBoxW(window,
                        L"Ungespeicherte Projektaenderungen verwerfen?",
                        L"KatanaRecomp",
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        model->cancel_job();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int run_desktop() {
    SetProcessDPIAware();
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"KatanaRecompMainWindow";
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&window_class) == 0u && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Windows-GUI-Klasse konnte nicht registriert werden.");
    const auto window = CreateWindowExW(0,
                                        window_class.lpszClassName,
                                        L"KatanaRecomp 0.40 - Phase 10",
                                        WS_OVERLAPPEDWINDOW,
                                        CW_USEDEFAULT,
                                        CW_USEDEFAULT,
                                        820,
                                        420,
                                        nullptr,
                                        nullptr,
                                        instance,
                                        nullptr);
    if (window == nullptr)
        throw std::runtime_error("KatanaRecomp-Hauptfenster konnte nicht geoeffnet werden.");
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0u, 0u) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

} // namespace

#else
#include <X11/Xlib.h>

namespace {
std::filesystem::path settings_path() {
    const auto* home = std::getenv("HOME");
    return home == nullptr
               ? std::filesystem::path(".katana-settings-v1.conf")
               : std::filesystem::path(home) / ".config" / "KatanaRecomp" / "settings-v1.conf";
}

int run_desktop() {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) throw std::runtime_error("X11-Anzeige ist nicht verfuegbar.");
    const auto screen = DefaultScreen(display);
    const auto window = XCreateSimpleWindow(display,
                                            RootWindow(display, screen),
                                            10,
                                            10,
                                            820,
                                            420,
                                            1,
                                            BlackPixel(display, screen),
                                            WhitePixel(display, screen));
    XStoreName(display, window, "KatanaRecomp 0.40 - Phase 10");
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(display, window);
    bool running = true;
    while (running) {
        XEvent event{};
        XNextEvent(display, &event);
        if (event.type == Expose) {
            const auto summary = model->accessible_summary();
            XDrawString(display,
                        window,
                        DefaultGC(display, screen),
                        20,
                        40,
                        summary.c_str(),
                        static_cast<int>(summary.size()));
        } else if (event.type == KeyPress) {
            running = false;
        } else if (event.type == DestroyNotify) {
            running = false;
        }
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
} // namespace
#endif

int main(const int argc, char* argv[]) {
    try {
        model = std::make_unique<katana::gui::Model>(settings_path());
        if (argc == 2 && std::string_view(argv[1]) == "--smoke") {
            std::cout << "KR_PHASE10_GUI_MINIMAL_START\n" << model->automation_snapshot_json();
            return EXIT_SUCCESS;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--automation") {
            model->open_project(argv[2]);
            const auto result = model->run_job(
                katana::app::JobKind::Build, argv[3], "gui-automation", KATANA_RECOMP_VERSION);
            std::cout << "KR_PHASE10_GUI_END_TO_END\n"
                      << katana::app::format_job_result_json(result)
                      << model->automation_snapshot_json();
            return result.state == katana::app::JobState::Completed ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        return run_desktop();
    } catch (const std::exception& error) {
        std::cerr << "KatanaRecomp GUI: " << katana::app::redact_sensitive_text(error.what())
                  << '\n';
        return EXIT_FAILURE;
    }
}
