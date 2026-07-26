#include "katana/build_contract.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/native_aot_profile.hpp"
#include "katana/runtime/abi.hpp"
#include "katana/runtime/block_table.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/memory.hpp"
#include "katana/runtime/platform_services.hpp"
#include "katana/testing/sh4_sst.hpp"
#include "katana/testing/sh4_sst_generated.hpp"
#include "katana/testing/sh4_sst_harness.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef KATANA_SH4_SST_WAIVERS_FILE
#define KATANA_SH4_SST_WAIVERS_FILE "tests/sh4_sst/waivers.json"
#endif

#ifndef KATANA_SH4_SST_DEFAULT_CORPUS_ROOT
#define KATANA_SH4_SST_DEFAULT_CORPUS_ROOT ""
#endif

#ifndef KATANA_SH4_SST_BUILD_TYPE
#if defined(NDEBUG)
#define KATANA_SH4_SST_BUILD_TYPE "Release-compatible"
#else
#define KATANA_SH4_SST_BUILD_TYPE "Debug-compatible"
#endif
#endif

#ifndef KATANA_SH4_SST_LTO
#define KATANA_SH4_SST_LTO 0
#endif

namespace {

namespace codegen = katana::codegen;
namespace runtime = katana::runtime;
namespace sst = katana::testing;
namespace sh4_sst = katana::testing::sh4_sst;

constexpr std::size_t maximum_counterexamples = 32u;
constexpr std::uint64_t address_space_size = 0x100000000ull;

struct Options {
    std::filesystem::path corpus_root{KATANA_SH4_SST_DEFAULT_CORPUS_ROOT};
    std::optional<std::string> filename;
    std::optional<std::uint32_t> case_index;
    std::optional<std::uint16_t> opcode;
    std::optional<std::string> family;
    sst::MemoryProfile memory_profile = sst::MemoryProfile::FlatSemanticMemory;
    sst::FpuComparisonMode fpu_comparison = sst::FpuComparisonMode::Strict;
    std::optional<std::size_t> shard;
    std::optional<std::size_t> shard_count;
    std::optional<std::filesystem::path> report_json;
    bool fail_fast = false;
};

struct CaseOutcome {
    sst::ResultClassification classification = sst::ResultClassification::Pass;
    std::string detail;
    sst::SstState actual_state;
    sst::SstStateComparison state_comparison;
    sst::SstStateComparison internal_comparison;
    std::vector<sst::SstMemoryObservation> expected_memory;
    std::vector<sst::SstMemoryObservation> actual_memory;
};

[[noreturn]] void usage_error(const std::string& message) {
    throw std::invalid_argument(message +
                                "\nUsage: katana-sh4-sst-runner --corpus-root PATH "
                                "[--file NAME] [--case INDEX] [--opcode HEX] [--family ID] "
                                "[--profile native-product-memory|flat-semantic-memory] "
                                "[--fpu strict|upstream-compatible] "
                                "[--shard INDEX --shard-count COUNT] [--fail-fast] "
                                "[--report-json PATH]");
}

std::string option_value(const int argc, char** argv, int& index, const std::string_view name) {
    const std::string_view argument(argv[index]);
    const auto prefix = std::string(name) + "=";
    if (argument.starts_with(prefix)) return std::string(argument.substr(prefix.size()));
    if (argument != name) return {};
    if (index + 1 >= argc) usage_error(std::string(name) + " requires a value");
    return argv[++index];
}

template <typename Integer>
Integer parse_integer(const std::string_view text, const int base, const std::string_view option) {
    Integer value{};
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, value, base);
    if (text.empty() || error != std::errc{} || end != last)
        usage_error(std::string(option) + " has an invalid integer value");
    return value;
}

std::uint16_t parse_opcode(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2u);
    const auto value = parse_integer<std::uint32_t>(text, 16, "--opcode");
    if (value > std::numeric_limits<std::uint16_t>::max())
        usage_error("--opcode must fit in 16 bits");
    return static_cast<std::uint16_t>(value);
}

Options parse_options(const int argc, char** argv) {
    Options result;
    bool corpus_seen = false;
    bool file_seen = false;
    bool case_seen = false;
    bool opcode_seen = false;
    bool family_seen = false;
    bool profile_seen = false;
    bool fpu_seen = false;
    bool shard_seen = false;
    bool shard_count_seen = false;
    bool report_seen = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: katana-sh4-sst-runner --corpus-root PATH "
                         "[--file NAME] [--case INDEX] [--opcode HEX] [--family ID] "
                         "[--profile native-product-memory|flat-semantic-memory] "
                         "[--fpu strict|upstream-compatible] "
                         "[--shard INDEX --shard-count COUNT] [--fail-fast] "
                         "[--report-json PATH]\n";
            std::exit(EXIT_SUCCESS);
        }
        if (argument == "--fail-fast") {
            if (result.fail_fast) usage_error("--fail-fast was provided more than once");
            result.fail_fast = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--corpus-root"); !value.empty()) {
            if (corpus_seen) usage_error("--corpus-root was provided more than once");
            result.corpus_root = value;
            corpus_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--file"); !value.empty()) {
            if (file_seen) usage_error("--file was provided more than once");
            result.filename = value;
            file_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--case"); !value.empty()) {
            if (case_seen) usage_error("--case was provided more than once");
            result.case_index = parse_integer<std::uint32_t>(value, 10, "--case");
            case_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--opcode"); !value.empty()) {
            if (opcode_seen) usage_error("--opcode was provided more than once");
            result.opcode = parse_opcode(value);
            opcode_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--family"); !value.empty()) {
            if (family_seen) usage_error("--family was provided more than once");
            result.family = value;
            family_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--profile"); !value.empty()) {
            if (profile_seen) usage_error("--profile was provided more than once");
            if (value == "native-product-memory") {
                result.memory_profile = sst::MemoryProfile::NativeProductMemory;
            } else if (value == "flat-semantic-memory") {
                result.memory_profile = sst::MemoryProfile::FlatSemanticMemory;
            } else {
                usage_error("--profile must be native-product-memory or flat-semantic-memory");
            }
            profile_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--fpu"); !value.empty()) {
            if (fpu_seen) usage_error("--fpu was provided more than once");
            if (value == "strict") {
                result.fpu_comparison = sst::FpuComparisonMode::Strict;
            } else if (value == "upstream-compatible") {
                result.fpu_comparison = sst::FpuComparisonMode::UpstreamCompatible;
            } else {
                usage_error("--fpu must be strict or upstream-compatible");
            }
            fpu_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--shard"); !value.empty()) {
            if (shard_seen) usage_error("--shard was provided more than once");
            result.shard = parse_integer<std::size_t>(value, 10, "--shard");
            shard_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--shard-count"); !value.empty()) {
            if (shard_count_seen) usage_error("--shard-count was provided more than once");
            result.shard_count = parse_integer<std::size_t>(value, 10, "--shard-count");
            shard_count_seen = true;
            continue;
        }
        if (const auto value = option_value(argc, argv, index, "--report-json"); !value.empty()) {
            if (report_seen) usage_error("--report-json was provided more than once");
            result.report_json = std::filesystem::path(value);
            report_seen = true;
            continue;
        }
        usage_error("unknown argument: " + std::string(argument));
    }

    if (result.corpus_root.empty())
        usage_error("--corpus-root is required because the runner never downloads data");
    if (result.shard.has_value() != result.shard_count.has_value())
        usage_error("--shard and --shard-count must be provided together");
    if (result.shard_count && *result.shard_count == 0u)
        usage_error("--shard-count must be greater than zero");
    if (result.shard && result.shard_count && *result.shard >= *result.shard_count)
        usage_error("--shard must be smaller than --shard-count");
    return result;
}

