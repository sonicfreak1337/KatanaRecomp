#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace katana::codegen::detail {

enum class SecureArtifactKind : std::uint8_t {
    Missing,
    Unsafe,
    Regular,
    Oversized,
};

struct SecureArtifactRead {
    SecureArtifactKind kind = SecureArtifactKind::Missing;
    std::string content;
};

[[nodiscard]] inline bool cache_path_within(
    const std::filesystem::path& root,
    const std::filesystem::path& target) {
    const auto relative = target.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != "..";
}

[[nodiscard]] inline std::string secure_cache_nonce() {
    std::random_device random;
    return std::to_string(random()) + '-' + std::to_string(random());
}

#ifdef _WIN32

class WinHandle final {
public:
    WinHandle() = default;
    explicit WinHandle(const HANDLE value) noexcept : value_(value) {}
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this == &other) return *this;
        reset();
        value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        return *this;
    }
    ~WinHandle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

    void reset() noexcept {
        if (*this) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct WinFileIdentity {
    DWORD volume = 0u;
    DWORD index_high = 0u;
    DWORD index_low = 0u;

    bool operator==(const WinFileIdentity&) const = default;
};

[[nodiscard]] inline std::optional<BY_HANDLE_FILE_INFORMATION>
win_file_information(const HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information))
        return std::nullopt;
    return information;
}

[[nodiscard]] inline WinFileIdentity win_file_identity(
    const BY_HANDLE_FILE_INFORMATION& information) noexcept {
    return {
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow};
}

