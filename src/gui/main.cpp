#include "katana/gui/model.hpp"

#include <algorithm>
#include <atomic>
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
bool native_qol_smoke = false;
bool native_qol_failed = false;
} // namespace

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

namespace {

constexpr UINT job_finished_message = WM_APP + 1u;
std::jthread job_thread;
HWND content_label = nullptr;
HWND progress_label = nullptr;
HWND overall_progress = nullptr;
HWND step_progress = nullptr;
HWND selected_gdi_view = nullptr;
HWND selected_output_view = nullptr;
HWND log_view = nullptr;
HWND main_window = nullptr;
std::filesystem::path selected_gdi;
std::filesystem::path selected_output;
std::atomic_bool preparing_job = false;
std::shared_ptr<katana::app::Cancellation> preparation_cancellation;
HBRUSH dark_background = nullptr;
HBRUSH dark_control = nullptr;
bool dark_mode = false;
int vertical_scroll = 0;
bool layout_in_progress = false;
constexpr int content_height = 650;

int dpi_scale(const HWND window, const int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

bool high_contrast_enabled() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, contrast.cbSize, &contrast, 0u) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0u;
}

void apply_dark_title_bar(const HWND window) {
    const BOOL enabled = dark_mode ? TRUE : FALSE;
    constexpr DWORD immersive_dark_mode = 20u;
    static_cast<void>(DwmSetWindowAttribute(
        window, immersive_dark_mode, &enabled, static_cast<DWORD>(sizeof(enabled))));
}

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
    if (!model) {
        std::cerr << "KatanaRecomp GUI callback: Modell nicht initialisiert\n";
        return;
    }
    const auto state = model->snapshot();
    auto summary_text = model->accessible_summary();
    if (!selected_gdi.empty()) summary_text += " Gewaehlte GDI: " + selected_gdi.string() + ".";
    if (!selected_output.empty()) summary_text += " Ausgabe: " + selected_output.string() + ".";
    const auto summary = widen(summary_text);
    SetWindowTextW(content_label, summary.c_str());
    SetWindowTextW(selected_gdi_view,
                   selected_gdi.empty() ? L"Keine GDI gewaehlt" : selected_gdi.c_str());
    SetWindowTextW(selected_output_view,
                   selected_output.empty() ? L"Kein Ausgabeordner gewaehlt"
                                           : selected_output.c_str());
    const auto busy = preparing_job.load() || state.job_active;
    auto progress_text = preparing_job.load() ? std::string("GDI wird eingelesen und geprueft ...")
                         : state.job_active   ? "Aktiver Job: " + state.job_stage + " (gesamt " +
                                                  std::to_string(state.job_progress) + " %)"
                                            : std::string("Bereit");
    if (state.job_active && state.job_step_total)
        progress_text += " - " + std::to_string(state.job_step_current.value_or(0u)) + "/" +
                         std::to_string(*state.job_step_total);
    else if (state.job_active)
        progress_text += " - Schrittfortschritt unbestimmt";
    if (state.job_active)
        progress_text += " - " + std::to_string(state.job_elapsed_ms / 1'000u) + " s";
    const auto progress = widen(progress_text);
    SetWindowTextW(progress_label, progress.c_str());
    SendMessageW(overall_progress, PBM_SETPOS, state.job_progress, 0u);
    if (state.job_active && state.job_step_total && *state.job_step_total != 0u) {
        SendMessageW(step_progress, PBM_SETMARQUEE, FALSE, 0u);
        const auto position = static_cast<int>(std::min<std::uint64_t>(
            1'000u, state.job_step_current.value_or(0u) * 1'000u / *state.job_step_total));
        SendMessageW(step_progress, PBM_SETPOS, position, 0u);
    } else if (state.job_active) {
        SendMessageW(step_progress, PBM_SETMARQUEE, TRUE, 40u);
    } else {
        SendMessageW(step_progress, PBM_SETMARQUEE, FALSE, 0u);
        SendMessageW(step_progress, PBM_SETPOS, 0u, 0u);
    }
    std::string log = "Stufe: " + state.job_stage + "\r\n";
    for (const auto& diagnostic : state.diagnostics) {
        log += diagnostic.code + ": " + diagnostic.message +
               "\r\nNaechster Schritt: " + diagnostic.recovery + "\r\n";
    }
    if (!state.live_log.empty()) log += "\r\nBuildlog:\r\n" + state.live_log;
    SetWindowTextW(log_view, widen(log).c_str());
    for (const int control : {1001, 1003, 1006})
        EnableWindow(GetDlgItem(main_window, control), busy ? FALSE : TRUE);
    EnableWindow(GetDlgItem(main_window, 1004), busy ? TRUE : FALSE);
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
    selected_gdi = source;
}