std::string compiler_description() {
    std::ostringstream output;
#if defined(__clang__)
    output << "Clang " << __clang_version__;
#elif defined(_MSC_VER)
    output << "MSVC " << _MSC_VER;
#if defined(_MSC_FULL_VER)
    output << '.' << _MSC_FULL_VER;
#endif
#elif defined(__GNUC__)
    output << "GCC " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#else
    output << "unknown-cxx20-compiler";
#endif
    return output.str();
}

std::string host_platform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(__unix__)
    return "unix";
#else
    return "unknown";
#endif
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

bool is_failure(const sst::ResultClassification classification) noexcept {
    switch (classification) {
    case sst::ResultClassification::Pass:
    case sst::ResultClassification::NotApplicableReferenceAlignment:
    case sst::ResultClassification::NotApplicableReferenceException:
    case sst::ResultClassification::NotApplicableReferenceMmio:
    case sst::ResultClassification::NotApplicableReferenceKnownBug:
    case sst::ResultClassification::NotApplicableKatanaRestricted:
    case sst::ResultClassification::NotApplicableAccessShape:
        return false;
    default:
        return true;
    }
}

const sst::SstWaiver* find_waiver(const sst::SstWaiverFile& file,
                                  const std::string_view filename,
                                  const std::uint32_t case_index) noexcept {
    for (const auto& waiver : file.waivers) {
        if (waiver.filename != filename) continue;
        if (!waiver.case_indices.empty() &&
            std::binary_search(waiver.case_indices.begin(), waiver.case_indices.end(), case_index))
            return &waiver;
        if (waiver.case_range && case_index >= waiver.case_range->first &&
            case_index <= waiver.case_range->last)
            return &waiver;
    }
    return nullptr;
}

sh4_sst::Applicability classify_runner_applicability(const sst::SstTestCase& test,
                                                     const bool family_supported,
                                                     const sst::MemoryProfile profile) {
    // Every native instruction entry is halfword aligned. Check all four oracle
    // addresses before creating mappings or putting a block into RuntimeBlockTable.
    for (std::size_t index = 0u; index < test.cycles.size(); ++index) {
        if (!test.cycles[index].has_fetch()) {
            return {false,
                    sst::ResultClassification::CorpusInvalid,
                    "cycle " + std::to_string(index) + " has no fetch oracle"};
        }
        if ((test.cycles[index].fetch_address & 1u) != 0u) {
            return {false,
                    sst::ResultClassification::NotApplicableReferenceAlignment,
                    "fetch oracle " + std::to_string(index) + " is not 2-byte aligned"};
        }
    }
    if ((test.initial.pc & 1u) != 0u) {
        return {false,
                sst::ResultClassification::NotApplicableReferenceAlignment,
                "initial PC is not 2-byte aligned"};
    }

    for (const auto& cycle : test.cycles) {
        const auto overflows =
            [](const bool active, const std::uint32_t address, const std::uint32_t width) {
                return active && static_cast<std::uint64_t>(address) + width > address_space_size;
            };
        if (overflows(cycle.has_read(), cycle.read_address, cycle.read_size) ||
            overflows(cycle.has_write(), cycle.write_address, cycle.write_size)) {
            return {false,
                    sst::ResultClassification::NotApplicableReferenceException,
                    "reference access crosses the 32-bit address-space boundary"};
        }
    }

    const auto base = sh4_sst::classify_case_applicability(test, family_supported, profile);
    if (!base.applicable || profile != sst::MemoryProfile::FlatSemanticMemory) return base;

    struct AccessRange {
        std::uint32_t address = 0u;
        std::uint32_t width = 0u;
    };
    std::vector<AccessRange> accesses;
    accesses.reserve(test.cycles.size() * 2u);
    for (const auto& cycle : test.cycles) {
        if (cycle.has_read()) accesses.push_back({cycle.read_address, cycle.read_size});
        if (cycle.has_write()) accesses.push_back({cycle.write_address, cycle.write_size});
    }
    for (std::size_t left = 0u; left < accesses.size(); ++left) {
        for (std::size_t right = left + 1u; right < accesses.size(); ++right) {
            for (std::uint32_t left_byte = 0u; left_byte < accesses[left].width; ++left_byte) {
                for (std::uint32_t right_byte = 0u; right_byte < accesses[right].width;
                     ++right_byte) {
                    const auto left_virtual = accesses[left].address + left_byte;
                    const auto right_virtual = accesses[right].address + right_byte;
                    if (left_virtual == right_virtual) continue;
                    if (runtime::canonical_physical_address(left_virtual) ==
                        runtime::canonical_physical_address(right_virtual)) {
                        // Katana's genuine no-MMU guest path intentionally aliases
                        // P0/P1/P2/P3. flat-semantic-memory is not alias evidence, so
                        // reject a vector as soon as that projection could be observed
                        // instead of pretending the aliases are independent.
                        return {false,
                                sst::ResultClassification::NotApplicableAccessShape,
                                "vector observes distinct 32-bit virtual aliases which "
                                "Katana's guest path projects to one physical byte"};
                    }
                }
            }
        }
    }
    return base;
}

class SparseMemoryDevice final : public runtime::MemoryDevice {
  public:
    explicit SparseMemoryDevice(const std::size_t logical_size) : logical_size_(logical_size) {
        if (logical_size_ == 0u || static_cast<std::uint64_t>(logical_size_) > address_space_size)
            throw std::invalid_argument("invalid sparse SST memory size");
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return logical_size_;
    }

