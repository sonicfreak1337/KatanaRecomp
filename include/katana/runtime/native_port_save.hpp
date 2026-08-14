#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

class NativePortPlatformServices;

// This is a title/SDK-facing persistence provider, not a Maple or VMU device
// model. A title adapter maps its own ABI and guest-memory side effects to
// these completed semantic operations.
inline constexpr std::uint32_t native_port_save_provider_contract_version = 1u;
inline constexpr std::uint32_t native_port_save_default_block_bytes = 512u;
inline constexpr std::uint32_t native_port_save_default_block_count = 200u;
inline constexpr std::uint64_t native_port_save_any_generation = UINT64_MAX;

struct NativePortSaveEndpoint final {
    std::uint8_t controller_index = 0u;
    std::uint8_t storage_index = 0u;

    [[nodiscard]] bool operator==(const NativePortSaveEndpoint&) const = default;
};

struct NativePortSaveUnitConfig final {
    NativePortSaveEndpoint endpoint;
    // Stable, title-selected identity. It is serialized into every volume and
    // prevents a newly selected host medium from accepting another medium's
    // records merely because it occupies the same SDK endpoint.
    std::string_view identity;
    bool present = false;
    bool writable = true;
    std::uint32_t block_bytes = native_port_save_default_block_bytes;
    std::uint32_t block_count = native_port_save_default_block_count;
    std::uint32_t maximum_file_count = 32u;
};

struct NativePortSaveProviderConfig final {
    std::uint32_t contract_version = native_port_save_provider_contract_version;
    // Namespace for the title's semantic Save/VMU family. It contributes to
    // the platform record key but never becomes a host-controlled path.
    std::string_view provider_id;
    // Identity of the exact title/profile that owns this save ABI. It is
    // carried in the atomically committed volume and makes records from a
    // different executable/content identity incompatible rather than silently
    // readable at the same host endpoint.
    std::string_view profile_identity;
    std::span<const NativePortSaveUnitConfig> units;
};

enum class NativePortSaveOperation : std::uint8_t {
    Query,
    List,
    Read,
    Write,
    Remove,
};

enum class NativePortSaveError : std::uint8_t {
    None,
    Absent,
    NotFound,
    AlreadyExists,
    InvalidArgument,
    ReadOnly,
    InsufficientBlocks,
    DirectoryFull,
    BufferTooSmall,
    GenerationConflict,
    Corrupt,
    Incompatible,
    ResourceLimit,
    PlatformFailure,
};

struct NativePortSaveCompletion final {
    NativePortSaveOperation operation = NativePortSaveOperation::Query;
    NativePortSaveError error = NativePortSaveError::None;
    // Strictly increasing for every provider call, including an absent medium
    // or a rejected request. Adapters can bind this to their callback/completion
    // ordering without observing host I/O internals.
    std::uint64_t completion_sequence = 0u;
    std::uint64_t generation = 0u;
    std::uint64_t transferred_bytes = 0u;
    std::uint64_t required_bytes = 0u;
    std::uint32_t platform_error_code = 0u;
};

using NativePortSaveCompletionCallback =
    void (*)(void* user_data, const NativePortSaveCompletion& completion) noexcept;

struct NativePortSaveFileMetadata final {
    // File IDs are logical SDK names, never filesystem path fragments.
    std::string_view file_id;
    std::string_view application_id;
    std::string_view title;
    std::string_view description;
    std::uint32_t user_flags = 0u;
};

struct NativePortSaveDirectoryEntry final {
    std::string file_id;
    std::string application_id;
    std::string title;
    std::string description;
    std::uint32_t user_flags = 0u;
    std::uint32_t allocated_blocks = 0u;
    std::uint64_t byte_size = 0u;
};

struct NativePortSaveUnitStatus final {
    bool present = false;
    bool writable = false;
    std::uint32_t block_bytes = 0u;
    std::uint32_t block_count = 0u;
    std::uint32_t used_blocks = 0u;
    std::uint32_t free_blocks = 0u;
    std::uint32_t directory_entries = 0u;
    std::uint64_t generation = 0u;
};