void start_job(HWND window, const katana::app::JobKind kind) {
    if (job_thread.joinable()) return;
    if (selected_gdi.empty()) throw std::logic_error("Zuerst eine .gdi-Quelle waehlen.");
    if (selected_output.empty()) throw std::logic_error("Zuerst einen Ausgabeordner waehlen.");
    const auto source = selected_gdi;
    const auto output = selected_output;
    const auto cancellation = std::make_shared<katana::app::Cancellation>();
    preparation_cancellation = cancellation;
    preparing_job.store(true);
    job_thread = std::jthread([window, kind, source, output, cancellation] {
        try {
            const auto session = std::filesystem::temp_directory_path() / "KatanaRecomp" /
                                 ("session-" + std::to_string(GetCurrentProcessId()));
            std::filesystem::create_directories(session);
            model->new_project(session / "input.katana",
                               source.stem().string(),
                               katana::io::ProjectInputFormat::DreamcastGdi,
                               source,
                               false);
            model->save_project();
            preparing_job.store(false);
            static_cast<void>(
                model->run_job(kind, output, "gui-job", KATANA_RECOMP_VERSION, cancellation));
        } catch (const std::exception& error) {
            preparing_job.store(false);
            show_error(error);
        }
        PostMessageW(window, job_finished_message, 0u, 0);
    });
}

void cancel_active_job() {
    if (preparation_cancellation) preparation_cancellation->request();
    model->cancel_job();
}

void layout_controls(const HWND window) {
    if (layout_in_progress) return;
    layout_in_progress = true;
    RECT client{};
    GetClientRect(window, &client);
    const auto width = std::max(320, static_cast<int>(client.right - client.left));
    const auto height = std::max(1, static_cast<int>(client.bottom - client.top));
    const auto scaled_content_height = dpi_scale(window, content_height);
    const auto maximum_scroll = std::max(0, scaled_content_height - height);
    vertical_scroll = std::clamp(vertical_scroll, 0, maximum_scroll);
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
    info.nMin = 0;
    info.nMax = scaled_content_height - 1;
    info.nPage = static_cast<UINT>(height);
    info.nPos = vertical_scroll;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
    const auto s = [window](const int value) { return dpi_scale(window, value); };
    const auto y = [&s](const int value) { return s(value) - vertical_scroll; };
    MoveWindow(GetDlgItem(window, 1001), s(16), y(16), s(145), s(34), TRUE);
    MoveWindow(GetDlgItem(window, 1003), s(171), y(16), s(145), s(34), TRUE);
    MoveWindow(GetDlgItem(window, 1006), s(326), y(16), s(110), s(34), TRUE);
    MoveWindow(GetDlgItem(window, 1004), s(446), y(16), s(105), s(34), TRUE);
    MoveWindow(selected_gdi_view, s(16), y(62), width - s(32), s(26), TRUE);
    MoveWindow(selected_output_view, s(16), y(96), width - s(32), s(26), TRUE);
    MoveWindow(content_label, s(16), y(136), width - s(32), s(68), TRUE);
    MoveWindow(progress_label, s(16), y(212), width - s(32), s(28), TRUE);
    MoveWindow(overall_progress, s(16), y(246), width - s(32), s(20), TRUE);
    MoveWindow(step_progress, s(16), y(276), width - s(32), s(20), TRUE);
    MoveWindow(log_view, s(16), y(310), width - s(32), s(320), TRUE);
    layout_in_progress = false;
}