    [[nodiscard]] std::uint8_t read_u8(const std::uint32_t offset) const override {
        require_offset(offset, 1u);
        const auto found = pages_.find(offset >> page_shift);
        if (found == pages_.end()) return 0u;
        return (*found->second)[offset & page_mask];
    }

    [[nodiscard]] std::uint16_t read_u16(const std::uint32_t offset) const override {
        require_offset(offset, 2u);
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(read_u8(offset)) |
                                          (static_cast<std::uint16_t>(read_u8(offset + 1u)) << 8u));
    }

    [[nodiscard]] std::uint32_t read_u32(const std::uint32_t offset) const override {
        require_offset(offset, 4u);
        return static_cast<std::uint32_t>(read_u8(offset)) |
               (static_cast<std::uint32_t>(read_u8(offset + 1u)) << 8u) |
               (static_cast<std::uint32_t>(read_u8(offset + 2u)) << 16u) |
               (static_cast<std::uint32_t>(read_u8(offset + 3u)) << 24u);
    }

    void write_u8(const std::uint32_t offset, const std::uint8_t value) override {
        require_offset(offset, 1u);
        auto found = pages_.find(offset >> page_shift);
        if (found == pages_.end()) {
            if (value == 0u) return;
            auto page = std::make_unique<Page>();
            page->fill(0u);
            found = pages_.emplace(offset >> page_shift, std::move(page)).first;
        }
        (*found->second)[offset & page_mask] = value;
    }

    void write_u16(const std::uint32_t offset, const std::uint16_t value) override {
        require_offset(offset, 2u);
        write_u8(offset, static_cast<std::uint8_t>(value));
        write_u8(offset + 1u, static_cast<std::uint8_t>(value >> 8u));
    }

    void write_u32(const std::uint32_t offset, const std::uint32_t value) override {
        require_offset(offset, 4u);
        write_u8(offset, static_cast<std::uint8_t>(value));
        write_u8(offset + 1u, static_cast<std::uint8_t>(value >> 8u));
        write_u8(offset + 2u, static_cast<std::uint8_t>(value >> 16u));
        write_u8(offset + 3u, static_cast<std::uint8_t>(value >> 24u));
    }

    void clear() noexcept {
        pages_.clear();
    }

  private:
    static constexpr std::uint32_t page_shift = 12u;
    static constexpr std::uint32_t page_size = 1u << page_shift;
    static constexpr std::uint32_t page_mask = page_size - 1u;
    using Page = std::array<std::uint8_t, page_size>;

    void require_offset(const std::uint32_t offset, const std::size_t width) const {
        if (static_cast<std::uint64_t>(offset) + width > static_cast<std::uint64_t>(logical_size_))
            throw std::out_of_range("sparse SST memory access is outside its device");
    }

    std::size_t logical_size_ = 0u;
    std::unordered_map<std::uint32_t, std::unique_ptr<Page>> pages_;
};

class SstPlatformServices final : public runtime::PlatformServices {
  public:
    SstPlatformServices(runtime::CpuState& cpu, const std::uint64_t initial_cycle)
        : cpu_(cpu), scheduler_cycle_(initial_cycle) {}

    [[nodiscard]] std::string_view name() const noexcept override {
        return "sh4-sst-native-aot";
    }

    [[nodiscard]] std::uint32_t abi_version() const noexcept override {
        return runtime::platform_services_abi_version;
    }

    [[nodiscard]] std::uint32_t guest_cycle_contract() const noexcept override {
        return runtime::guest_cycle_contract_version;
    }

    [[nodiscard]] runtime::PlatformCapabilities capabilities() const noexcept override {
        return runtime::platform_capability(runtime::PlatformCapability::Memory) |
               runtime::platform_capability(runtime::PlatformCapability::Scheduler) |
               runtime::platform_capability(runtime::PlatformCapability::Interrupts);
    }

    void read_memory(const std::uint32_t address,
                     const std::span<std::uint8_t> destination) override {
        for (std::size_t index = 0u; index < destination.size(); ++index) {
            destination[index] =
                runtime::guest_read_u8(cpu_, address + static_cast<std::uint32_t>(index));
        }
    }

    void write_memory(const std::uint32_t address,
                      const std::span<const std::uint8_t> source) override {
        for (std::size_t index = 0u; index < source.size(); ++index) {
            runtime::guest_write_u8(
                cpu_, address + static_cast<std::uint32_t>(index), source[index]);
        }
    }

    [[nodiscard]] std::uint64_t scheduler_cycle() const noexcept override {
        return scheduler_cycle_;
    }

    [[nodiscard]] std::optional<std::uint64_t>
    next_scheduler_event_cycle() const noexcept override {
        return std::nullopt;
    }

    [[nodiscard]] runtime::PlatformSchedulerResult
    consume_guest_cycles(const std::uint64_t guest_cycles, const std::size_t) override {
        scheduler_cycle_ += guest_cycles;
        return {scheduler_cycle_, 0u, false, false};
    }

    [[nodiscard]] std::optional<runtime::PlatformInterruptRequest> poll_interrupt() override {
        return std::nullopt;
    }

    [[nodiscard]] runtime::PlatformDmaResult
    start_dma(const runtime::PlatformDmaRequest&) override {
        throw std::runtime_error("DMA is forbidden in the SH-4 SST native runner");
    }

    [[nodiscard]] runtime::PlatformFallbackResult
    controlled_fallback(runtime::CpuState&, const runtime::PlatformFallbackRequest&) override {
        throw std::runtime_error("interpreter fallback is forbidden in the SH-4 SST native runner");
    }

    [[nodiscard]] bool prefetch(runtime::CpuState&,
                                const runtime::GuestInstructionOrigin,
                                const std::uint32_t) override {
        throw std::runtime_error(
            "PREF/store-queue services are outside the SH-4 SST supported gate");
    }

    [[nodiscard]] bool can_chain_executable_block(const std::uint32_t) const noexcept override {
        return false;
    }

  private:
    runtime::CpuState& cpu_;
    std::uint64_t scheduler_cycle_ = 0u;
};

class NativeTraceScope final {
  public:
    NativeTraceScope(sh4_sst::NativeExecutionTrace& trace,
                     runtime::PlatformServices& services) noexcept {
        sh4_sst::begin_native_execution_trace(trace, services);
    }
    ~NativeTraceScope() noexcept {
        sh4_sst::end_native_execution_trace();
    }

    NativeTraceScope(const NativeTraceScope&) = delete;
    NativeTraceScope& operator=(const NativeTraceScope&) = delete;
};

