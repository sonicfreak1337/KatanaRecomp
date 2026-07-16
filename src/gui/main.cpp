#include "katana/gui/model.hpp"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
std::unique_ptr<katana::gui::Model> model;
bool native_smoke = false;
bool native_automation = false;
bool native_automation_failed = false;
} // namespace

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

namespace {

constexpr UINT job_finished_message = WM_APP + 1u;
std::jthread job_thread;
HWND content_label = nullptr;
HWND progress_label = nullptr;
HWND log_view = nullptr;
HWND main_window = nullptr;
std::filesystem::path selected_gdi;
std::filesystem::path selected_output;

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

std::string session_suffix() {
    return std::to_string(GetCurrentProcessId());
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
    auto summary_text = model->accessible_summary();
    if (!selected_output.empty()) summary_text += " Ausgabe: " + selected_output.string() + ".";
    const auto summary = widen(summary_text);
    SetWindowTextW(content_label, summary.c_str());
    const auto progress = widen(state.job_active ? "Aktiver Job: " + state.job_stage + " (" +
                                                       std::to_string(state.job_progress) + " %)"
                                                 : "Bereit");
    SetWindowTextW(progress_label, progress.c_str());
    std::string log = "Stufe: " + state.job_stage + "\r\n";
    for (const auto& diagnostic : state.diagnostics) {
        log += diagnostic.code + ": " + diagnostic.message +
               "\r\nNaechster Schritt: " + diagnostic.recovery + "\r\n";
    }
    const auto build_log = selected_output / "recompile.log";
    if (!selected_output.empty() && std::filesystem::exists(build_log)) {
        std::ifstream input(build_log, std::ios::binary);
        std::string compiler_log((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
        constexpr std::size_t visible_log_limit = 16'000u;
        if (compiler_log.size() > visible_log_limit)
            compiler_log.erase(0u, compiler_log.size() - visible_log_limit);
        log += "\r\nBuildlog:\r\n" + compiler_log;
    }
    SetWindowTextW(log_view, widen(log).c_str());
    for (const int control : {1001, 1003, 1006})
        EnableWindow(GetDlgItem(main_window, control), state.job_active ? FALSE : TRUE);
    EnableWindow(GetDlgItem(main_window, 1004), state.job_active ? TRUE : FALSE);
}

void show_error(const std::exception& error) {
    const auto message = widen(katana::app::redact_sensitive_text(error.what()));
    MessageBoxW(
        nullptr, message.c_str(), L"KatanaRecomp - Wiederherstellung", MB_OK | MB_ICONERROR);
}

std::filesystem::path run_file_dialog(const std::wstring& mode) {
    std::vector<wchar_t> executable_path(32768u);
    const auto executable_length = GetModuleFileNameW(
        nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (executable_length == 0u || executable_length == executable_path.size())
        throw std::runtime_error("GUI-Programmpfad ist nicht bestimmbar.");
    auto helper = std::filesystem::path(std::wstring(executable_path.data(), executable_length))
                      .parent_path() /
                  "katana-file-dialog.exe";
    const auto result_path = std::filesystem::temp_directory_path() /
                             ("katana-dialog-" + std::to_string(GetCurrentProcessId()) + "-" +
                              std::to_string(GetTickCount64()) + ".txt");
    std::wstring command =
        L"\"" + helper.wstring() + L"\" " + mode + L" \"" + result_path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr,
                        command.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        helper.parent_path().c_str(),
                        &startup,
                        &process)) {
        throw std::runtime_error("Isolierter nativer Dateidialog konnte nicht gestartet werden.");
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0u;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0u) throw std::runtime_error("Nativer Dateidialog ist fehlgeschlagen.");
    std::ifstream input(result_path, std::ios::binary);
    if (!input) throw std::runtime_error("Dateidialog-Ergebnis konnte nicht gelesen werden.");
    std::string selected((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    std::error_code cleanup_error;
    std::filesystem::remove(result_path, cleanup_error);
    return selected.empty() ? std::filesystem::path{} : std::filesystem::path(widen(selected));
}

std::filesystem::path select_source(HWND) {
    return run_file_dialog(L"open-source");
}

std::filesystem::path select_output(HWND) {
    return run_file_dialog(L"select-folder");
}

void select_gdi(HWND window) {
    const auto source = select_source(window);
    if (source.empty()) return;
    auto extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".gdi") throw std::invalid_argument("Die GUI akzeptiert nur .gdi-Quellen.");
    const auto session = std::filesystem::temp_directory_path() / "KatanaRecomp" /
                         ("session-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(session);
    const auto manifest = session / "input.katana";
    model->new_project(
        manifest, source.stem().string(), katana::io::ProjectInputFormat::DreamcastGdi, source);
    model->save_project();
    selected_gdi = source;
}

void start_job(HWND window, const katana::app::JobKind kind) {
    if (job_thread.joinable()) return;
    if (selected_gdi.empty()) throw std::logic_error("Zuerst eine .gdi-Quelle waehlen.");
    if (selected_output.empty()) throw std::logic_error("Zuerst einen Ausgabeordner waehlen.");
    const auto output = selected_output;
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
        main_window = window;
        const wchar_t* button_class = L"BUTTON";
        const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
        CreateWindowW(button_class,
                      L"GDI waehlen",
                      style,
                      16,
                      16,
                      145,
                      34,
                      window,
                      reinterpret_cast<HMENU>(1001),
                      nullptr,
                      nullptr);
        CreateWindowW(button_class,
                      L"Ausgabeordner",
                      style,
                      171,
                      16,
                      145,
                      34,
                      window,
                      reinterpret_cast<HMENU>(1003),
                      nullptr,
                      nullptr);
        CreateWindowW(button_class,
                      L"Rekompilieren",
                      style,
                      326,
                      16,
                      110,
                      34,
                      window,
                      reinterpret_cast<HMENU>(1006),
                      nullptr,
                      nullptr);
        CreateWindowW(button_class,
                      L"Abbrechen",
                      style,
                      466,
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
                                      85,
                                      window,
                                      nullptr,
                                      nullptr,
                                      nullptr);
        progress_label = CreateWindowW(L"STATIC",
                                       L"Bereit",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       18,
                                       170,
                                       740,
                                       30,
                                       window,
                                       nullptr,
                                       nullptr,
                                       nullptr);
        log_view = CreateWindowW(L"EDIT",
                                 L"Stufe: bereit",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                                     ES_AUTOVSCROLL | ES_READONLY,
                                 18,
                                 210,
                                 760,
                                 150,
                                 window,
                                 nullptr,
                                 nullptr,
                                 nullptr);
        SetTimer(window, 1u, 100u, nullptr);
        refresh();
        if (native_smoke) PostMessageW(window, WM_CLOSE, 0u, 0u);
        if (native_automation) {
            SetWindowPos(window, nullptr, 0, 0, 960, 600, SWP_NOMOVE | SWP_NOZORDER);
            PostMessageW(window, WM_KEYDOWN, VK_F6, 0u);
            PostMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0u, WHEEL_DELTA), 0u);
            PostMessageW(window, WM_COMMAND, 1006u, 0u);
        }
        return 0;
    }
    case WM_COMMAND:
        try {
            switch (LOWORD(wparam)) {
            case 1001: {
                select_gdi(window);
                break;
            }
            case 1003:
                selected_output = select_output(window);
                break;
            case 1006:
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
    case WM_SIZE: {
        const auto width = LOWORD(lparam);
        const auto height = HIWORD(lparam);
        MoveWindow(content_label, 18, 76, std::max(100, static_cast<int>(width) - 36), 85, TRUE);
        MoveWindow(progress_label, 18, 170, std::max(100, static_cast<int>(width) - 36), 30, TRUE);
        MoveWindow(log_view,
                   18,
                   210,
                   std::max(100, static_cast<int>(width) - 36),
                   std::max(80, static_cast<int>(height) - 230),
                   TRUE);
        return 0;
    }
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
    case WM_MOUSEWHEEL: {
        const auto lines = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        SendMessageW(log_view, EM_LINESCROLL, 0u, -lines * 3);
        return 0;
    }
    case WM_TIMER:
        refresh();
        return 0;
    case job_finished_message:
        if (job_thread.joinable()) job_thread.join();
        refresh();
        if (native_automation) {
            const auto state = model->snapshot();
            native_automation_failed =
                state.job_active || !state.diagnostics.empty() ||
                !std::filesystem::exists(selected_output / "game.exe") ||
                !std::filesystem::exists(selected_output / "sourcecode" / "CMakeLists.txt") ||
                !std::filesystem::exists(selected_output / "recompile.log");
            DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        if (model->snapshot().job_active &&
            MessageBoxW(window,
                        L"Aktiven Job abbrechen und KatanaRecomp schliessen?",
                        L"KatanaRecomp",
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
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
#include <unistd.h>

namespace {
std::filesystem::path settings_path() {
    const auto* home = std::getenv("HOME");
    return home == nullptr
               ? std::filesystem::path(".katana-settings-v1.conf")
               : std::filesystem::path(home) / ".config" / "KatanaRecomp" / "settings-v1.conf";
}

std::string session_suffix() {
    return std::to_string(getpid());
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
            const auto source = std::filesystem::absolute(argv[2]).lexically_normal();
            if (source.extension() != ".gdi")
                throw std::invalid_argument("GUI-Automatisierung akzeptiert nur .gdi-Quellen.");
            const auto session = std::filesystem::temp_directory_path() / "KatanaRecomp" /
                                 ("automation-" + session_suffix());
            std::filesystem::create_directories(session);
            model->new_project(session / "input.katana",
                               source.stem().string(),
                               katana::io::ProjectInputFormat::DreamcastGdi,
                               source);
            model->save_project();
            const auto result = model->run_job(
                katana::app::JobKind::Build, argv[3], "gui-automation", KATANA_RECOMP_VERSION);
            std::cout << "KR_PHASE10_GUI_MODEL_AUTOMATION\n"
                      << katana::app::format_job_result_json(result)
                      << model->automation_snapshot_json();
            return result.state == katana::app::JobState::Completed ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--native-smoke") {
            native_smoke = true;
            const auto result = run_desktop();
            std::cout << "KR_PHASE10_NATIVE_SHELL_LIFECYCLE\n";
            return result;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--native-automation") {
            selected_gdi = std::filesystem::absolute(argv[2]).lexically_normal();
            selected_output = std::filesystem::absolute(argv[3]).lexically_normal();
            if (selected_gdi.extension() != ".gdi")
                throw std::invalid_argument("Native GUI-Automatisierung akzeptiert nur .gdi.");
            const auto session = std::filesystem::temp_directory_path() / "KatanaRecomp" /
                                 ("native-automation-" + session_suffix());
            std::filesystem::create_directories(session);
            model->new_project(session / "input.katana",
                               selected_gdi.stem().string(),
                               katana::io::ProjectInputFormat::DreamcastGdi,
                               selected_gdi);
            model->save_project();
            native_automation = true;
            const auto result = run_desktop();
            if (!native_automation_failed) std::cout << "KR_PHASE10_NATIVE_GUI_END_TO_END\n";
            return result == 0 && !native_automation_failed ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        return run_desktop();
    } catch (const std::exception& error) {
        std::cerr << "KatanaRecomp GUI: " << katana::app::redact_sensitive_text(error.what())
                  << '\n';
        return EXIT_FAILURE;
    }
}