LRESULT window_proc_impl(HWND window, const UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        main_window = window;
        dark_mode = model->settings().theme != "light" && !high_contrast_enabled();
        if (dark_mode) {
            dark_background = CreateSolidBrush(RGB(24, 24, 27));
            dark_control = CreateSolidBrush(RGB(38, 38, 42));
            apply_dark_title_bar(window);
        }
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
        const DWORD path_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY;
        selected_gdi_view = CreateWindowW(L"EDIT",
                                          L"Keine GDI gewaehlt",
                                          path_style,
                                          16,
                                          62,
                                          760,
                                          26,
                                          window,
                                          reinterpret_cast<HMENU>(1010),
                                          nullptr,
                                          nullptr);
        selected_output_view = CreateWindowW(L"EDIT",
                                             L"Kein Ausgabeordner gewaehlt",
                                             path_style,
                                             16,
                                             96,
                                             760,
                                             26,
                                             window,
                                             reinterpret_cast<HMENU>(1011),
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
                                      16,
                                      136,
                                      740,
                                      68,
                                      window,
                                      nullptr,
                                      nullptr,
                                      nullptr);
        progress_label = CreateWindowW(L"STATIC",
                                       L"Bereit",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       16,
                                       212,
                                       740,
                                       28,
                                       window,
                                       nullptr,
                                       nullptr,
                                       nullptr);
        overall_progress = CreateWindowW(PROGRESS_CLASSW,
                                         L"",
                                         WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                         16,
                                         246,
                                         760,
                                         20,
                                         window,
                                         reinterpret_cast<HMENU>(1012),
                                         nullptr,
                                         nullptr);
        step_progress = CreateWindowW(PROGRESS_CLASSW,
                                      L"",
                                      WS_CHILD | WS_VISIBLE | PBS_SMOOTH | PBS_MARQUEE,
                                      16,
                                      276,
                                      760,
                                      20,
                                      window,
                                      reinterpret_cast<HMENU>(1013),
                                      nullptr,
                                      nullptr);
        SendMessageW(overall_progress, PBM_SETRANGE32, 0u, 100u);
        SendMessageW(step_progress, PBM_SETRANGE32, 0u, 1'000u);
        if (dark_mode) {
            for (const auto progress : {overall_progress, step_progress}) {
                SetWindowTheme(progress, L"", L"");
                SendMessageW(progress, PBM_SETBKCOLOR, 0u, RGB(38, 38, 42));
                SendMessageW(progress, PBM_SETBARCOLOR, 0u, RGB(230, 116, 37));
            }
        }
        log_view = CreateWindowW(L"EDIT",
                                 L"Stufe: bereit",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                                     ES_AUTOVSCROLL | ES_READONLY,
                                 16,
                                 310,
                                 760,
                                 320,
                                 window,
                                 nullptr,
                                 nullptr,
                                 nullptr);
        for (const int identifier : {1001, 1003, 1004, 1006}) {
            if (dark_mode)
                SetWindowTheme(GetDlgItem(window, identifier), L"DarkMode_Explorer", nullptr);
        }
        layout_controls(window);
        SetTimer(window, 1u, 100u, nullptr);
        refresh();
        if (native_qol_smoke) {
            wchar_t progress_class[64]{};
            GetClassNameW(overall_progress, progress_class, 64);
            SCROLLINFO scroll{};
            scroll.cbSize = sizeof(scroll);
            scroll.fMask = SIF_ALL;
            GetScrollInfo(window, SB_VERT, &scroll);
            native_qol_failed =
                selected_gdi_view == nullptr || selected_output_view == nullptr ||
                overall_progress == nullptr || step_progress == nullptr || log_view == nullptr ||
                std::wstring(progress_class) != PROGRESS_CLASSW ||
                (GetWindowLongPtrW(selected_gdi_view, GWL_STYLE) & WS_TABSTOP) == 0 ||
                GetClassLongPtrW(window, GCLP_HICON) == 0 || dark_mode == high_contrast_enabled();
            for (const int test_dpi : {96, 144, 192, 288}) {
                native_qol_failed = native_qol_failed ||
                                    MulDiv(content_height, test_dpi, 96) <= 0 ||
                                    MulDiv(320, test_dpi, 96) <= MulDiv(26, test_dpi, 96);
            }
            SetWindowPos(window, nullptr, 0, 0, 620, 320, SWP_NOMOVE | SWP_NOZORDER);
            layout_controls(window);
            GetScrollInfo(window, SB_VERT, &scroll);
            native_qol_failed = native_qol_failed || scroll.nMax <= static_cast<int>(scroll.nPage);
            PostMessageW(window, WM_CLOSE, 0u, 0u);
        }
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
                cancel_active_job();
                break;
            default:
                return 0;
            }
            refresh();
        } catch (const std::exception& error) {
            show_error(error);
        }
        return 0;
    case WM_SIZE: {
        static_cast<void>(lparam);
        layout_controls(window);
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window,
                     nullptr,
                     suggested->left,
                     suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout_controls(window);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &info);
        auto next = vertical_scroll;
        switch (LOWORD(wparam)) {
        case SB_LINEUP:
            next -= dpi_scale(window, 24);
            break;
        case SB_LINEDOWN:
            next += dpi_scale(window, 24);
            break;
        case SB_PAGEUP:
            next -= static_cast<int>(info.nPage);
            break;
        case SB_PAGEDOWN:
            next += static_cast<int>(info.nPage);
            break;
        case SB_THUMBTRACK:
            next = info.nTrackPos;
            break;
        default:
            break;
        }
        const auto maximum = std::max(info.nMin, info.nMax - static_cast<int>(info.nPage) + 1);
        vertical_scroll = std::clamp(next, info.nMin, maximum);
        layout_controls(window);
        return 0;
    }
    case WM_ERASEBKGND:
        if (dark_mode) {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, dark_background);
            return 1;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize.x = dpi_scale(window, 590);
        limits->ptMinTrackSize.y = dpi_scale(window, 300);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if (dark_mode) {
            const auto device = reinterpret_cast<HDC>(wparam);
            SetTextColor(device, RGB(235, 235, 240));
            SetBkColor(device, RGB(38, 38, 42));
            return reinterpret_cast<LRESULT>(message == WM_CTLCOLORSTATIC ? dark_background
                                                                          : dark_control);
        }
        break;
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
            cancel_active_job();
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: {
        const auto lines = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        if (GetFocus() == log_view)
            SendMessageW(log_view, EM_LINESCROLL, 0u, -lines * 3);
        else {
            vertical_scroll -= lines * dpi_scale(window, 48);
            layout_controls(window);
        }
        return 0;
    }
    case WM_TIMER:
        refresh();
        return 0;
    case job_finished_message:
        if (job_thread.joinable()) job_thread.join();
        preparation_cancellation.reset();
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
        if ((preparing_job.load() || model->snapshot().job_active) &&
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
        cancel_active_job();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (dark_background != nullptr) DeleteObject(dark_background);
        if (dark_control != nullptr) DeleteObject(dark_control);
        dark_background = nullptr;
        dark_control = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND window,
                             const UINT message,
                             WPARAM wparam,
                             LPARAM lparam) noexcept {
    try {
        return window_proc_impl(window, message, wparam, lparam);
    } catch (const std::exception& error) {
        std::cerr << "KatanaRecomp GUI callback: "
                  << katana::app::redact_sensitive_text(error.what()) << '\n';
        native_qol_failed = true;
        native_automation_failed = true;
        PostQuitMessage(EXIT_FAILURE);
        return 0;
    } catch (...) {
        std::cerr << "KatanaRecomp GUI callback: unbekannter Fehler\n";
        native_qol_failed = true;
        native_automation_failed = true;
        PostQuitMessage(EXIT_FAILURE);
        return 0;
    }
}

