#define NOMINMAX
#include <windows.h>

#include <commdlg.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::wstring argument(const int argc, wchar_t* argv[], const int index) {
    return index < argc ? std::wstring(argv[index]) : std::wstring{};
}

} // namespace

int wmain(const int argc, wchar_t* argv[]) {
    if (argc != 3) return 2;
    const auto mode = argument(argc, argv, 1);
    const auto output_path = std::filesystem::path(argument(argc, argv, 2));
    std::vector<wchar_t> path(32768u);
    if (mode == L"save-manifest") {
        const std::wstring default_name = L"project.katana";
        std::copy(default_name.begin(), default_name.end(), path.begin());
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    BOOL selected = FALSE;
    if (mode == L"select-folder") {
        BROWSEINFOW browse{};
        browse.lpszTitle = L"Ausgabeordner fuer Sourcecode und game.exe waehlen";
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) == S_OK) {
            if (auto* item = SHBrowseForFolderW(&browse); item != nullptr) {
                selected = SHGetPathFromIDListW(item, path.data());
                CoTaskMemFree(item);
            }
            CoUninitialize();
        }
    } else if (mode == L"open-manifest") {
        dialog.lpstrFilter =
            L"Katana-Projekte (*.katana;*.manifest)\0*.katana;*.manifest\0Alle Dateien\0*.*\0";
        dialog.Flags |= OFN_FILEMUSTEXIST;
        selected = GetOpenFileNameW(&dialog);
    } else if (mode == L"open-source") {
        dialog.lpstrFilter =
            L"Katana-Quellen (*.gdi;*.elf;*.bin)\0*.gdi;*.elf;*.bin\0Alle Dateien\0*.*\0";
        dialog.Flags |= OFN_FILEMUSTEXIST;
        selected = GetOpenFileNameW(&dialog);
    } else if (mode == L"save-manifest") {
        dialog.lpstrFilter = L"Katana-Projekt (*.katana)\0*.katana\0";
        dialog.lpstrDefExt = L"katana";
        dialog.Flags |= OFN_OVERWRITEPROMPT;
        selected = GetSaveFileNameW(&dialog);
    } else {
        return 2;
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return 3;
    if (selected) {
        const auto length =
            WideCharToMultiByte(CP_UTF8, 0, path.data(), -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, path.data(), -1, utf8.data(), length, nullptr, nullptr);
        utf8.pop_back();
        output << utf8;
    }
    return output ? 0 : 3;
}