[[nodiscard]] inline bool win_safe_directory_information(
    const BY_HANDLE_FILE_INFORMATION& information) noexcept {
    return (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u &&
           (information.dwFileAttributes &
            FILE_ATTRIBUTE_REPARSE_POINT) == 0u;
}

[[nodiscard]] inline bool win_safe_regular_information(
    const BY_HANDLE_FILE_INFORMATION& information) noexcept {
    return (information.dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0u;
}

struct WinDirectoryChain {
    std::vector<WinHandle> handles;
    std::vector<WinFileIdentity> identities;
};

struct WinDirectoryChainResult {
    SecureArtifactKind kind = SecureArtifactKind::Unsafe;
    WinDirectoryChain chain;
};

[[nodiscard]] inline WinDirectoryChainResult win_open_directory_chain(
    const std::filesystem::path& root,
    const std::filesystem::path& directory,
    const bool create) {
    if (!root.is_absolute() || !directory.is_absolute() ||
        (!cache_path_within(root, directory) &&
         directory != root))
        return {};

    const auto root_path = directory.root_path();
    if (root_path.empty() || root.root_path() != root_path)
        return {};

    std::size_t root_component_count = 0u;
    for (const auto& component : root.relative_path()) {
        if (!component.empty() && component != ".")
            ++root_component_count;
    }

    WinDirectoryChain result;
    auto open_directory =
        [](const std::filesystem::path& path) {
            return WinHandle(CreateFileW(
                path.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
        };

    auto current = root_path;
    auto handle = open_directory(current);
    if (!handle) {
        return {
            GetLastError() == ERROR_FILE_NOT_FOUND ||
                    GetLastError() == ERROR_PATH_NOT_FOUND
                ? SecureArtifactKind::Missing
                : SecureArtifactKind::Unsafe,
            {}};
    }
    auto information = win_file_information(handle.get());
    if (!information ||
        !win_safe_directory_information(*information))
        return {};
    result.identities.push_back(win_file_identity(*information));
    result.handles.push_back(std::move(handle));

    std::size_t component_index = 0u;
    for (const auto& component : directory.relative_path()) {
        if (component.empty() || component == ".") continue;
        current /= component;
        handle = open_directory(current);
        if (!handle) {
            const auto error = GetLastError();
            const bool missing =
                error == ERROR_FILE_NOT_FOUND ||
                error == ERROR_PATH_NOT_FOUND;
            const bool may_create = create && missing;
            if (!may_create)
                return {
                    missing ? SecureArtifactKind::Missing
                            : SecureArtifactKind::Unsafe,
                    {}};
            if (!CreateDirectoryW(current.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                return {};
            handle = open_directory(current);
            if (!handle) return {};
        }
        information = win_file_information(handle.get());
        if (!information ||
            !win_safe_directory_information(*information))
            return {};
        result.identities.push_back(win_file_identity(*information));
        result.handles.push_back(std::move(handle));
        ++component_index;
    }
    return {SecureArtifactKind::Regular, std::move(result)};
}

[[nodiscard]] inline WinHandle win_open_regular_file(
    const std::filesystem::path& path,
    const DWORD access) {
    return WinHandle(CreateFileW(
        path.c_str(),
        access,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
}

[[nodiscard]] inline SecureArtifactRead win_read_open_file(
    const HANDLE handle,
    const std::size_t maximum_bytes) {
    const auto before = win_file_information(handle);
    if (!before || !win_safe_regular_information(*before))
        return {SecureArtifactKind::Unsafe, {}};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0)
        return {SecureArtifactKind::Unsafe, {}};
    if (static_cast<std::uint64_t>(size.QuadPart) >
        maximum_bytes)
        return {SecureArtifactKind::Oversized, {}};
    if (static_cast<std::uint64_t>(size.QuadPart) >
        std::numeric_limits<std::size_t>::max())
        return {SecureArtifactKind::Unsafe, {}};

    std::string content(
        static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0u;
    while (offset < content.size()) {
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            content.size() - offset,
            1024u * 1024u));
        DWORD read = 0u;
        if (!ReadFile(
                handle,
                content.data() + offset,
                request,
                &read,
                nullptr) ||
            read != request)
            return {SecureArtifactKind::Unsafe, {}};
        offset += read;
    }
    std::array<char, 1u> extra{};
    DWORD extra_read = 0u;
    if (!ReadFile(
            handle,
            extra.data(),
            static_cast<DWORD>(extra.size()),
            &extra_read,
            nullptr) ||
        extra_read != 0u)
        return {SecureArtifactKind::Unsafe, {}};
    const auto after = win_file_information(handle);
    if (!after ||
        win_file_identity(*before) != win_file_identity(*after) ||
        before->nFileSizeHigh != after->nFileSizeHigh ||
        before->nFileSizeLow != after->nFileSizeLow ||
        CompareFileTime(
            &before->ftLastWriteTime,
            &after->ftLastWriteTime) != 0)
        return {SecureArtifactKind::Unsafe, {}};
    return {SecureArtifactKind::Regular, std::move(content)};
}

[[nodiscard]] inline SecureArtifactRead secure_cache_read(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::size_t maximum_bytes) {
    const auto chain = win_open_directory_chain(
        root, path.parent_path(), false);
    if (chain.kind != SecureArtifactKind::Regular)
        return {chain.kind, {}};
    const auto file = win_open_regular_file(path, GENERIC_READ);
    if (!file) {
        const auto error = GetLastError();
        return {
            error == ERROR_FILE_NOT_FOUND ||
                    error == ERROR_PATH_NOT_FOUND
                ? SecureArtifactKind::Missing
                : SecureArtifactKind::Unsafe,
            {}};
    }
    return win_read_open_file(file.get(), maximum_bytes);
}

[[nodiscard]] inline bool win_delete_open_file(
    const HANDLE handle) {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(
               handle,
               FileDispositionInfo,
               &disposition,
               sizeof(disposition)) != FALSE;
}

[[nodiscard]] inline bool secure_cache_erase_if_matches(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::string_view expected_content,
    const std::size_t maximum_bytes) {
    const auto chain = win_open_directory_chain(
        root, path.parent_path(), false);
    if (chain.kind != SecureArtifactKind::Regular) return false;
    const auto file =
        win_open_regular_file(path, GENERIC_READ | DELETE);
    if (!file) return false;
    const auto read = win_read_open_file(file.get(), maximum_bytes);
    return read.kind == SecureArtifactKind::Regular &&
           read.content == expected_content &&
           win_delete_open_file(file.get());
}

[[nodiscard]] inline bool secure_cache_erase_oversized(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::size_t maximum_bytes) {
    const auto chain = win_open_directory_chain(
        root, path.parent_path(), false);
    if (chain.kind != SecureArtifactKind::Regular) return false;
    const auto file =
        win_open_regular_file(path, GENERIC_READ | DELETE);
    if (!file) return false;
    const auto information = win_file_information(file.get());
    LARGE_INTEGER size{};
    return information &&
           win_safe_regular_information(*information) &&
           GetFileSizeEx(file.get(), &size) &&
           size.QuadPart >= 0 &&
           static_cast<std::uint64_t>(size.QuadPart) >
               maximum_bytes &&
           win_delete_open_file(file.get());
}

inline void win_write_all(
    const HANDLE handle,
    const std::string_view content) {
    std::size_t offset = 0u;
    while (offset < content.size()) {
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            content.size() - offset,
            1024u * 1024u));
        DWORD written = 0u;
        if (!WriteFile(
                handle,
                content.data() + offset,
                request,
                &written,
                nullptr) ||
            written != request)
            throw std::runtime_error(
                "Begrenztes Cache-Artefakt konnte nicht geschrieben "
                "werden.");
        offset += written;
    }
    if (!FlushFileBuffers(handle))
        throw std::runtime_error(
            "Begrenztes Cache-Artefakt konnte nicht finalisiert "
            "werden.");
}

inline void secure_cache_publish(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::string_view content,
    const std::size_t maximum_bytes) {
    auto chain = win_open_directory_chain(
        root, path.parent_path(), true);
    if (chain.kind != SecureArtifactKind::Regular)
        throw std::runtime_error(
            "Begrenzter Cache besitzt keinen sicheren Zielordner.");

    std::filesystem::path staging;
    for (std::size_t attempt = 0u; attempt < 32u; ++attempt) {
        staging = root /
                  (".publish-bounded-" + secure_cache_nonce());
        if (CreateDirectoryW(staging.c_str(), nullptr)) break;
        staging.clear();
    }
    if (staging.empty())
        throw std::runtime_error(
            "Begrenztes Cache-Staging konnte nicht atomar angelegt "
            "werden.");

    WinHandle staging_handle(CreateFileW(
        staging.c_str(),
        FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    const auto staging_information =
        staging_handle
            ? win_file_information(staging_handle.get())
            : std::nullopt;
    if (!staging_information ||
        !win_safe_directory_information(*staging_information)) {
        if (staging_handle)
            static_cast<void>(
                win_delete_open_file(staging_handle.get()));
        staging_handle.reset();
        throw std::runtime_error(
            "Begrenztes Cache-Staging ist kein sicherer Ordner.");
    }

    const auto temporary = staging / "artifact.tmp";
    WinHandle temporary_handle(CreateFileW(
        temporary.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!temporary_handle) {
        static_cast<void>(
            win_delete_open_file(staging_handle.get()));
        staging_handle.reset();
        throw std::runtime_error(
            "Begrenztes Cache-Stagingartefakt konnte nicht angelegt "
            "werden.");
    }

    const auto cleanup = [&]() noexcept {
        if (temporary_handle)
            static_cast<void>(
                win_delete_open_file(temporary_handle.get()));
        temporary_handle.reset();
        if (staging_handle)
            static_cast<void>(
                win_delete_open_file(staging_handle.get()));
        staging_handle.reset();
    };
    try {
        win_write_all(temporary_handle.get(), content);
        const auto temporary_information =
            win_file_information(temporary_handle.get());
        if (!temporary_information ||
            !win_safe_regular_information(*temporary_information))
            throw std::runtime_error(
                "Begrenztes Cache-Stagingartefakt ist unsicher.");

        if (!CreateHardLinkW(
                path.c_str(), temporary.c_str(), nullptr)) {
            const auto error = GetLastError();
            cleanup();
            const auto concurrent = secure_cache_read(
                root, path, maximum_bytes);
            if (concurrent.kind ==
                    SecureArtifactKind::Regular &&
                concurrent.content == content)
                return;
            if (error == ERROR_FILE_EXISTS ||
                error == ERROR_ALREADY_EXISTS)
                throw std::runtime_error(
                    "Begrenzter Cache-Publish kollidiert mit einem "
                    "unsicheren oder abweichenden Artefakt.");
            throw std::runtime_error(
                "Begrenzter Cache-Publish konnte keinen atomaren "
                "Hardlink anlegen.");
        }

        // The published hardlink now owns the immutable bytes. Closing the
        // writable staging handle before reopening the target both removes
        // Windows share-mode ambiguity for concurrent readers and ensures
        // verification observes an object that no cache handle can mutate.
        if (!win_delete_open_file(temporary_handle.get()))
            throw std::runtime_error(
                "Begrenztes Cache-Stagingartefakt konnte nicht "
                "entfernt werden.");
        temporary_handle.reset();
        WinHandle published(CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        const auto published_information =
            published ? win_file_information(published.get())
                      : std::nullopt;
        if (!published_information ||
            !win_safe_regular_information(*published_information) ||
            win_file_identity(*published_information) !=
                win_file_identity(*temporary_information) ||
            [&]() {
                const auto verified = win_read_open_file(
                    published.get(), maximum_bytes);
                return verified.kind !=
                           SecureArtifactKind::Regular ||
                       verified.content != content;
            }()) {
            throw std::runtime_error(
                "Begrenzter Cache-Publish verlor seine "
                "Dateiidentitaet.");
        }
        cleanup();
    } catch (...) {
        cleanup();
        throw;
    }
}

#else

class PosixFd final {
public:
    PosixFd() = default;
    explicit PosixFd(const int value) noexcept : value_(value) {}
    PosixFd(const PosixFd&) = delete;
    PosixFd& operator=(const PosixFd&) = delete;
    PosixFd(PosixFd&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    PosixFd& operator=(PosixFd&& other) noexcept {
        if (this == &other) return *this;
        reset();
        value_ = std::exchange(other.value_, -1);
        return *this;
    }
    ~PosixFd() { reset(); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ >= 0;
    }

    void reset() noexcept {
        if (*this) close(value_);
        value_ = -1;
    }

private:
    int value_ = -1;
};

struct PosixFileIdentity {
    dev_t device{};
    ino_t inode{};

    bool operator==(const PosixFileIdentity&) const = default;
};

[[nodiscard]] inline PosixFileIdentity posix_file_identity(
    const struct stat& status) noexcept {
    return {status.st_dev, status.st_ino};
}

[[nodiscard]] inline bool posix_same_snapshot(
    const struct stat& left,
    const struct stat& right) noexcept {
    if (posix_file_identity(left) != posix_file_identity(right) ||
        left.st_mode != right.st_mode ||
        left.st_size != right.st_size ||
        left.st_nlink != right.st_nlink)
        return false;
#ifdef __APPLE__
    return left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec &&
           left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
           left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec &&
           left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
    return left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
}

struct PosixDirectoryChain {
    PosixFd parent;
    PosixFd cache_root;
    std::vector<PosixFileIdentity> identities;
};

struct PosixDirectoryChainResult {
    SecureArtifactKind kind = SecureArtifactKind::Unsafe;
    PosixDirectoryChain chain;
};

[[nodiscard]] inline PosixDirectoryChainResult
posix_open_directory_chain(
    const std::filesystem::path& root,
    const std::filesystem::path& directory,
    const bool create) {
    if (!root.is_absolute() || !directory.is_absolute() ||
        (!cache_path_within(root, directory) && directory != root) ||
        root.root_path() != directory.root_path())
        return {};

    std::size_t root_component_count = 0u;
    for (const auto& component : root.relative_path()) {
        if (!component.empty() && component != ".")
            ++root_component_count;
    }

    PosixFd current(open(
        directory.root_path().c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!current)
        return {
            errno == ENOENT ? SecureArtifactKind::Missing
                            : SecureArtifactKind::Unsafe,
            {}};
    struct stat status{};
    if (fstat(current.get(), &status) != 0 ||
        !S_ISDIR(status.st_mode))
        return {};

    PosixDirectoryChain result;
    result.identities.push_back(posix_file_identity(status));
    if (root_component_count == 0u) {
        result.cache_root = PosixFd(dup(current.get()));
        if (!result.cache_root) return {};
    }

    std::size_t component_index = 0u;
    for (const auto& component : directory.relative_path()) {
        if (component.empty() || component == ".") continue;
        auto next = PosixFd(openat(
            current.get(),
            component.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!next) {
            const auto open_error = errno;
            const bool may_create =
                create && open_error == ENOENT;
            if (!may_create)
                return {
                    open_error == ENOENT
                        ? SecureArtifactKind::Missing
                        : SecureArtifactKind::Unsafe,
                    {}};
            if (mkdirat(
                    current.get(), component.c_str(), 0700) != 0 &&
                errno != EEXIST)
                return {};
            next = PosixFd(openat(
                current.get(),
                component.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (!next) return {};
        }
        if (fstat(next.get(), &status) != 0 ||
            !S_ISDIR(status.st_mode))
            return {};
        result.identities.push_back(posix_file_identity(status));
        ++component_index;
        if (component_index == root_component_count) {
            result.cache_root = PosixFd(dup(next.get()));
            if (!result.cache_root) return {};
        }
        current = std::move(next);
    }
    result.parent = std::move(current);
    if (!result.cache_root) return {};
    return {SecureArtifactKind::Regular, std::move(result)};
}

[[nodiscard]] inline bool posix_directory_chain_matches(
    const std::filesystem::path& root,
    const std::filesystem::path& directory,
    const std::vector<PosixFileIdentity>& expected) {
    const auto current =
        posix_open_directory_chain(root, directory, false);
    return current.kind == SecureArtifactKind::Regular &&
           current.chain.identities == expected;
}

[[nodiscard]] inline SecureArtifactRead posix_read_open_file(
    const int descriptor,
    const std::size_t maximum_bytes,
    struct stat* const snapshot = nullptr) {
    struct stat before{};
    if (fstat(descriptor, &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_size < 0)
        return {SecureArtifactKind::Unsafe, {}};
    if (static_cast<std::uint64_t>(before.st_size) >
        maximum_bytes) {
        if (snapshot != nullptr) *snapshot = before;
        return {SecureArtifactKind::Oversized, {}};
    }
    if (static_cast<std::uint64_t>(before.st_size) >
        std::numeric_limits<std::size_t>::max())
        return {SecureArtifactKind::Unsafe, {}};

    std::string content(
        static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0u;
    while (offset < content.size()) {
        ssize_t count = -1;
        do {
            count = read(
                descriptor,
                content.data() + offset,
                std::min<std::size_t>(
                    content.size() - offset,
                    1024u * 1024u));
        } while (count < 0 && errno == EINTR);
        if (count <= 0)
            return {SecureArtifactKind::Unsafe, {}};
        offset += static_cast<std::size_t>(count);
    }
    std::array<char, 1u> extra{};
    ssize_t extra_count = -1;
    do {
        extra_count =
            read(descriptor, extra.data(), extra.size());
    } while (extra_count < 0 && errno == EINTR);
    if (extra_count != 0)
        return {SecureArtifactKind::Unsafe, {}};
    struct stat after{};
    if (fstat(descriptor, &after) != 0 ||
        !posix_same_snapshot(before, after))
        return {SecureArtifactKind::Unsafe, {}};
    if (snapshot != nullptr) *snapshot = after;
    return {SecureArtifactKind::Regular, std::move(content)};
}

[[nodiscard]] inline SecureArtifactRead secure_cache_read(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::size_t maximum_bytes) {
    const auto chain = posix_open_directory_chain(
        root, path.parent_path(), false);
    if (chain.kind != SecureArtifactKind::Regular)
        return {chain.kind, {}};
    PosixFd file(openat(
        chain.chain.parent.get(),
        path.filename().c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!file)
        return {
            errno == ENOENT ? SecureArtifactKind::Missing
                            : SecureArtifactKind::Unsafe,
            {}};
    auto read = posix_read_open_file(file.get(), maximum_bytes);
    if (read.kind == SecureArtifactKind::Regular &&
        !posix_directory_chain_matches(
            root,
            path.parent_path(),
            chain.chain.identities))
        return {SecureArtifactKind::Unsafe, {}};
    return read;
}

[[nodiscard]] inline bool posix_remove_name_if_identity(
    const PosixDirectoryChain& chain,
    const std::filesystem::path& filename,
    const struct stat& expected) {
    std::string quarantine;
    PosixFd quarantine_directory;
    for (std::size_t attempt = 0u; attempt < 32u; ++attempt) {
        quarantine =
            ".erase-bounded-" + secure_cache_nonce();
        if (mkdirat(
                chain.parent.get(),
                quarantine.c_str(),
                0700) == 0) {
            quarantine_directory = PosixFd(openat(
                chain.parent.get(),
                quarantine.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            break;
        }
    }
    if (!quarantine_directory) return false;
    const auto cleanup_directory = [&]() noexcept {
        quarantine_directory.reset();
        static_cast<void>(unlinkat(
            chain.parent.get(),
            quarantine.c_str(),
            AT_REMOVEDIR));
    };
    if (renameat(
            chain.parent.get(),
            filename.c_str(),
            quarantine_directory.get(),
            "artifact") != 0) {
        cleanup_directory();
        return false;
    }
    PosixFd quarantined(openat(
        quarantine_directory.get(),
        "artifact",
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat actual{};
    if (!quarantined ||
        fstat(quarantined.get(), &actual) != 0 ||
        posix_file_identity(actual) !=
            posix_file_identity(expected)) {
        if (linkat(
                quarantine_directory.get(),
                "artifact",
                chain.parent.get(),
                filename.c_str(),
                0) == 0)
            static_cast<void>(unlinkat(
                quarantine_directory.get(), "artifact", 0));
        cleanup_directory();
        return false;
    }
    const auto removed =
        unlinkat(quarantine_directory.get(), "artifact", 0) == 0;
    cleanup_directory();
    return removed;
}

[[nodiscard]] inline bool secure_cache_erase_if_matches(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::string_view expected_content,
    const std::size_t maximum_bytes) {
    const auto chain = posix_open_directory_chain(
        root, path.parent_path(), false);
    if (chain.kind != SecureArtifactKind::Regular) return false;
    PosixFd file(openat(
        chain.chain.parent.get(),
        path.filename().c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!file) return false;
    struct stat snapshot{};
    const auto read = posix_read_open_file(
        file.get(), maximum_bytes, &snapshot);
    return read.kind == SecureArtifactKind::Regular &&
           read.content == expected_content &&
           posix_directory_chain_matches(
               root,
               path.parent_path(),
               chain.chain.identities) &&
           posix_remove_name_if_identity(
               chain.chain, path.filename(), snapshot);
}

[[nodiscard]] inline bool secure_cache_erase_oversized(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::size_t maximum_bytes) {
    const auto chain = posix_open_directory_chain(
        root, path.parent_path(), false);
    if (chain.kind != SecureArtifactKind::Regular) return false;
    PosixFd file(openat(
        chain.chain.parent.get(),
        path.filename().c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!file) return false;
    struct stat snapshot{};
    const auto read = posix_read_open_file(
        file.get(), maximum_bytes, &snapshot);
    return read.kind == SecureArtifactKind::Oversized &&
           posix_directory_chain_matches(
               root,
               path.parent_path(),
               chain.chain.identities) &&
           posix_remove_name_if_identity(
               chain.chain, path.filename(), snapshot);
}

inline void posix_write_all(
    const int descriptor,
    const std::string_view content) {
    std::size_t offset = 0u;
    while (offset < content.size()) {
        ssize_t count = -1;
        do {
            count = write(
                descriptor,
                content.data() + offset,
                std::min<std::size_t>(
                    content.size() - offset,
                    1024u * 1024u));
        } while (count < 0 && errno == EINTR);
        if (count <= 0)
            throw std::runtime_error(
                "Begrenztes Cache-Artefakt konnte nicht geschrieben "
                "werden.");
        offset += static_cast<std::size_t>(count);
    }
    if (fsync(descriptor) != 0)
        throw std::runtime_error(
            "Begrenztes Cache-Artefakt konnte nicht finalisiert "
            "werden.");
}

inline void secure_cache_publish(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::string_view content,
    const std::size_t maximum_bytes) {
    auto chain = posix_open_directory_chain(
        root, path.parent_path(), true);
    if (chain.kind != SecureArtifactKind::Regular)
        throw std::runtime_error(
            "Begrenzter Cache besitzt keinen sicheren Zielordner.");

    std::string staging;
    PosixFd staging_directory;
    for (std::size_t attempt = 0u; attempt < 32u; ++attempt) {
        staging = ".publish-bounded-" + secure_cache_nonce();
        if (mkdirat(
                chain.chain.cache_root.get(),
                staging.c_str(),
                0700) == 0) {
            staging_directory = PosixFd(openat(
                chain.chain.cache_root.get(),
                staging.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            break;
        }
    }
    if (!staging_directory)
        throw std::runtime_error(
            "Begrenztes Cache-Staging konnte nicht atomar angelegt "
            "werden.");

    PosixFd temporary(openat(
        staging_directory.get(),
        "artifact.tmp",
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600));
    const auto cleanup = [&]() noexcept {
        temporary.reset();
        static_cast<void>(unlinkat(
            staging_directory.get(), "artifact.tmp", 0));
        staging_directory.reset();
        static_cast<void>(unlinkat(
            chain.chain.cache_root.get(),
            staging.c_str(),
            AT_REMOVEDIR));
    };
    if (!temporary) {
        cleanup();
        throw std::runtime_error(
            "Begrenztes Cache-Stagingartefakt konnte nicht angelegt "
            "werden.");
    }
    try {
        posix_write_all(temporary.get(), content);
        struct stat temporary_status{};
        if (fstat(temporary.get(), &temporary_status) != 0 ||
            !S_ISREG(temporary_status.st_mode))
            throw std::runtime_error(
                "Begrenztes Cache-Stagingartefakt ist unsicher.");
        if (linkat(
                staging_directory.get(),
                "artifact.tmp",
                chain.chain.parent.get(),
                path.filename().c_str(),
                0) != 0) {
            const auto error = errno;
            cleanup();
            if (error == EEXIST) {
                const auto concurrent = secure_cache_read(
                    root, path, maximum_bytes);
                if (concurrent.kind ==
                        SecureArtifactKind::Regular &&
                    concurrent.content == content)
                    return;
                throw std::runtime_error(
                    "Begrenzter Cache-Publish kollidiert mit einem "
                    "unsicheren oder abweichenden Artefakt.");
            }
            throw std::runtime_error(
                "Begrenzter Cache-Publish konnte keinen atomaren "
                "Hardlink anlegen.");
        }
        PosixFd published(openat(
            chain.chain.parent.get(),
            path.filename().c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat published_status{};
        const auto verified =
            published
                ? posix_read_open_file(
                      published.get(), maximum_bytes,
                      &published_status)
                : SecureArtifactRead{
                      SecureArtifactKind::Unsafe, {}};
        if (!published ||
            posix_file_identity(published_status) !=
                posix_file_identity(temporary_status) ||
            verified.kind != SecureArtifactKind::Regular ||
            verified.content != content) {
            static_cast<void>(posix_remove_name_if_identity(
                chain.chain,
                path.filename(),
                temporary_status));
            throw std::runtime_error(
                "Begrenzter Cache-Publish verlor seine "
                "Dateiidentitaet.");
        }
        if (!posix_directory_chain_matches(
                root,
                path.parent_path(),
                chain.chain.identities)) {
            static_cast<void>(posix_remove_name_if_identity(
                chain.chain,
                path.filename(),
                temporary_status));
            throw std::runtime_error(
                "Begrenzter Cache-Publish verlor seine "
                Elternverzeichnisidentitaet.");
        }
        cleanup();
    } catch (...) {
        cleanup();
        throw;
    }
}

#endif

} // namespace katana::codegen::detail