class GuestMemorySinkScope final {
  public:
    GuestMemorySinkScope(runtime::Memory& memory, sh4_sst::MemoryTraceRecorder& recorder) noexcept
        : memory_(memory) {
        memory_.set_guest_memory_access_sink(recorder.sink());
    }
    ~GuestMemorySinkScope() noexcept {
        memory_.clear_guest_memory_access_sink();
    }

    GuestMemorySinkScope(const GuestMemorySinkScope&) = delete;
    GuestMemorySinkScope& operator=(const GuestMemorySinkScope&) = delete;

  private:
    runtime::Memory& memory_;
};

class FlatPrivilegeScope final {
  public:
    FlatPrivilegeScope(runtime::CpuState& cpu, const bool enabled) noexcept
        : cpu_(cpu), enabled_(enabled), original_md_(cpu.sr & runtime::sr_md_mask) {
        if (enabled_) {
            // flat-semantic-memory is deliberately not runtime/privilege evidence. Set
            // raw SR.MD without write_sr(), so the SST architectural R/R_bank mapping
            // is not perturbed merely to let arbitrary 32-bit reference addresses use
            // Katana's real guest-memory path.
            cpu_.sr |= runtime::sr_md_mask;
        }
    }

    ~FlatPrivilegeScope() noexcept {
        if (!enabled_) return;
        cpu_.sr = (cpu_.sr & ~runtime::sr_md_mask) | original_md_;
    }

    FlatPrivilegeScope(const FlatPrivilegeScope&) = delete;
    FlatPrivilegeScope& operator=(const FlatPrivilegeScope&) = delete;

  private:
    runtime::CpuState& cpu_;
    bool enabled_ = false;
    std::uint32_t original_md_ = 0u;
};

bool needs_flat_privilege(const sst::SstTestCase& test, const sst::MemoryProfile profile) noexcept {
    if (profile != sst::MemoryProfile::FlatSemanticMemory) return false;
    for (const auto& cycle : test.cycles) {
        if ((cycle.has_read() && cycle.read_address >= 0x80000000u) ||
            (cycle.has_write() && cycle.write_address >= 0x80000000u))
            return true;
    }
    return false;
}

std::uint32_t physical_data_address(const std::uint32_t virtual_address) noexcept {
    return runtime::canonical_physical_address(virtual_address);
}

std::uint32_t device_offset(const std::uint32_t physical_address,
                            const sst::MemoryProfile profile) {
    if (profile == sst::MemoryProfile::FlatSemanticMemory) return physical_address;
    constexpr auto main_ram_base = runtime::dreamcast_main_ram_area_bases.front();
    constexpr auto main_ram_end =
        main_ram_base + static_cast<std::uint32_t>(runtime::dreamcast_main_ram_size *
                                                   runtime::dreamcast_main_ram_mirrors_per_area);
    if (physical_address < main_ram_base || physical_address >= main_ram_end)
        throw sst::SstHarnessInvalid(
            "native-product-memory setup escaped direct Dreamcast main RAM");
    return static_cast<std::uint32_t>((physical_address - main_ram_base) %
                                      runtime::dreamcast_main_ram_size);
}

void seed_expected_memory(const std::vector<sst::SstMemoryObservation>& observations,
                          SparseMemoryDevice& device,
                          const sst::MemoryProfile profile) {
    std::map<std::uint32_t, std::uint8_t> simulated;
    std::map<std::uint32_t, std::uint8_t> initial;

    for (const auto& observation : observations) {
        const auto physical = physical_data_address(observation.address);
        const auto offset = device_offset(physical, profile);
        for (std::uint32_t byte = 0u; byte < observation.width; ++byte) {
            const auto byte_offset = offset + byte;
            const auto byte_value =
                static_cast<std::uint8_t>(observation.value >> static_cast<unsigned>(byte * 8u));
            if (observation.operation == sst::SstMemoryOperation::Read) {
                const auto found = simulated.find(byte_offset);
                if (found == simulated.end()) {
                    simulated.emplace(byte_offset, byte_value);
                    initial.emplace(byte_offset, byte_value);
                } else if (found->second != byte_value) {
                    throw sst::SstHarnessInvalid(
                        "SST memory oracle is inconsistent after Katana address "
                        "translation at physical byte " +
                        hex32(physical + byte));
                }
            } else {
                simulated[byte_offset] = byte_value;
            }
        }
    }

    for (const auto [offset, value] : initial)
        device.write_u8(offset, value);
}

struct RuntimeSlotBinding {
    std::uint8_t slot = 0u;
    std::uint32_t canonical_address = 0u;
    std::uint32_t runtime_address = 0u;
};

std::vector<RuntimeSlotBinding>
materialize_runtime_slots(const sst::SstTestCase& test,
                          const sh4_sst::GeneratedFormDescriptor& descriptor) {
    constexpr std::uint8_t normal_slot_count = 4u;
    constexpr std::uint8_t maximum_slot_count = 8u;
    if (descriptor.canonical_slots.size() < normal_slot_count ||
        descriptor.canonical_slots.size() > maximum_slot_count)
        throw sst::SstHarnessInvalid("generated form has an invalid canonical slot count");

    std::array<std::optional<std::uint32_t>, maximum_slot_count> runtime_addresses{};
    std::array<const sh4_sst::GeneratedCanonicalSlotDescriptor*, maximum_slot_count>
        canonical_slots{};
    std::set<std::uint32_t> canonical_addresses;
    for (const auto& slot : descriptor.canonical_slots) {
        if (slot.slot >= maximum_slot_count || canonical_slots[slot.slot] != nullptr ||
            !canonical_addresses.insert(slot.canonical_address).second)
            throw sst::SstHarnessInvalid("generated form has duplicate or invalid canonical slots");
        canonical_slots[slot.slot] = &slot;
        if (slot.slot < normal_slot_count) {
            runtime_addresses[slot.slot] =
                test.initial.pc + static_cast<std::uint32_t>(slot.slot) * 2u;
        }
    }
    for (std::uint8_t slot = 0u; slot < normal_slot_count; ++slot) {
        if (canonical_slots[slot] == nullptr)
            throw sst::SstHarnessInvalid("generated form omits a normal canonical slot");
    }
    for (std::size_t slot = 0u; slot < maximum_slot_count; ++slot) {
        const bool expected = slot < descriptor.canonical_slots.size();
        if ((canonical_slots[slot] != nullptr) != expected)
            throw sst::SstHarnessInvalid("generated canonical slot indices are not contiguous");
    }

    for (std::size_t cycle = 0u; cycle < descriptor.fetch_slots.size(); ++cycle) {
        if (!test.cycles[cycle].has_fetch())
            throw sst::SstHarnessInvalid("runtime SST vector omits an expected instruction fetch");
        const auto slot = descriptor.fetch_slots[cycle];
        if (slot >= maximum_slot_count || canonical_slots[slot] == nullptr)
            throw sst::SstHarnessInvalid(
                "generated fetch shape references an absent canonical slot");
        const auto runtime_address = test.cycles[cycle].fetch_address;
        if (runtime_addresses[slot] && *runtime_addresses[slot] != runtime_address)
            throw sst::SstHarnessInvalid(
                "runtime corpus vector contradicts its generated fetch shape");
        runtime_addresses[slot] = runtime_address;
    }

    std::vector<RuntimeSlotBinding> result;
    result.reserve(descriptor.canonical_slots.size());
    for (const auto& slot : descriptor.canonical_slots) {
        if (!runtime_addresses[slot.slot])
            throw sst::SstHarnessInvalid("generated external slot has no observed runtime fetch");
        result.push_back({slot.slot, slot.canonical_address, *runtime_addresses[slot.slot]});
    }
    return result;
}

