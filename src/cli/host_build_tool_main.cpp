#include "host_build_tool.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::optional<std::string> environment_value(
    const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0u;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        return std::nullopt;
    const auto result = *value == '\0'
        ? std::optional<std::string>{}
        : std::optional<std::string>(value);
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
#endif
}

[[nodiscard]] std::optional<katana::cli::HostBuildToolKind>
parse_kind(const std::string_view value) {
    if (value == "compile")
        return katana::cli::HostBuildToolKind::Compile;
    if (value == "archive")
        return katana::cli::HostBuildToolKind::Archive;
    if (value == "link")
        return katana::cli::HostBuildToolKind::Link;
    return std::nullopt;
}

[[nodiscard]] std::string lower_filename(const char* value) {
    auto result = std::filesystem::path(value).filename().string();
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

[[nodiscard]] int run_copied_wrapper(
    const int argc,
    char* argv[]) {
    const auto invoked_name = lower_filename(argv[0]);
    const auto archive =
        invoked_name == "katana-host-archive-wrapper" ||
        invoked_name == "katana-host-archive-wrapper.exe";
    const auto compile = invoked_name == "katana-host-cl-wrapper.exe";
    const auto link = invoked_name == "katana-host-link-wrapper.exe";
    if (!archive && !compile && !link) return -1;

    const auto kind = archive
        ? katana::cli::HostBuildToolKind::Archive
        : compile ? katana::cli::HostBuildToolKind::Compile
                  : katana::cli::HostBuildToolKind::Link;
    auto event_root = environment_value("KATANA_HOST_BUILD_EVENT_ROOT");
    std::optional<std::filesystem::path> derived_event_root;
    if (!event_root && archive) {
        const auto wrapper_directory =
            std::filesystem::absolute(argv[0]).parent_path();
        if (wrapper_directory.filename() == ".katana-host-build-tools")
            derived_event_root = wrapper_directory.parent_path() /
                                 ".katana-host-build-events";
    }
    const auto real_tool = environment_value(
        compile ? "KATANA_HOST_BUILD_REAL_COMPILER"
                : link ? "KATANA_HOST_BUILD_REAL_LINKER"
                       : "KATANA_HOST_BUILD_REAL_ARCHIVER");
    if ((!event_root && !derived_event_root) || argc < 2 ||
        (!archive && !real_tool))
        return 125;

    const auto tool = real_tool ? *real_tool : std::string(argv[1]);
    const auto first_argument = real_tool ? 1 : 2;
    std::vector<const char*> arguments;
    arguments.reserve(static_cast<std::size_t>(
        std::max(0, argc - first_argument)));
    for (int index = first_argument; index < argc; ++index)
        arguments.push_back(argv[index]);
    return katana::cli::run_host_build_tool_launcher(
        kind,
        event_root ? std::filesystem::path(*event_root)
                   : *derived_event_root,
        tool,
        arguments);
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc < 1 || argv[0] == nullptr) return 125;
    if (const auto copied = run_copied_wrapper(argc, argv); copied >= 0)
        return copied;
    if (argc < 5) return 125;

    const auto kind = parse_kind(argv[2]);
    if (!kind) return 125;
    const auto mode = std::string_view(argv[3]);
    std::string tool;
    std::vector<const char*> arguments;

    if (mode == "--direct") {
        tool = argv[4];
        for (int index = 5; index < argc; ++index)
            arguments.push_back(argv[index]);
    } else if (mode == "--chain") {
        if (argc < 6) return 125;
        tool = argv[4];
        for (int index = 5; index < argc; ++index)
            arguments.push_back(argv[index]);
    } else if (mode == "--compiler-cache") {
        const auto cache =
            environment_value("KATANA_HOST_BUILD_COMPILER_CACHE");
        if (cache) {
            tool = *cache;
            for (int index = 4; index < argc; ++index)
                arguments.push_back(argv[index]);
        } else {
            tool = argv[4];
            for (int index = 5; index < argc; ++index)
                arguments.push_back(argv[index]);
        }
    } else {
        return 125;
    }

    return katana::cli::run_host_build_tool_launcher(
        *kind, std::filesystem::path(argv[1]),
        std::move(tool), arguments);
}
