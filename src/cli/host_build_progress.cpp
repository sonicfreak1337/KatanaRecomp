#include "host_build_progress.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace katana::cli {
namespace {

inline constexpr std::string_view event_header =
    "KATANA_HOST_BUILD_EVENT_V1";
inline constexpr std::uint64_t maximum_event_units = 1'000'000u;
inline constexpr std::size_t maximum_event_files = 65'536u;

enum class EventState : std::uint8_t {
    Started,
    Committed,
    Failed,
};

struct ParsedEvent final {
    std::string identity;
    HostBuildToolKind kind = HostBuildToolKind::Compile;
    EventState state = EventState::Started;
    std::uint64_t units = 0u;
};

struct EventRootIdentity final {
#ifdef _WIN32
    DWORD volume = 0u;
    DWORD index_high = 0u;
    DWORD index_low = 0u;
#else
    dev_t device = 0;
    ino_t inode = 0;
#endif

    auto operator<=>(const EventRootIdentity&) const = default;
};

class EventRootBinding final {
  public:
    explicit EventRootBinding(std::filesystem::path root) noexcept
        : root_(std::move(root)) {
        try {
#ifdef _WIN32
            // Keep the root object open for the observer lifetime. Delete
            // sharing is required because tool launchers atomically rename
            // child event files; path identity is checked around every scan.
            handle_ = CreateFileW(
                root_.c_str(),
                FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (handle_ == INVALID_HANDLE_VALUE) return;
            BY_HANDLE_FILE_INFORMATION identity{};
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (GetFileType(handle_) != FILE_TYPE_DISK ||
                !GetFileInformationByHandle(handle_, &identity) ||
                !GetFileInformationByHandleEx(
                    handle_, FileAttributeTagInfo, &attributes,
                    sizeof(attributes)) ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
                static_cast<void>(CloseHandle(handle_));
                handle_ = INVALID_HANDLE_VALUE;
                return;
            }
            identity_ = EventRootIdentity{
                identity.dwVolumeSerialNumber,
                identity.nFileIndexHigh,
                identity.nFileIndexLow};
#else
            auto flags = O_RDONLY;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
            flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            descriptor_ = ::open(root_.c_str(), flags);
            if (descriptor_ < 0) return;
            struct stat identity {};
            if (::fstat(descriptor_, &identity) != 0 ||
                !S_ISDIR(identity.st_mode)) {
                static_cast<void>(::close(descriptor_));
                descriptor_ = -1;
                return;
            }
            identity_ = EventRootIdentity{
                identity.st_dev, identity.st_ino};
#endif
        } catch (...) {
        }
    }

    ~EventRootBinding() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            static_cast<void>(CloseHandle(handle_));
#else
        if (descriptor_ >= 0) static_cast<void>(::close(descriptor_));
#endif
    }

    EventRootBinding(const EventRootBinding&) = delete;
    EventRootBinding& operator=(const EventRootBinding&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return identity_.has_value();
    }