int run_desktop() {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_PROGRESS_CLASS;
    if (!InitCommonControlsEx(&common_controls))
        throw std::runtime_error("Windows-Fortschrittssteuerelemente sind nicht verfuegbar.");
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"KatanaRecompMainWindow";
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&window_class) == 0u && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Windows-GUI-Klasse konnte nicht registriert werden.");
    const auto title = widen(std::string("KatanaRecomp ") + KATANA_RECOMP_VERSION);
    const auto window = CreateWindowExW(0,
                                        window_class.lpszClassName,
                                        title.c_str(),
                                        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
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
    const auto title = std::string("KatanaRecomp ") + KATANA_RECOMP_VERSION;
    XStoreName(display, window, title.c_str());
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
#ifdef _WIN32
        if (argc == 2 && std::string_view(argv[1]) == "--qol-smoke") {
            native_qol_smoke = true;
            model->update_settings([](auto& settings) { settings.theme = "dark"; });
            const auto result = run_desktop();
            if (!native_qol_failed) std::cout << "KR_PHASE11_NATIVE_QOL_READY\n";
            return result == 0 && !native_qol_failed ? EXIT_SUCCESS : EXIT_FAILURE;
        }
#endif
        if (argc == 4 && std::string_view(argv[1]) == "--native-automation") {
            selected_gdi = std::filesystem::absolute(argv[2]).lexically_normal();
            selected_output = std::filesystem::absolute(argv[3]).lexically_normal();
            if (selected_gdi.extension() != ".gdi")
                throw std::invalid_argument("Native GUI-Automatisierung akzeptiert nur .gdi.");
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
