#pragma once

#include "katana/runtime/block_abi.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace katana::runtime {

class ExecutableCodeTracker;
struct GuestWriteEvent;

using BackendBlockFunction = BlockExit (*)(CpuState&, BlockExecutionContext&);

struct BlockVariantKey {
    std::uint64_t address_space_generation = 0u;
    std::uint64_t mmu_generation = 0u;
    std::uint64_t watchpoint_generation = 0u;
    std::uint32_t fpscr_mode = 0u;
    std::uint64_t runtime_generation = 0u;

    [[nodiscard]] auto operator<=>(const BlockVariantKey&) const noexcept = default;
};

enum class StaticVariantPolicy : std::uint8_t {
    Exact,
    // The emitted native body reads architectural runtime state and is valid
    // for every direct P1/P2 address-space/MMU/watchpoint/FPSCR generation.
    // Runtime-generation remains exact because it denotes a different native
    // code universe rather than an architectural mode.
    DirectP1P2RuntimeStateAgnostic
};

// A byte range inside a native AOT template whose previous live value is
// semantically irrelevant because the proven native entry overwrites it before
// reading it. The generated block still performs the guest store and later live
// load, so intervening CPU/DMA writes retain their guest-visible data semantics;
// only code identity and code invalidation exclude these bytes. Ranges are
// relative to CodeAddressMapping::{source,runtime}_start.
struct NativeAotTemplateMutableRange {
    std::uint32_t offset = 0u;
    std::uint32_t size = 0u;

    [[nodiscard]] bool operator==(const NativeAotTemplateMutableRange&) const noexcept = default;
};

enum class NativeAotTemplateValidationMode : std::uint8_t {
    // Retain and track the complete source-backed template. This is the
    // conservative contract used by VBR copies and loaded disc modules.
    SourceModule,
    // Retain and track only the exact runtime block whose bytes were proven by
    // an export-time block identity. The surrounding runtime payload is not a
    // source-module dependency.
    RuntimeBlock
};

[[nodiscard]] bool native_aot_mutable_ranges_valid(
    std::span<const NativeAotTemplateMutableRange> ranges,
    std::uint32_t extent) noexcept;
[[nodiscard]] bool native_aot_offset_is_mutable(
    std::span<const NativeAotTemplateMutableRange> ranges,
    std::uint32_t offset) noexcept;
[[nodiscard]] bool native_aot_write_overlaps_immutable(
    std::uint32_t tracked_start,
    std::uint32_t tracked_extent,
    std::span<const NativeAotTemplateMutableRange> mutable_ranges,
    std::uint32_t write_start,
    std::size_t write_size) noexcept;

struct RuntimeAotTemplateContract {
    CodeAddressMapping mapping;
    // Validation starts at the physical byte corresponding to mapping.runtime_start
    // (derived from the block's physical origin).  It may be larger than the mapped
    // executable range so writes to an adjacent literal pool invalidate every native
    // block backed by the template as well.
    std::uint32_t validation_extent = 0u;
    std::vector<NativeAotTemplateMutableRange> mutable_ranges;
    NativeAotTemplateValidationMode validation_mode =
        NativeAotTemplateValidationMode::SourceModule;

    [[nodiscard]] bool operator==(const RuntimeAotTemplateContract&) const noexcept = default;
};

enum class RuntimeBlockFastpathKind : std::uint8_t {
    None,
    CompositeCallback,
    MemoryFill,
    MmioWait,
    CountedLoop
};

struct RuntimeBlockFastpathBinding {
    RuntimeBlockFastpathKind kind = RuntimeBlockFastpathKind::None;
    // The descriptor is immutable process-local data owned by the generated
    // product and must outlive the registered block.
    const void* descriptor = nullptr;
};

struct RuntimeBlock {
    std::uint32_t virtual_start = 0u;
    std::uint32_t physical_origin = 0u;
    std::uint32_t size = 0u;
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    BlockVariantKey variant;
    BackendBlockFunction function = nullptr;
    std::string provenance;
    bool runtime_registered = false;
    std::optional<RuntimeAotTemplateContract> aot_template;
    StaticVariantPolicy static_variant_policy = StaticVariantPolicy::Exact;
    RuntimeBlockFastpathBinding fastpath;
};

struct RuntimeBlockHandle {
    std::uint64_t id = 0u;
    std::uint64_t generation = 0u;

    [[nodiscard]] explicit operator bool() const noexcept {
        return id != 0u;
    }
    [[nodiscard]] auto operator<=>(const RuntimeBlockHandle&) const noexcept = default;
};

enum class BlockDispatchGenerationGuardKind : std::uint8_t {
    None,
    StaticAot,
    Materializer
};