std::vector<runtime::CodeAddressMapping>
materialize_relocations(const sh4_sst::GeneratedFormDescriptor& descriptor,
                        const std::span<const RuntimeSlotBinding> slots) {
    std::map<std::uint32_t, std::uint32_t> source_to_runtime;
    std::map<std::uint32_t, std::uint32_t> runtime_to_source;
    for (const auto& recipe : descriptor.relocation_recipes) {
        const auto anchor = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
            return slot.slot == recipe.anchor_slot;
        });
        if (anchor == slots.end() || anchor->canonical_address + recipe.delta != recipe.source)
            throw sst::SstHarnessInvalid("generated relocation recipe has an invalid slot anchor");
        const auto runtime_address = anchor->runtime_address + recipe.delta;
        const auto [source, source_inserted] =
            source_to_runtime.emplace(recipe.source, runtime_address);
        if (!source_inserted && source->second != runtime_address)
            throw sst::SstHarnessInvalid(
                "generated relocation recipes disagree on one source address");
        const auto [runtime_mapping, runtime_inserted] =
            runtime_to_source.emplace(runtime_address, recipe.source);
        if (!runtime_inserted && runtime_mapping->second != recipe.source)
            throw sst::SstHarnessInvalid(
                "generated relocation recipes alias distinct source addresses");
    }
    if (source_to_runtime.empty())
        throw sst::SstHarnessInvalid("generated form contains no relocation recipes");

    std::vector<runtime::CodeAddressMapping> result;
    result.reserve(source_to_runtime.size());
    for (const auto& [source, runtime_address] : source_to_runtime)
        result.push_back({source, runtime_address, 1u});
    return result;
}

std::optional<std::uint32_t> runtime_block_start(const sh4_sst::GeneratedBlockDescriptor& block,
                                                 const std::span<const RuntimeSlotBinding> slots) {
    std::optional<std::uint32_t> result;
    const auto canonical_end = static_cast<std::uint64_t>(block.canonical_address) + block.size;
    for (const auto& slot : slots) {
        if (slot.canonical_address < block.canonical_address ||
            static_cast<std::uint64_t>(slot.canonical_address) >= canonical_end)
            continue;
        const auto delta = slot.canonical_address - block.canonical_address;
        const auto candidate = slot.runtime_address - delta;
        if (result && *result != candidate) {
            throw sst::SstHarnessInvalid(
                "one generated native block maps to inconsistent runtime starts");
        }
        result = candidate;
    }
    return result;
}

struct BoundBlocks {
    runtime::RuntimeBlockTable table;
    runtime::BlockVariantKey variant;
    std::vector<std::unique_ptr<runtime::ScopedCodeAddressMapping>> mappings;
};

BoundBlocks bind_generated_blocks(const sst::SstTestCase& test,
                                  const sh4_sst::GeneratedFormDescriptor& descriptor) {
    BoundBlocks result;
    result.variant = {};

    const auto slots = materialize_runtime_slots(test, descriptor);
    const auto mappings = materialize_relocations(descriptor, slots);
    result.mappings.reserve(mappings.size());
    for (const auto& mapping : mappings) {
        result.mappings.push_back(std::make_unique<runtime::ScopedCodeAddressMapping>(mapping));
    }

    std::vector<runtime::RuntimeBlock> blocks;
    blocks.reserve(descriptor.blocks.size());
    std::set<std::uint32_t> bound_addresses;
    for (const auto& generated : descriptor.blocks) {
        if (generated.function == nullptr || generated.size == 0u)
            throw sst::SstHarnessInvalid(
                "generated form contains an empty native block descriptor");
        const auto runtime_start = runtime_block_start(generated, slots);
        if (!runtime_start) {
            // Analysis-closure blocks without a materialized slot are
            // intentionally absent. Entry remains FailUnboundTarget.
            continue;
        }
        if (!bound_addresses.insert(*runtime_start).second)
            throw sst::SstHarnessInvalid(
                "generated form binds two native blocks to one runtime address");
        const auto remaining = address_space_size - static_cast<std::uint64_t>(*runtime_start);
        const auto bounded_size =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(generated.size, remaining));
        if (bounded_size == 0u)
            throw sst::SstHarnessInvalid("generated runtime block has zero extent");
        runtime::RuntimeBlock block;
        block.virtual_start = *runtime_start;
        block.physical_origin = runtime::canonical_physical_address(*runtime_start);
        block.size = bounded_size;
        block.end_kind = runtime::BlockEndKind::Fallthrough;
        block.variant = result.variant;
        block.function = generated.function;
        block.provenance =
            "sh4-sst-aot-" + std::string(descriptor.key) + "-" + hex32(*runtime_start);
        blocks.push_back(std::move(block));
    }
    if (blocks.empty())
        throw sst::SstHarnessInvalid(
            "supported generated form has no concrete native block binding");
    static_cast<void>(result.table.register_static_contextual_bulk(std::move(blocks)));
    return result;
}

