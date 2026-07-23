#pragma once

#include "katana/runtime/executable_modules.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

enum class DiscLoadRoute : std::uint8_t {
    BiosPio,
    BiosDma,
    BiosPioStream,
    BiosDmaStream,
    TaskfileDma,
    SystemBootstrap
};

struct DiscLoadSourceRange {
    // Unknown is intentional for transports whose command buffer no longer carries a proven
    // logical-disc position. It must never be guessed from a representation-specific source ID.
    bool known = false;
    std::uint64_t byte_offset = 0u;
    std::uint64_t byte_count = 0u;

    [[nodiscard]] bool operator==(const DiscLoadSourceRange&) const = default;
};

struct DiscLoadRequest {
    std::uint64_t sequence = 0u;
    DiscLoadRoute route = DiscLoadRoute::BiosPio;
    std::uint32_t guest_destination = 0u;
    std::uint32_t physical_destination = 0u;
    CodeWriteSource write_source = CodeWriteSource::Copy;
    std::string content_identity;
    std::string byte_identity;
    DiscLoadSourceRange source_range;
    std::span<const std::uint8_t> bytes;
    bool guest_translation_validated = false;
};

struct DiscLoadCommittedRange {
    std::uint32_t target_physical_address = 0u;
    std::uint32_t backing_physical_address = 0u;
    std::uint32_t source_offset = 0u;
    std::uint32_t size = 0u;
    std::string byte_identity;
    bool executable_backing = false;
    bool module_activated = false;
    bool exact_module_retained = false;
    bool source_range_known = false;
    std::uint64_t source_byte_offset = 0u;

    [[nodiscard]] bool operator==(const DiscLoadCommittedRange&) const = default;
};

struct DiscLoadCommit {
    std::uint64_t sequence = 0u;
    DiscLoadRoute route = DiscLoadRoute::BiosPio;
    std::uint32_t guest_destination = 0u;
    std::uint32_t physical_destination = 0u;
    CodeWriteSource write_source = CodeWriteSource::Copy;
    std::string content_identity;
    std::string byte_identity;
    DiscLoadSourceRange source_range;
    std::uint64_t committed_bytes = 0u;
    std::vector<DiscLoadCommittedRange> ranges;
    bool bytes_changed = false;
    bool identity_rebound = false;

    [[nodiscard]] bool operator==(const DiscLoadCommit&) const = default;
};

using DiscLoadTransactionExecutor = std::function<DiscLoadCommit(const DiscLoadRequest&)>;
using DiscLoadRangeResolver =
    std::function<std::vector<DiscLoadCommittedRange>(std::uint32_t, std::size_t)>;
using DiscLoadAdmissionObserver =
    std::function<void(const DiscLoadRequest&, std::span<const DiscLoadCommittedRange>)>;

// Exported latent-AOT metadata contains no path, file name or source bytes. The logical byte range
// is in the representation-independent cooked DiscSource address space bound by content_identity.
struct DiscLoadAotModuleDescriptor {
    std::string opaque_id;
    std::string content_identity;
    std::uint64_t source_byte_offset = 0u;
    std::uint32_t byte_size = 0u;
    std::string byte_identity;

    [[nodiscard]] bool operator==(const DiscLoadAotModuleDescriptor&) const = default;
};

[[nodiscard]] const char* disc_load_route_name(DiscLoadRoute route) noexcept;
// Claims the current non-zero sequence and advances without ever wrapping the allocator.
[[nodiscard]] std::uint64_t claim_disc_load_sequence(std::uint64_t& next_sequence);
[[nodiscard]] std::string disc_load_byte_identity(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string disc_load_source_identity(std::string_view content_identity,
                                                    std::string_view byte_identity);
void validate_disc_load_commit(const DiscLoadRequest& request, const DiscLoadCommit& commit);

// The coordinator admits a complete transfer before touching RAM. Every target extent must be a
// writable linear backing (never MMIO), and all executable-module publications are structurally
// prepared as one batch. The caller's Memory guest-write observer must call consume_guest_write()
// first and skip its ordinary code invalidation when it returns true; the coordinator then applies
// exactly one provenance/invalidation/module commit for the complete transfer.
class ExecutableDiscLoadTransactionCoordinator final {
  public:
    ExecutableDiscLoadTransactionCoordinator(Memory& memory,
                                             ExecutableModuleCatalog& modules,
                                             RuntimeBlockTable& blocks,
                                             ExecutableCodeTracker& tracker,
                                             DiscLoadRangeResolver range_resolver,
                                             DiscLoadAdmissionObserver admission_observer = {});

    // This bounded registry may be installed once, before the first transaction. Partial registered
    // ranges publish only non-promotable RuntimeMaterializable candidates, so an attempted dispatch
    // fails through the native binder as MissingAot. A complete contiguous target replaces them only
    // after its full bytes match byte_identity.
    void set_aot_module_descriptors(std::span<const DiscLoadAotModuleDescriptor> descriptors);
    [[nodiscard]] DiscLoadCommit execute(const DiscLoadRequest& request);
    [[nodiscard]] bool consume_guest_write(const GuestWriteEvent& event) noexcept;
    [[nodiscard]] bool transaction_active() const noexcept;

  private:
    struct PendingWrite {
        std::uint32_t physical_address = 0u;
        std::size_t size = 0u;
        CodeWriteSource source = CodeWriteSource::Copy;
        bool active = false;
    };
    struct AotCoverageInterval {
        std::uint32_t begin = 0u;
        std::uint32_t end = 0u;
    };
    struct AotAssembly {
        DiscLoadAotModuleDescriptor descriptor;
        std::optional<std::uint32_t> target_base;
        std::vector<AotCoverageInterval> coverage;
    };

    Memory& memory_;
    ExecutableModuleCatalog& modules_;
    RuntimeBlockTable& blocks_;
    ExecutableCodeTracker& tracker_;
    DiscLoadRangeResolver range_resolver_;
    DiscLoadAdmissionObserver admission_observer_;
    PendingWrite pending_write_;
    std::vector<AotAssembly> aot_assemblies_;
    bool aot_descriptors_configured_ = false;
    bool transaction_started_ = false;
};

} // namespace katana::runtime