struct BlockDispatchGenerationGuard {
    RuntimeBlockHandle block;
    BlockDispatchGenerationGuardKind kind = BlockDispatchGenerationGuardKind::None;
    std::uint64_t table_lifetime = 0u;
    std::uint64_t table_generation = 0u;
    std::uint64_t code_generation = 0u;
    std::uint64_t module_generation = 0u;
    std::uint64_t relocation_generation = 0u;
    std::uint64_t validation_generation = 0u;
    // One-based immutable index into the sealed Static-AOT entry array. The
    // table lifetime is checked before resolving this index.
    std::uint32_t static_entry_index = 0u;
    bool runtime_registered = false;

    [[nodiscard]] bool operator==(const BlockDispatchGenerationGuard&) const noexcept = default;
};

// An indirect-dispatch result is consumed immediately by the generated caller.
// The table owns the string/template storage referenced here. Dynamic results are
// reusable only while their generation guard remains current; static results
// remain valid until their sealed tier is invalidated.
struct ValidatedBlockExecution {
    RuntimeBlockHandle block;
    BackendBlockFunction function = nullptr;
    std::uint32_t virtual_start = 0u;
    std::uint32_t physical_origin = 0u;
    std::uint32_t size = 0u;
    BlockVariantKey variant;
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    bool runtime_registered = false;
    std::string_view provenance;
    const RuntimeAotTemplateContract* aot_template = nullptr;
    RuntimeBlockFastpathBinding fastpath;
    BlockDispatchGenerationGuard generation_guard;
    bool generation_guard_reusable = false;
};

enum class RuntimeBlockLookupMode : std::uint8_t { Direct, ReferenceTree };
enum class StaticAotInvalidationContract : std::uint8_t {
    Conservative,
    Coordinated
};
enum class RuntimeBlockDispatchState : std::uint8_t {
    StaticCompiled,
    RuntimeMaterialized,
    Rejected
};

struct RuntimeBlockDispatchStatus {
    RuntimeBlockDispatchState state = RuntimeBlockDispatchState::Rejected;
    std::uint64_t generation = 0u;
    std::optional<RuntimeBlockHandle> handle;
};

struct RuntimeBlockLookupCounters {
    std::uint64_t direct_probes = 0u;
    std::uint64_t reference_probes = 0u;

    [[nodiscard]] bool operator==(const RuntimeBlockLookupCounters&) const = default;
};

struct RuntimeBlockRecordSnapshot {
    RuntimeBlockHandle handle;
    std::uint32_t virtual_start = 0u;
    std::uint32_t physical_origin = 0u;
    std::uint32_t size = 0u;
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    BlockVariantKey variant;
    StaticVariantPolicy static_variant_policy = StaticVariantPolicy::Exact;
    std::string identity;
    std::string provenance;
    bool runtime_registered = false;
    bool active = false;
    bool static_block = false;
    std::optional<RuntimeAotTemplateContract> aot_template;

    [[nodiscard]] bool operator==(const RuntimeBlockRecordSnapshot&) const = default;
};

struct RuntimeBlockRejectionSnapshot {
    std::uint32_t virtual_address = 0u;
    BlockVariantKey variant;
    std::uint64_t generation = 0u;

    [[nodiscard]] bool operator==(const RuntimeBlockRejectionSnapshot&) const = default;
};

struct RuntimeBlockTableSnapshot {
    std::vector<RuntimeBlockRecordSnapshot> records;
    std::vector<RuntimeBlockRejectionSnapshot> rejected;
    std::uint64_t next_id = 1u;
    std::size_t active_count = 0u;
    bool static_sealed = false;
    bool code_tracker_bound = false;
    RuntimeBlockLookupMode lookup_mode = RuntimeBlockLookupMode::Direct;
    RuntimeBlockLookupCounters lookup_counters;

    [[nodiscard]] bool operator==(const RuntimeBlockTableSnapshot&) const = default;
};

[[nodiscard]] std::uint32_t canonical_physical_address(std::uint32_t address) noexcept;
// A direct P1/P2 block must retain the same linear virtual-to-physical alias
// mapping for every halfword, not merely at its entry address.
[[nodiscard]] bool direct_p1_p2_block_binding_contiguous(
    std::uint32_t virtual_start,
    std::uint32_t physical_origin,
    std::uint32_t size) noexcept;
[[nodiscard]] std::string stable_runtime_block_identity(const RuntimeBlock& block);
[[nodiscard]] BlockExit
execute_runtime_block(const RuntimeBlock& block, CpuState& cpu, BlockExecutionContext& context);
[[nodiscard]] BlockExit execute_runtime_block(const ValidatedBlockExecution& block,
                                              CpuState& cpu,
                                              BlockExecutionContext& context);