struct NativePortSaveQueryRequest final {
    NativePortSaveEndpoint endpoint;
    NativePortSaveCompletionCallback completion = nullptr;
    void* completion_user_data = nullptr;
};

struct NativePortSaveListRequest final {
    NativePortSaveEndpoint endpoint;
    // Use an exact generation, including zero for an uncreated volume, to
    // obtain CAS semantics. The explicit `any` constant opts out.
    std::uint64_t expected_generation = native_port_save_any_generation;
    NativePortSaveCompletionCallback completion = nullptr;
    void* completion_user_data = nullptr;
};

struct NativePortSaveReadRequest final {
    NativePortSaveEndpoint endpoint;
    std::string_view file_id;
    std::span<std::byte> destination;
    std::uint64_t byte_offset = 0u;
    // Zero reads to EOF. A non-zero value must be wholly within the file; a
    // failed range check never changes destination RAM.
    std::uint64_t byte_count = 0u;
    std::uint64_t expected_generation = native_port_save_any_generation;
    NativePortSaveCompletionCallback completion = nullptr;
    void* completion_user_data = nullptr;
};

struct NativePortSaveWriteRequest final {
    NativePortSaveEndpoint endpoint;
    NativePortSaveFileMetadata metadata;
    std::span<const std::byte> payload;
    std::uint64_t expected_generation = native_port_save_any_generation;
    // `false` makes creation explicit and reports AlreadyExists when the
    // logical file is already in the directory.
    bool replace_existing = true;
    NativePortSaveCompletionCallback completion = nullptr;
    void* completion_user_data = nullptr;
};

struct NativePortSaveRemoveRequest final {
    NativePortSaveEndpoint endpoint;
    std::string_view file_id;
    std::uint64_t expected_generation = native_port_save_any_generation;
    NativePortSaveCompletionCallback completion = nullptr;
    void* completion_user_data = nullptr;
};

struct NativePortSaveQueryResult final {
    NativePortSaveCompletion completion;
    NativePortSaveUnitStatus status;
};

struct NativePortSaveListResult final {
    NativePortSaveCompletion completion;
    NativePortSaveUnitStatus status;
    std::vector<NativePortSaveDirectoryEntry> entries;
};

struct NativePortSaveReadResult final {
    NativePortSaveCompletion completion;
    NativePortSaveUnitStatus status;
};

struct NativePortSaveWriteResult final {
    NativePortSaveCompletion completion;
    NativePortSaveUnitStatus status;
};

struct NativePortSaveRemoveResult final {
    NativePortSaveCompletion completion;
    NativePortSaveUnitStatus status;
};

// Owner-thread confined, like NativePortPlatformServices. Every operation made
// on that owner thread produces exactly one completion. In particular, a
// caller's RAM destination is changed only after a complete, identity-bound
// volume has been validated and the result reports its transferred byte count.
// The provider has no addresses, callbacks or save formats specific to a title.
class NativePortSaveProvider final {
  public:
    NativePortSaveProvider(NativePortPlatformServices& platform,
                           const NativePortSaveProviderConfig& config);
    ~NativePortSaveProvider();

    NativePortSaveProvider(const NativePortSaveProvider&) = delete;
    NativePortSaveProvider& operator=(const NativePortSaveProvider&) = delete;
    NativePortSaveProvider(NativePortSaveProvider&&) = delete;
    NativePortSaveProvider& operator=(NativePortSaveProvider&&) = delete;

    [[nodiscard]] NativePortSaveQueryResult query(const NativePortSaveQueryRequest& request);
    [[nodiscard]] NativePortSaveListResult list(const NativePortSaveListRequest& request);
    [[nodiscard]] NativePortSaveReadResult read(const NativePortSaveReadRequest& request);
    [[nodiscard]] NativePortSaveWriteResult write(const NativePortSaveWriteRequest& request);
    [[nodiscard]] NativePortSaveRemoveResult remove(const NativePortSaveRemoveRequest& request);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::runtime