std::optional<sst::ResultClassification> execute_bound_blocks(runtime::CpuState& cpu,
                                                              BoundBlocks& blocks,
                                                              runtime::PlatformServices& services,
                                                              sh4_sst::NativeExecutionTrace& trace,
                                                              std::string& failure_detail) {
    constexpr std::size_t maximum_dispatches = 8u;
    runtime::BlockExecutionContext context;
    context.scheduler_cycle = services.scheduler_cycle();
    context.scheduler_event_budget = runtime::guest_cycle_flush_event_budget;

    try {
        NativeTraceScope native_trace(trace, services);
        for (std::size_t dispatch = 0u;
             dispatch < maximum_dispatches && trace.instructions.size() < sst::sh4_sst_cycle_count;
             ++dispatch) {
            const auto handle = blocks.table.lookup(cpu.pc, blocks.variant);
            if (!handle) {
                blocks.table.mark_rejected(cpu.pc, blocks.variant);
                failure_detail = "native AOT target " + hex32(cpu.pc) + " is not pre-bound";
                return sst::ResultClassification::FailUnboundTarget;
            }
            const auto block = blocks.table.resolve(*handle);
            if (!block) {
                failure_detail = "native AOT block handle became invalid at " + hex32(cpu.pc);
                return sst::ResultClassification::HarnessInvalid;
            }
            static_cast<void>(runtime::execute_runtime_block(block->get(), cpu, context));
        }
        if (trace.instructions.size() < sst::sh4_sst_cycle_count) {
            failure_detail = "native AOT dispatch budget ended after " +
                             std::to_string(trace.instructions.size()) +
                             " instruction observations";
            return sst::ResultClassification::FailControlFlow;
        }
    } catch (const sh4_sst::AotDispatchError& error) {
        failure_detail = std::string(error.call() ? "unbound native call target "
                                                  : "unbound native jump target ") +
                         hex32(error.target());
        return sst::ResultClassification::FailUnboundTarget;
    } catch (const runtime::MemoryAccessError& error) {
        failure_detail = "unexpected Katana guest-memory exception at " + hex32(error.address()) +
                         ": " + error.what();
        return sst::ResultClassification::FailUnexpectedException;
    } catch (const std::exception& error) {
        failure_detail =
            std::string("unexpected exception from native AOT execution: ") + error.what();
        return sst::ResultClassification::FailUnexpectedException;
    } catch (...) {
        failure_detail = "unknown exception from native AOT execution";
        return sst::ResultClassification::FailUnexpectedException;
    }
    return std::nullopt;
}

CaseOutcome execute_case(const sst::SstTestCase& test,
                         const sh4_sst::GeneratedFormDescriptor& descriptor,
                         const sst::MemoryProfile profile,
                         const sst::FpuComparisonMode fpu_mode,
                         runtime::CpuState& cpu,
                         SparseMemoryDevice& memory_device) {
    CaseOutcome outcome;
    outcome.expected_memory = sh4_sst::expected_memory_observations(test);
    memory_device.clear();
    seed_expected_memory(outcome.expected_memory, memory_device, profile);

    cpu.memory.clear_guest_memory_access_sink();
    cpu.memory.clear_guest_write_observer();
    cpu.memory.clear_trace_handler();
    cpu.memory.clear_mmio_trace_handler();
    cpu.memory.set_mmio_access_tracking(false);
    cpu.address_space.reset();
    cpu.gdrom_services = nullptr;
    cpu.g1_bus = nullptr;

    const auto round_trip = sst::import_and_verify_sst_state(test.initial, cpu);
    sst::require_sst_setup_round_trip(round_trip);
    const auto before = sst::initialize_sst_internal_canaries(cpu);

    auto blocks = bind_generated_blocks(test, descriptor);
    sh4_sst::NativeExecutionTrace native_trace;
    sh4_sst::MemoryTraceRecorder memory_trace;
    SstPlatformServices services(cpu, before.total_guest_cycles);
    std::optional<sst::ResultClassification> execution_failure;
    std::string execution_failure_detail;
    const auto exception_generation_before = cpu.exception_generation;
    {
        FlatPrivilegeScope privilege(cpu, needs_flat_privilege(test, profile));
        GuestMemorySinkScope memory_sink(cpu.memory, memory_trace);
        execution_failure =
            execute_bound_blocks(cpu, blocks, services, native_trace, execution_failure_detail);
    }
    cpu.memory.clear_guest_memory_access_sink();

    if (!native_trace.complete())
        throw sst::SstInfrastructureError(
            "host allocation failed while recording native instruction observations");
    if (!memory_trace.complete())
        throw sst::SstInfrastructureError(
            "host allocation failed while recording guest-memory observations");
    if (cpu.exception_generation != exception_generation_before && !execution_failure) {
        execution_failure = sst::ResultClassification::FailUnexpectedException;
        execution_failure_detail = "native execution entered Katana exception cause " +
                                   std::to_string(static_cast<unsigned>(cpu.last_exception_cause));
    }

    outcome.actual_state = sst::export_sst_state(cpu);
    outcome.state_comparison = sst::compare_sst_states(test.final, outcome.actual_state, fpu_mode);
    const auto elapsed_before = before.total_guest_cycles + before.pending_guest_cycles;
    const auto elapsed_after = cpu.total_guest_cycles + cpu.pending_guest_cycles;
    const auto cycle_delta = elapsed_after >= elapsed_before ? elapsed_after - elapsed_before : 0u;
    outcome.internal_comparison = sst::compare_sst_internal_state_after_success(
        before,
        cpu,
        {cycle_delta,
         test.cycles.back().fetch_address,
         runtime::canonical_physical_address(test.cycles.back().fetch_address)});
    outcome.actual_memory = memory_trace.observations();

    sh4_sst::CaseComparisonInput comparison;
    comparison.test = &test;
    comparison.native_trace = &native_trace;
    comparison.actual_memory = &outcome.actual_memory;
    comparison.actual_state = outcome.actual_state;
    comparison.internal_state = outcome.internal_comparison;
    comparison.tested_has_delay_slot = descriptor.tested_has_delay_slot;
    comparison.fpu_comparison = fpu_mode;
    comparison.execution_failure = execution_failure;
    comparison.execution_failure_detail = execution_failure_detail;
    const auto compared = sh4_sst::compare_case_result(comparison);
    outcome.classification = compared.classification;
    outcome.detail = compared.detail;
    return outcome;
}

bool selected_by_filters(const Options& options,
                         const sh4_sst::GeneratedCaseDescriptor& selected,
                         const sh4_sst::GeneratedFormDescriptor& form,
                         const std::size_t ordinal) {
    if (options.shard && options.shard_count && ordinal % *options.shard_count != *options.shard)
        return false;
    if (options.filename && selected.filename != *options.filename) return false;
    if (options.case_index && selected.case_index != *options.case_index) return false;
    if (options.opcode && form.opcode != *options.opcode) return false;
    if (options.family && form.family != *options.family) return false;
    return true;
}

void increment_classification(std::map<sst::ResultClassification, std::uint64_t>& counts,
                              const sst::ResultClassification classification) {
    ++counts[classification];
}