// RuntimeBlockTable is confined to its owning guest execution thread. Device
// notifications must be delivered on that thread before dispatch resumes.
class RuntimeBlockTable {
  public:
    RuntimeBlockTable() noexcept;
    RuntimeBlockTable(const RuntimeBlockTable&) = delete;
    RuntimeBlockTable& operator=(const RuntimeBlockTable&) = delete;
    RuntimeBlockTable(RuntimeBlockTable&&) = delete;
    RuntimeBlockTable& operator=(RuntimeBlockTable&&) = delete;

    [[nodiscard]] RuntimeBlockHandle register_static(RuntimeBlock block);
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    register_static_variant(std::uint32_t virtual_address,
                            std::uint32_t physical_address,
                            const BlockVariantKey& source_variant,
                            const BlockVariantKey& target_variant);
    [[nodiscard]] std::vector<RuntimeBlockHandle>
    register_static_bulk(std::vector<RuntimeBlock> blocks);
    // Contextual entry blocks may share instruction bytes (for example when a
    // branch target is also its owner's delay slot). Shared virtual bytes must
    // describe the same physical mapping and every dispatch start stays unique.
    [[nodiscard]] std::vector<RuntimeBlockHandle>
    register_static_contextual_bulk(std::vector<RuntimeBlock> blocks);
    void seal_static();
    [[nodiscard]] RuntimeBlockHandle register_bootstrap_static(RuntimeBlock block);
    [[nodiscard]] RuntimeBlockHandle register_runtime(RuntimeBlock block);
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup(std::uint32_t virtual_address, const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_physical(std::uint32_t physical_address, const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::optional<ValidatedBlockExecution>
    lookup_static_aot(std::uint32_t physical_address,
                      std::uint32_t virtual_address,
                      const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::vector<RuntimeBlockHandle> aliases(std::uint32_t physical_origin) const;
    [[nodiscard]] std::optional<std::reference_wrapper<const RuntimeBlock>>
    resolve(RuntimeBlockHandle handle) const noexcept;
    [[nodiscard]] bool active(RuntimeBlockHandle handle) const noexcept;
    [[nodiscard]] RuntimeBlockDispatchStatus
    dispatch_status(std::uint32_t virtual_address, const BlockVariantKey& variant) const noexcept;
    void mark_rejected(std::uint32_t virtual_address,
                       const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] RuntimeBlockLookupMode lookup_mode() const noexcept;
    void set_lookup_mode(RuntimeBlockLookupMode mode) noexcept;
    [[nodiscard]] const RuntimeBlockLookupCounters& lookup_counters() const noexcept;
    [[nodiscard]] std::uint64_t dispatch_lifetime() const noexcept;
    [[nodiscard]] std::uint64_t dispatch_generation() const noexcept;
    [[nodiscard]] bool static_aot_dispatch_ready() const noexcept;
    [[nodiscard]] bool static_dispatch_generation_guard_current(
        const BlockDispatchGenerationGuard& guard) const noexcept;
    [[nodiscard]] RuntimeBlockTableSnapshot snapshot() const;
    void reset_lookup_counters() const noexcept;
    [[nodiscard]] bool erase_identity(const std::string& block_identity) noexcept;
    // Valid linear ranges are checked exactly against active validation extents after the
    // physical-page index narrows the candidates. Nonempty overflow or alias-boundary ranges
    // conservatively return true.
    [[nodiscard]] bool may_overlap_active_physical(std::uint32_t address,
                                                   std::size_t size) const noexcept;
    [[nodiscard]] std::size_t erase_overlapping_physical(std::uint32_t physical_address,
                                                         std::size_t size) noexcept;
    // Allocation-free companion for a direct-RAM store batch. Changed pages
    // narrow the search and each active block candidate is evaluated once for
    // the complete event span.
    [[nodiscard]] std::size_t erase_overlapping_physical_batch(
        std::span<const GuestWriteEvent> events) noexcept;
    void bind_code_tracker(
        const ExecutableCodeTracker* tracker,
        StaticAotInvalidationContract static_aot_invalidation =
            StaticAotInvalidationContract::Conservative) noexcept;
    void clear() noexcept;

    struct PreparedDiscLoadInvalidation {
        std::vector<std::uint64_t> ids;
    };
    [[nodiscard]] PreparedDiscLoadInvalidation
    prepare_disc_load_invalidation(std::uint32_t physical_address, std::size_t size) const;
    [[nodiscard]] std::size_t
    commit_disc_load_invalidation(PreparedDiscLoadInvalidation plan) noexcept;

  private:
    friend class ExecutableDiscLoadTransactionCoordinator;
    struct VariantAddressKey {
        BlockVariantKey variant;
        std::uint32_t address = 0u;
        [[nodiscard]] auto operator<=>(const VariantAddressKey&) const noexcept = default;
    };
    struct VariantAddressHash {
        [[nodiscard]] std::size_t operator()(const VariantAddressKey& key) const noexcept;
    };
    struct PhysicalLookupKey {
        BlockVariantKey variant;
        std::uint32_t physical = 0u;
        std::uint32_t virtual_start = 0u;
        [[nodiscard]] auto operator<=>(const PhysicalLookupKey&) const noexcept = default;
    };
    struct Record {
        RuntimeBlock block;
        std::string identity;
        std::uint64_t generation = 1u;
        std::uint64_t write_batch_visit_epoch = 0u;
        bool active = true;
        bool static_block = false;
    };
    static constexpr std::size_t static_aot_halfwords_per_page = 2048u;
    static constexpr std::size_t static_aot_page_count = 1u << 17u;
    struct StaticAotEntry {
        const Record* record = nullptr;
        std::uint64_t id = 0u;
        std::uint64_t generation = 1u;
    };
    struct StaticAotPage {
        // Zero is empty, UINT32_MAX is ambiguous, every other value is a
        // one-based index into static_aot_entries_.
        std::array<std::uint32_t, static_aot_halfwords_per_page> entries{};
        // Dynamic/runtime entries shadow only their exact physical halfword;
        // unrelated runtime code on the same 4-KiB page must not disable the
        // entire immutable tier.
        // Runtime blocks are rare. Keep the immutable empty-slot lookup cheap
        // and allocate a shadow list only for an actually occupied halfword.
        std::array<std::unique_ptr<std::vector<const Record*>>,
                   static_aot_halfwords_per_page>
            dynamic_entries{};
    };

    using VirtualIndex = std::map<VariantAddressKey, std::uint64_t>;
    using DirectVirtualIndex =
        std::unordered_map<VariantAddressKey, std::uint64_t, VariantAddressHash>;
    using PhysicalIndex = std::map<PhysicalLookupKey, std::uint64_t>;
    using AliasIndex = std::map<std::uint32_t, std::set<std::uint64_t>>;

    [[nodiscard]] RuntimeBlockHandle
    insert(RuntimeBlock block, bool runtime_registered, bool allow_contextual_overlap = false);
    [[nodiscard]] bool dispatchable(const Record& record) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    overlapping_active_virtual(const RuntimeBlock& block,
                               std::uint64_t ignored_id = 0u) const noexcept;
    void index_active(std::uint64_t id, const Record& record);
    void rebuild_static_aot_index();
    void deactivate(std::uint64_t id) noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_index(const VirtualIndex& index,
                 std::uint32_t virtual_address,
                 const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_direct_index(const DirectVirtualIndex& index,
                        std::uint32_t virtual_address,
                        const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_physical_index(const PhysicalIndex& index,
                          std::uint32_t physical_address,
                          const BlockVariantKey& variant) const noexcept;

    std::map<std::uint64_t, Record> records_;
    std::map<std::string, std::uint64_t> identities_;
    VirtualIndex active_virtual_ranges_;
    VirtualIndex static_virtual_index_;
    VirtualIndex dynamic_virtual_index_;
    DirectVirtualIndex static_direct_virtual_index_;
    DirectVirtualIndex dynamic_direct_virtual_index_;
    PhysicalIndex static_physical_index_;
    PhysicalIndex dynamic_physical_index_;
    AliasIndex static_alias_index_;
    AliasIndex dynamic_alias_index_;
    std::map<std::uint32_t, std::set<std::uint64_t>> active_physical_pages_;
    std::uint64_t next_id_ = 1u;
    std::size_t active_count_ = 0u;
    bool static_sealed_ = false;
    bool contextual_virtual_overlaps_ = false;
    const ExecutableCodeTracker* code_tracker_ = nullptr;
    RuntimeBlockLookupMode lookup_mode_ = RuntimeBlockLookupMode::Direct;
    mutable RuntimeBlockLookupCounters lookup_counters_;
    mutable std::map<VariantAddressKey, std::uint64_t> rejected_generations_;
    std::uint64_t dispatch_lifetime_ = 0u;
    std::uint64_t dispatch_generation_ = 1u;
    std::uint64_t write_batch_visit_epoch_ = 0u;
    StaticAotInvalidationContract static_aot_invalidation_ =
        StaticAotInvalidationContract::Conservative;
    std::vector<std::unique_ptr<StaticAotPage>> static_aot_pages_;
    std::vector<StaticAotEntry> static_aot_entries_;
};

} // namespace katana::runtime