    [[nodiscard]] bool path_identity_matches() const noexcept {
        if (!identity_) return false;
        try {
#ifdef _WIN32
            const auto current = CreateFileW(
                root_.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (current == INVALID_HANDLE_VALUE) return false;
            BY_HANDLE_FILE_INFORMATION information{};
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            const auto valid =
                GetFileType(current) == FILE_TYPE_DISK &&
                GetFileInformationByHandle(current, &information) &&
                GetFileInformationByHandleEx(
                    current, FileAttributeTagInfo, &attributes,
                    sizeof(attributes)) &&
                (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u &&
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u;
            static_cast<void>(CloseHandle(current));
            return valid && EventRootIdentity{
                                information.dwVolumeSerialNumber,
                                information.nFileIndexHigh,
                                information.nFileIndexLow} == *identity_;
#else
            struct stat information {};
            return ::lstat(root_.c_str(), &information) == 0 &&
                   S_ISDIR(information.st_mode) &&
                   EventRootIdentity{
                       information.st_dev, information.st_ino} == *identity_;
#endif
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool enumerate(
        std::vector<std::string>& names) const noexcept {
        names.clear();
        try {
#ifdef _WIN32
            std::error_code error;
            for (std::filesystem::directory_iterator iterator(
                     root_, error), end;
                 !error && iterator != end;
                 iterator.increment(error)) {
                if (names.size() >= maximum_event_files) return false;
                names.push_back(iterator->path().filename().string());
            }
            return !error;
#else
            auto flags = O_RDONLY;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
            flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            // A new open-file-description is required: dup() would share the
            // directory offset and make later scans start at EOF.
            const auto duplicate =
                ::openat(descriptor_, ".", flags);
            if (duplicate < 0) return false;
            auto* directory = ::fdopendir(duplicate);
            if (directory == nullptr) {
                static_cast<void>(::close(duplicate));
                return false;
            }
            errno = 0;
            bool valid = true;
            while (const auto* entry = ::readdir(directory)) {
                const std::string_view name(entry->d_name);
                if (name == "." || name == "..") continue;
                if (names.size() >= maximum_event_files) {
                    valid = false;
                    break;
                }
                names.emplace_back(name);
            }
            if (errno != 0) valid = false;
            if (::closedir(directory) != 0) valid = false;
            return valid;
#endif
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }
#ifndef _WIN32
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
#endif

  private:
    std::filesystem::path root_;
    std::optional<EventRootIdentity> identity_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

enum class EventReadState : std::uint8_t {
    Ready,
    Vanished,
    Partial,
    Invalid,
};

struct EventReadResult final {
    EventReadState state = EventReadState::Invalid;
    std::string document;
};

[[nodiscard]] EventReadResult read_event_document(
    const EventRootBinding& root,
    const std::string& name) noexcept {
    try {
#ifdef _WIN32
        const auto path = root.root() / name;
        const auto handle = CreateFileW(
            path.c_str(),
            GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return {error == ERROR_FILE_NOT_FOUND ||
                            error == ERROR_PATH_NOT_FOUND
                        ? EventReadState::Vanished
                        : EventReadState::Invalid,
                    {}};
        }
        BY_HANDLE_FILE_INFORMATION identity{};
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        LARGE_INTEGER size{};
        const auto safe_identity =
            GetFileType(handle) == FILE_TYPE_DISK &&
            GetFileInformationByHandle(handle, &identity) &&
            GetFileInformationByHandleEx(
                handle,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) &&
            GetFileSizeEx(handle, &size) &&
            identity.nNumberOfLinks == 1u &&
            (attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY |
              FILE_ATTRIBUTE_REPARSE_POINT)) == 0u;
        if (!safe_identity || size.QuadPart < 0 || size.QuadPart > 256) {
            static_cast<void>(CloseHandle(handle));
            return {EventReadState::Invalid, {}};
        }
        if (size.QuadPart == 0) {
            static_cast<void>(CloseHandle(handle));
            return {EventReadState::Partial, {}};
        }
        std::string document(
            static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0u;
        const auto read_ok = ReadFile(
            handle,
            document.data(),
            static_cast<DWORD>(document.size()),
            &read,
            nullptr) && read == document.size();
        static_cast<void>(CloseHandle(handle));
        return read_ok
                   ? EventReadResult{
                         EventReadState::Ready, std::move(document)}
                   : EventReadResult{EventReadState::Partial, {}};
#else
        auto flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const auto descriptor =
            ::openat(root.descriptor(), name.c_str(), flags);
        if (descriptor < 0)
            return {errno == ENOENT
                        ? EventReadState::Vanished
                        : EventReadState::Invalid,
                    {}};
        struct stat information {};
        const auto safe_identity =
            ::fstat(descriptor, &information) == 0 &&
            S_ISREG(information.st_mode) &&
            information.st_nlink == 1;
        if (!safe_identity || information.st_size < 0 ||
            information.st_size > 256) {
            static_cast<void>(::close(descriptor));
            return {EventReadState::Invalid, {}};
        }
        if (information.st_size == 0) {
            static_cast<void>(::close(descriptor));
            return {EventReadState::Partial, {}};
        }
        std::string document(
            static_cast<std::size_t>(information.st_size), '\0');
        std::size_t offset = 0u;
        while (offset < document.size()) {
            const auto read = ::read(
                descriptor,
                document.data() + offset,
                document.size() - offset);
            if (read < 0 && errno == EINTR) continue;
            if (read <= 0) break;
            offset += static_cast<std::size_t>(read);
        }
        const auto close_ok = ::close(descriptor) == 0;
        return close_ok && offset == document.size()
                   ? EventReadResult{
                         EventReadState::Ready, std::move(document)}
                   : EventReadResult{EventReadState::Partial, {}};
#endif
    } catch (...) {
        return {EventReadState::Invalid, {}};
    }
}

enum class EventParseState : std::uint8_t {
    Parsed,
    Transient,
    Invalid,
};

struct EventParseResult final {
    EventParseState state = EventParseState::Invalid;
    std::optional<ParsedEvent> event;
};

[[nodiscard]] EventParseResult parse_event_file(
    const EventRootBinding& root,
    const std::string& name,
    const HostBuildProgressObserverHooks& hooks) {
    constexpr std::array suffixes{
        std::pair{std::string_view(".started"), EventState::Started},
        std::pair{std::string_view(".committed"), EventState::Committed},
        std::pair{std::string_view(".failed"), EventState::Failed}};
    std::optional<EventState> state;
    std::string identity;
    for (const auto& [suffix, candidate] : suffixes) {
        if (!name.ends_with(suffix)) continue;
        state = candidate;
        identity = name.substr(0u, name.size() - suffix.size());
        break;
    }
    if (!state || !identity.starts_with("event-"))
        return {EventParseState::Invalid, std::nullopt};
    if (hooks.before_event_open)
        hooks.before_event_open(root.root() / name);
    auto document = read_event_document(root, name);
    if (document.state == EventReadState::Vanished ||
        document.state == EventReadState::Partial)
        return {EventParseState::Transient, std::nullopt};
    if (document.state != EventReadState::Ready)
        return {EventParseState::Invalid, std::nullopt};
    std::istringstream input(document.document);
    std::string header;
    std::string kind_line;
    std::string units_line;
    if (!std::getline(input, header) ||
        !std::getline(input, kind_line) ||
        !std::getline(input, units_line))
        return {EventParseState::Transient, std::nullopt};
    input >> std::ws;
    if (!input.eof() || header != event_header ||
        !kind_line.starts_with("kind=") ||
        !units_line.starts_with("units="))
        return {EventParseState::Transient, std::nullopt};
    HostBuildToolKind kind;
    const auto kind_value = std::string_view(kind_line).substr(5u);
    if (kind_value == "compile")
        kind = HostBuildToolKind::Compile;
    else if (kind_value == "link")
        kind = HostBuildToolKind::Link;
    else if (kind_value == "archive")
        kind = HostBuildToolKind::Archive;
    else
        return {EventParseState::Transient, std::nullopt};
    std::uint64_t units = 0u;
    const auto units_value =
        std::string_view(units_line).substr(6u);
    const auto conversion = std::from_chars(
        units_value.data(),
        units_value.data() + units_value.size(),
        units,
        10);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != units_value.data() + units_value.size() ||
        units == 0u || units > maximum_event_units)
        return {EventParseState::Transient, std::nullopt};
    return {
        EventParseState::Parsed,
        ParsedEvent{std::move(identity), kind, *state, units}};
}

} // namespace

class HostBuildProgressObserver::Impl final {
  public:
    Impl(std::filesystem::path event_root,
         HostBuildProgressPlan plan,
         const katana::ProgressReporter& progress,
         HostBuildProgressObserverHooks hooks)
        : event_root_(std::move(event_root)),
          event_root_binding_(event_root_),
          hooks_(std::move(hooks)),
          plan_(std::move(plan)),
          compile_(progress.begin(
              katana::ProgressOperation::Compilation,
              katana::ProgressUnit::TranslationUnits,
              plan_.translation_units,
              "port-host-compile")),
          archive_(progress.begin(
              katana::ProgressOperation::Linking,
              katana::ProgressUnit::Files,
              plan_.archive_steps,
              "port-host-archive")),
          link_(progress.begin(
              katana::ProgressOperation::Linking,
              katana::ProgressUnit::Files,
              plan_.link_steps,
              "port-host-link")) {
        if (!event_root_binding_.valid()) observation_complete_ = false;
        publish();
    }

    void poll() noexcept {
        std::scoped_lock lock(mutex_);
        if (terminal_) return;
        try {
            scan_without_lock(false);
            recompute();
            publish();
        } catch (...) {
            observation_complete_ = false;
        }
    }

    bool finish_success(
        const HostBuildCompletionProof& proof) noexcept {
        std::scoped_lock lock(mutex_);
        if (terminal_) return final_success_;
        try {
            scan_without_lock(true);
            recompute();
            const auto compile_within_plan =
                !plan_.translation_units ||
                compile_started_ <= *plan_.translation_units;
            const auto compile_hits =
                plan_.translation_units && compile_within_plan
                    ? *plan_.translation_units - compile_started_
                    : 0u;
            const auto archive_within_plan =
                archive_started_ <= plan_.archive_steps;
            const auto archive_hits = archive_within_plan
                ? plan_.archive_steps - archive_started_
                : 0u;
            const auto link_within_plan =
                link_started_ <= plan_.link_steps;
            const auto link_hits = link_within_plan
                ? plan_.link_steps - link_started_
                : 0u;
            const auto compile_inventory_valid =
                compile_within_plan &&
                (compile_hits == 0u ||
                 proof.uninvoked_plan_edges_up_to_date_verified);
            const auto archive_inventory_valid =
                archive_within_plan &&
                (archive_hits == 0u ||
                 proof.uninvoked_plan_edges_up_to_date_verified);
            const auto link_inventory_valid =
                link_within_plan &&
                (link_hits == 0u ||
                 proof.uninvoked_plan_edges_up_to_date_verified);
            const auto null_build =
                compile_started_ == 0u && archive_started_ == 0u &&
                link_started_ == 0u;
            if (!observation_complete_ ||
                compile_started_ !=
                    compile_committed_ + compile_failed_ ||
                link_started_ != link_committed_ + link_failed_ ||
                archive_started_ !=
                    archive_committed_ + archive_failed_ ||
                compile_failed_ != 0u || archive_failed_ != 0u ||
                link_failed_ != 0u ||
                !proof.bound_build_graph_succeeded ||
                !proof.process_tree_quiescent ||
                !proof.linked_artifact_verified ||
                !compile_inventory_valid ||
                !archive_inventory_valid ||
                !link_inventory_valid ||
                (null_build &&
                 !proof.zero_tool_invocations_artifact_byte_identical)) {
                observation_complete_ = false;
                compile_.fail();
                archive_.fail();
                link_.fail();
                terminal_ = true;
                return false;
            }
            const auto compile_final = compile_committed_ + compile_hits;
            auto compile_counters = compile_counters_for(
                compile_committed_,
                compile_hits == 0u
                    ? std::nullopt
                    : std::optional<std::uint64_t>(compile_hits),
                true);
            compile_.update(compile_final, std::move(compile_counters));
            if (compile_committed_ == 0u && compile_hits != 0u)
                compile_.cached();
            else
                compile_.complete(compile_final);

            const auto archive_final =
                archive_committed_ + archive_hits;
            auto archive_counters = archive_counters_for(
                archive_committed_,
                archive_hits == 0u
                    ? std::nullopt
                    : std::optional<std::uint64_t>(
                          archive_hits),
                true);
            archive_.update(
                archive_final, std::move(archive_counters));
            if (archive_committed_ == 0u &&
                archive_hits != 0u)
                archive_.cached();
            else
                archive_.complete(archive_final);

            const auto link_final =
                link_committed_ + link_hits;
            auto link_counters = link_counters_for(
                link_committed_,
                link_hits == 0u
                    ? std::nullopt
                    : std::optional<std::uint64_t>(link_hits),
                true);
            link_.update(link_final, std::move(link_counters));
            if (link_committed_ == 0u && link_hits != 0u)
                link_.cached();
            else
                link_.complete(link_final);
            terminal_ = true;
            final_success_ = observation_complete_;
            return final_success_;
        } catch (...) {
            observation_complete_ = false;
            compile_.fail();
            archive_.fail();
            link_.fail();
            terminal_ = true;
            return false;
        }
    }

    void fail() noexcept {
        std::scoped_lock lock(mutex_);
        if (terminal_) return;
        compile_.fail();
        archive_.fail();
        link_.fail();
        terminal_ = true;
    }

    HostBuildProgressSnapshot snapshot() const noexcept {
        std::scoped_lock lock(mutex_);
        return {
            compile_started_, compile_committed_, compile_failed_,
            archive_started_, archive_committed_, archive_failed_,
            link_started_, link_committed_, link_failed_,
            observation_complete_};
    }

  private:
    struct RetainedEvent final {
        HostBuildToolKind kind = HostBuildToolKind::Compile;
        EventState state = EventState::Started;
        std::uint64_t units = 0u;
    };

    enum class ScanState : std::uint8_t {
        Stable,
        Transient,
        Invalid,
    };

    [[nodiscard]] static bool merge_event(
        RetainedEvent& retained,
        const ParsedEvent& parsed) noexcept {
        if (retained.units == 0u) {
            retained = {parsed.kind, parsed.state, parsed.units};
            return true;
        }
        if (retained.kind != parsed.kind ||
            retained.units != parsed.units)
            return false;
        if (retained.state == parsed.state) return true;
        if (retained.state == EventState::Started) {
            retained.state = parsed.state;
            return true;
        }
        if (parsed.state == EventState::Started) return true;
        // Started + Terminal is commutative; only conflicting terminal states
        // are contradictory.
        return false;
    }

    [[nodiscard]] ScanState scan_once(
        std::unordered_map<std::string, RetainedEvent>& scanned) {
        if (!event_root_binding_.path_identity_matches())
            return ScanState::Invalid;
        std::vector<std::string> names;
        if (!event_root_binding_.enumerate(names))
            return ScanState::Transient;
        auto state = ScanState::Stable;
        for (const auto& name : names) {
            const auto parsed =
                parse_event_file(event_root_binding_, name, hooks_);
            if (parsed.state == EventParseState::Invalid)
                return ScanState::Invalid;
            if (parsed.state == EventParseState::Transient) {
                state = ScanState::Transient;
                continue;
            }
            auto& retained = scanned[parsed.event->identity];
            if (!merge_event(retained, *parsed.event))
                return ScanState::Invalid;
        }
        if (!event_root_binding_.path_identity_matches())
            return ScanState::Invalid;
        return state;
    }

    void scan_without_lock(const bool final_scan) {
        constexpr std::size_t live_attempts = 4u;
        constexpr std::size_t final_attempts = 8u;
        const auto attempts = final_scan ? final_attempts : live_attempts;
        for (std::size_t attempt = 0u; attempt < attempts; ++attempt) {
            std::unordered_map<std::string, RetainedEvent> scanned;
            const auto state = scan_once(scanned);
            if (state == ScanState::Invalid) {
                observation_complete_ = false;
                return;
            }
            if (state == ScanState::Stable) {
                for (const auto& [identity, event] : scanned) {
                    const ParsedEvent parsed{
                        identity, event.kind, event.state, event.units};
                    if (!merge_event(events_[identity], parsed)) {
                        observation_complete_ = false;
                        return;
                    }
                }
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // A live writer may still own a partial .started document; only the
        // strict post-quiescence scan converts that bounded transient into a
        // terminal observation loss.
        if (final_scan) observation_complete_ = false;
    }

    void recompute() noexcept {
        compile_started_ = 0u;
        compile_committed_ = 0u;
        compile_failed_ = 0u;
        archive_started_ = 0u;
        archive_committed_ = 0u;
        archive_failed_ = 0u;
        link_started_ = 0u;
        link_committed_ = 0u;
        link_failed_ = 0u;
        for (const auto& [identity, event] : events_) {
            static_cast<void>(identity);
            auto* started = &link_started_;
            auto* committed = &link_committed_;
            auto* failed = &link_failed_;
            if (event.kind == HostBuildToolKind::Compile) {
                started = &compile_started_;
                committed = &compile_committed_;
                failed = &compile_failed_;
            } else if (event.kind == HostBuildToolKind::Archive) {
                started = &archive_started_;
                committed = &archive_committed_;
                failed = &archive_failed_;
            }
            *started += event.units;
            if (event.state == EventState::Committed)
                *committed += event.units;
            else if (event.state == EventState::Failed)
                *failed += event.units;
        }
    }

    katana::ProgressCounterSnapshot compile_counters_for(
        const std::uint64_t committed,
        const std::optional<std::uint64_t> cache_hits,
        const bool terminal) const {
        katana::ProgressCounterSnapshot counters;
        counters.configured_workers = plan_.configured_workers;
        if (plan_.translation_units)
            counters.planned_work = *plan_.translation_units;
        counters.started = compile_started_;
        counters.ready_work = committed;
        counters.committed_work = committed;
        counters.active_workers = terminal
                                      ? 0u
                                      : compile_started_ -
                                            compile_committed_ -
                                            compile_failed_;
        if (plan_.translation_units)
            counters.queued_work =
                terminal
                    ? 0u
                    : *plan_.translation_units > compile_started_
                    ? *plan_.translation_units - compile_started_
                    : 0u;
        if (terminal && cache_hits) {
            counters.cache_hits = cache_hits;
        }
        return counters;
    }

    katana::ProgressCounterSnapshot link_counters_for(
        const std::uint64_t committed,
        const std::optional<std::uint64_t> cache_hits,
        const bool terminal) const {
        katana::ProgressCounterSnapshot counters;
        counters.configured_workers = 1u;
        counters.planned_work = plan_.link_steps;
        counters.started = link_started_;
        counters.ready_work = committed;
        counters.committed_work = committed;
        counters.active_workers = terminal
                                      ? 0u
                                      : link_started_ -
                                            link_committed_ - link_failed_;
        counters.queued_work = terminal
            ? 0u
            : plan_.link_steps > link_started_
                ? plan_.link_steps - link_started_
                : 0u;
        if (terminal && cache_hits) {
            counters.cache_hits = cache_hits;
        }
        return counters;
    }

    katana::ProgressCounterSnapshot archive_counters_for(
        const std::uint64_t committed,
        const std::optional<std::uint64_t> cache_hits,
        const bool terminal) const {
        katana::ProgressCounterSnapshot counters;
        counters.configured_workers = 1u;
        counters.planned_work = plan_.archive_steps;
        counters.started = archive_started_;
        counters.ready_work = committed;
        counters.committed_work = committed;
        counters.active_workers = terminal
                                      ? 0u
                                      : archive_started_ -
                                            archive_committed_ -
                                            archive_failed_;
        counters.queued_work = terminal
            ? 0u
            : plan_.archive_steps > archive_started_
                ? plan_.archive_steps - archive_started_
                : 0u;
        if (terminal && cache_hits) counters.cache_hits = cache_hits;
        return counters;
    }

    void publish() {
        compile_.update(
            compile_committed_,
            compile_counters_for(
                compile_committed_, std::nullopt, false));
        archive_.update(
            archive_committed_,
            archive_counters_for(
                archive_committed_, std::nullopt, false));
        link_.update(
            link_committed_,
            link_counters_for(
                link_committed_, std::nullopt, false));
    }

    std::filesystem::path event_root_;
    EventRootBinding event_root_binding_;
    HostBuildProgressObserverHooks hooks_;
    HostBuildProgressPlan plan_;
    katana::ProgressScope compile_;
    katana::ProgressScope archive_;
    katana::ProgressScope link_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RetainedEvent> events_;
    std::uint64_t compile_started_ = 0u;
    std::uint64_t compile_committed_ = 0u;
    std::uint64_t compile_failed_ = 0u;
    std::uint64_t archive_started_ = 0u;
    std::uint64_t archive_committed_ = 0u;
    std::uint64_t archive_failed_ = 0u;
    std::uint64_t link_started_ = 0u;
    std::uint64_t link_committed_ = 0u;
    std::uint64_t link_failed_ = 0u;
    bool observation_complete_ = true;
    bool terminal_ = false;
    bool final_success_ = false;
};

HostBuildProgressObserver::HostBuildProgressObserver(
    std::filesystem::path event_root,
    HostBuildProgressPlan plan,
    const katana::ProgressReporter& progress,
    HostBuildProgressObserverHooks hooks)
    : impl_(std::make_unique<Impl>(
          std::move(event_root), std::move(plan), progress,
          std::move(hooks))) {}

HostBuildProgressObserver::~HostBuildProgressObserver() = default;
HostBuildProgressObserver::HostBuildProgressObserver(
    HostBuildProgressObserver&&) noexcept = default;
HostBuildProgressObserver& HostBuildProgressObserver::operator=(
    HostBuildProgressObserver&&) noexcept = default;

void HostBuildProgressObserver::poll() noexcept {
    if (impl_) impl_->poll();
}

bool HostBuildProgressObserver::finish_success(
    const HostBuildCompletionProof& proof) noexcept {
    return impl_ && impl_->finish_success(proof);
}

void HostBuildProgressObserver::fail() noexcept {
    if (impl_) impl_->fail();
}

HostBuildProgressSnapshot
HostBuildProgressObserver::snapshot() const noexcept {
    return impl_ ? impl_->snapshot()
                 : HostBuildProgressSnapshot{
                       0u, 0u, 0u,
                       0u, 0u, 0u,
                       0u, 0u, 0u,
                       false};
}

} // namespace katana::cli