void append_counterexample(sst::SstReportBasis& report,
                           const std::string_view filename,
                           const std::uint32_t case_index,
                           const std::uint16_t opcode,
                           const CaseOutcome& outcome) {
    if (report.first_counterexamples.size() >= maximum_counterexamples) return;
    sst::SstCounterexample counterexample;
    counterexample.filename = filename;
    counterexample.case_index = case_index;
    counterexample.opcode = opcode;
    counterexample.classification = outcome.classification;
    counterexample.state_differences = outcome.state_comparison.differences;
    counterexample.expected_memory = outcome.expected_memory;
    counterexample.actual_memory = outcome.actual_memory;
    counterexample.detail = outcome.detail;
    report.first_counterexamples.push_back(std::move(counterexample));
}

void replace_file_atomically(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination) {
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = static_cast<unsigned long>(GetLastError());
        throw std::runtime_error("cannot atomically replace SST report (Win32 error " +
                                 std::to_string(error) + ')');
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) throw std::runtime_error("cannot atomically replace SST report: " + error.message());
#endif
}

void write_report_atomically(const std::filesystem::path& path, const std::string_view json) {
    if (path.empty()) throw sst::SstInfrastructureError("empty --report-json destination");
    std::error_code error;
    const auto parent =
        path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
    std::filesystem::create_directories(parent, error);
    if (error)
        throw sst::SstInfrastructureError("cannot create SST report directory: " + error.message());

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(nonce);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw sst::SstInfrastructureError("cannot open temporary SST report: " +
                                              temporary.string());
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.put('\n');
        output.flush();
        if (!output)
            throw sst::SstInfrastructureError("cannot write temporary SST report: " +
                                              temporary.string());
    }
    try {
        replace_file_atomically(temporary, path);
    } catch (...) {
        std::filesystem::remove(temporary, error);
        throw;
    }
}

sst::SstReportBasis make_report_basis(const Options& options,
                                      const sst::SstManifest& manifest,
                                      const sst::SstWaiverFile& waivers) {
    sst::SstReportBasis report;
    report.katana_commit = std::string(katana::build_contract::katana_git_commit);
    report.corpus_manifest_sha256 = manifest.sha256;
    report.compiler = compiler_description();
    report.build_type = KATANA_SH4_SST_BUILD_TYPE;
    report.host_platform = host_platform();
    report.lto = KATANA_SH4_SST_LTO != 0;
    report.runtime_abi = runtime::abi_version;
    report.backend_abi = codegen::backend_interface_abi_version;
    // Keep the runner link closed over runtime_core + generated objects. The
    // profile name is a versioned evidence token, not a reason to link katana_core.
    report.backend_profile = "external-conformance";
    report.backend_profile_version = codegen::native_aot_emission_profile_version;
    report.generated_native_code_forms = static_cast<std::uint64_t>(std::count_if(
        sh4_sst::generated_forms().begin(), sh4_sst::generated_forms().end(), [](const auto& form) {
            return form.supported && !form.blocks.empty();
        }));
    report.scope = std::string(sh4_sst::generated_corpus_scope());
    report.memory_profile = options.memory_profile;
    report.fpu_comparison = options.fpu_comparison;
    report.selection.expected_scope_vectors =
        report.scope == "smoke" ? katana::sh4::external_evidence_contract::smoke_vector_count
                                : katana::sh4::external_evidence_contract::full_vector_count;
    report.selection.filename = options.filename;
    report.selection.case_index = options.case_index;
    report.selection.opcode = options.opcode;
    report.selection.family = options.family;
    report.selection.shard = options.shard;
    report.selection.shard_count = options.shard_count;
    report.selection.fail_fast = options.fail_fast;
    report.waivers = waivers.waivers;
    report.represented_opcodes.assign(sh4_sst::represented_external_opcodes().begin(),
                                      sh4_sst::represented_external_opcodes().end());
    report.katana_opcodes_without_external_evidence.assign(
        sh4_sst::unrepresented_katana_opcodes().begin(),
        sh4_sst::unrepresented_katana_opcodes().end());
    return report;
}

int run(const Options& options) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(options.corpus_root, filesystem_error) || filesystem_error)
        throw sst::SstInfrastructureError("SST corpus root is not a readable directory: " +
                                          options.corpus_root.string());

    const auto manifest = sst::calculate_sh4_sst_manifest(options.corpus_root);
    sst::require_pinned_sh4_sst_manifest(manifest);
    const auto waiver_path = std::filesystem::path(KATANA_SH4_SST_WAIVERS_FILE);
    const auto waivers = sst::parse_sh4_sst_waivers_file(waiver_path);
    for (const auto& waiver : waivers.waivers) {
        const auto declared_file =
            std::find_if(manifest.entries.begin(), manifest.entries.end(), [&](const auto& entry) {
                return entry.filename == waiver.filename;
            });
        if (declared_file == manifest.entries.end()) {
            throw sst::SstCorpusInvalid(
                "waiver references a filename absent from the pinned manifest", waiver.filename);
        }
    }
    auto report = make_report_basis(options, manifest, waivers);

    const auto generated_cases = sh4_sst::generated_cases();
    if (generated_cases.empty() || sh4_sst::generated_forms().empty())
        throw sst::SstHarnessInvalid("SST runner was linked without generated native forms/cases");
    if (generated_cases.size() != sh4_sst::generated_vector_count())
        throw sst::SstHarnessInvalid("generated SST case registry contradicts its vector count");

    static_assert(sizeof(std::size_t) >= sizeof(std::uint64_t),
                  "SH-4 SST sparse flat memory requires a 64-bit host");
    const auto device_size = options.memory_profile == sst::MemoryProfile::FlatSemanticMemory
                                 ? static_cast<std::size_t>(address_space_size)
                                 : runtime::dreamcast_main_ram_size;
    auto memory_device = std::make_shared<SparseMemoryDevice>(device_size);
    runtime::CpuState cpu;
    cpu.memory = runtime::Memory(0u, runtime::MemoryAlignmentPolicy::Strict);
    if (options.memory_profile == sst::MemoryProfile::FlatSemanticMemory) {
        cpu.memory.map_region("sh4-sst-flat-semantic", 0u, memory_device);
    } else {
        constexpr auto physical_main_ram_base = runtime::dreamcast_main_ram_area_bases.front();
        for (std::size_t mirror = 0u; mirror < runtime::dreamcast_main_ram_mirrors_per_area;
             ++mirror) {
            const auto base = physical_main_ram_base +
                              static_cast<std::uint32_t>(mirror * runtime::dreamcast_main_ram_size);
            cpu.memory.map_region("sh4-sst-native-product-main-ram-mirror-" +
                                      std::to_string(mirror),
                                  base,
                                  memory_device);
        }
    }

    std::map<sst::ResultClassification, std::uint64_t> classifications;
    std::set<std::string> used_files;
    std::optional<sst::SstCorpusFile> parsed_file;
    std::string parsed_filename;
    bool matched_any = false;
    bool stop = false;

    for (std::size_t ordinal = 0u; ordinal < generated_cases.size() && !stop; ++ordinal) {
        const auto& selected = generated_cases[ordinal];
        const auto* descriptor = sh4_sst::find_generated_form(selected.form_key);
        if (descriptor == nullptr)
            throw sst::SstHarnessInvalid(
                "generated case references an absent code-form descriptor");
        if (!selected_by_filters(options, selected, *descriptor, ordinal)) continue;
        matched_any = true;

        if (!parsed_file || parsed_filename != selected.filename) {
            parsed_file = sst::parse_sh4_sst_file(options.corpus_root /
                                                  std::filesystem::path(selected.filename));
            parsed_filename = selected.filename;
        }
        if (selected.case_index >= parsed_file->cases.size())
            throw sst::SstCorpusInvalid("generated case index is outside the pinned corpus file",
                                        std::string(selected.filename));
        const auto& test = parsed_file->cases[selected.case_index];
        if (descriptor->key != selected.form_key)
            throw sst::SstHarnessInvalid("generated case registry references the wrong AOT form");
        static_cast<void>(materialize_runtime_slots(test, *descriptor));
        if (descriptor->opcode != test.opcodes[1u])
            throw sst::SstHarnessInvalid(
                "generated AOT descriptor reports the wrong tested opcode");

        ++report.total_vectors;
        used_files.insert(std::string(selected.filename));

        if (const auto* waiver = find_waiver(waivers, selected.filename, selected.case_index)) {
            increment_classification(classifications, waiver->classification);
            continue;
        }

        const auto applicability =
            classify_runner_applicability(test, descriptor->supported, options.memory_profile);
        if (!applicability.applicable) {
            if (applicability.classification == sst::ResultClassification::CorpusInvalid) {
                throw sst::SstCorpusInvalid(applicability.detail, std::string(selected.filename));
            }
            if (applicability.classification == sst::ResultClassification::HarnessInvalid) {
                throw sst::SstHarnessInvalid(applicability.detail);
            }
            if (applicability.classification == sst::ResultClassification::InfrastructureError) {
                throw sst::SstInfrastructureError(applicability.detail,
                                                  std::string(selected.filename));
            }
            increment_classification(classifications, applicability.classification);
            if (is_failure(applicability.classification)) {
                ++report.applicable_vectors;
                ++report.failed_vectors;
                CaseOutcome outcome;
                outcome.classification = applicability.classification;
                outcome.detail = applicability.detail;
                append_counterexample(
                    report, selected.filename, selected.case_index, descriptor->opcode, outcome);
                stop = options.fail_fast;
            }
            continue;
        }

        ++report.applicable_vectors;
        CaseOutcome outcome;
        try {
            outcome = execute_case(test,
                                   *descriptor,
                                   options.memory_profile,
                                   options.fpu_comparison,
                                   cpu,
                                   *memory_device);
        } catch (const sst::SstError&) {
            // Corpus, harness, and infrastructure failures invalidate the run;
            // they are not per-vector semantic outcomes and must never be
            // diluted into the conformance denominator.
            throw;
        } catch (const std::exception& error) {
            throw sst::SstHarnessInvalid(std::string("unclassified runner exception: ") +
                                         error.what());
        } catch (...) {
            throw sst::SstHarnessInvalid("unclassified non-standard runner exception");
        }

        if (outcome.classification == sst::ResultClassification::CorpusInvalid)
            throw sst::SstCorpusInvalid(outcome.detail, std::string(selected.filename));
        if (outcome.classification == sst::ResultClassification::HarnessInvalid)
            throw sst::SstHarnessInvalid(outcome.detail);
        if (outcome.classification == sst::ResultClassification::InfrastructureError)
            throw sst::SstInfrastructureError(outcome.detail, std::string(selected.filename));
        increment_classification(classifications, outcome.classification);
        if (outcome.classification == sst::ResultClassification::Pass) {
            ++report.passed_vectors;
        } else if (is_failure(outcome.classification)) {
            ++report.failed_vectors;
            append_counterexample(
                report, selected.filename, selected.case_index, descriptor->opcode, outcome);
            stop = options.fail_fast;
        } else {
            // An execution-stage N/A is legal but no longer part of the applicable
            // denominator. Applicability is decided before native execution in normal
            // operation; this branch is defensive for typed harness extensions.
            --report.applicable_vectors;
        }
    }

    if (!matched_any)
        throw sst::SstHarnessInvalid(
            "the requested file/case/opcode/family/shard filters selected no "
            "generated vector");

    report.used_files.assign(used_files.begin(), used_files.end());
    report.selection.complete_scope =
        !options.filename && !options.case_index && !options.opcode && !options.family &&
        !options.shard && !options.shard_count && !options.fail_fast && !stop &&
        report.total_vectors == report.selection.expected_scope_vectors &&
        report.total_vectors == generated_cases.size();
    for (const auto [classification, count] : classifications)
        report.classifications.push_back({classification, count});
    if (!classifications.contains(sst::ResultClassification::Pass))
        report.classifications.push_back({sst::ResultClassification::Pass, 0u});

    const auto json = sst::format_sh4_sst_report_json(report);
    if (options.report_json) write_report_atomically(*options.report_json, json);

    std::cout << "SH-4 SST native AOT run: scope=" << report.scope
              << " profile=" << sst::memory_profile_name(report.memory_profile)
              << " fpu=" << sst::fpu_comparison_mode_name(report.fpu_comparison)
              << " total=" << report.total_vectors << " applicable=" << report.applicable_vectors
              << " pass=" << report.passed_vectors << " fail=" << report.failed_vectors << '\n';
    for (const auto& entry : report.classifications) {
        if (entry.count == 0u) continue;
        std::cout << "  " << sst::result_classification_name(entry.classification) << '='
                  << entry.count << '\n';
    }
    if (options.report_json) std::cout << "  report=" << options.report_json->string() << '\n';
    return report.failed_vectors == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(const int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const sst::SstError& error) {
        const auto classification =
            error.classification() == sst::ResultClassification::CorpusInvalid
                ? sst::ResultClassification::InfrastructureError
                : error.classification();
        std::cerr << "SH-4 SST " << sst::result_classification_name(classification) << ": "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    } catch (const std::invalid_argument& error) {
        std::cerr << "SH-4 SST harness-invalid: " << error.what() << '\n';
        return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "SH-4 SST infrastructure-error: " << error.what() << '\n';
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "SH-4 SST infrastructure-error: unknown non-standard exception\n";
        return EXIT_FAILURE;
    }
}
