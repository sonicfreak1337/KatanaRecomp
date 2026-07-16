#include "katana/io/elf32_sh_loader.hpp"
#include "katana/io/project_manifest.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/runtime/block_dispatch.hpp"
#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace {

enum class Target { Decoder, Loader, Ir, Runtime, All };

struct Options {
    Target target = Target::All;
    std::uint64_t seed = 0x3703u;
    std::size_t iterations = 256u;
    std::size_t max_input_size = 4096u;
};

class Random {
  public:
    explicit Random(const std::uint64_t seed) : state_(seed ^ 0x9E3779B97F4A7C15ull) {}

    std::uint64_t next() noexcept {
        state_ ^= state_ >> 12u;
        state_ ^= state_ << 25u;
        state_ ^= state_ >> 27u;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    std::size_t index(const std::size_t limit) noexcept {
        return limit == 0u ? 0u : static_cast<std::size_t>(next() % limit);
    }

  private:
    std::uint64_t state_;
};

void write_u16(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void write_u32(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

std::vector<std::uint8_t> minimal_elf_seed() {
    std::vector<std::uint8_t> bytes(86u, 0u);
    bytes[0] = 0x7Fu;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1u;
    bytes[5] = 1u;
    bytes[6] = 1u;
    write_u16(bytes, 16u, 2u);
    write_u16(bytes, 18u, 42u);
    write_u32(bytes, 20u, 1u);
    write_u32(bytes, 24u, 0x8C010000u);
    write_u32(bytes, 28u, 52u);
    write_u16(bytes, 40u, 52u);
    write_u16(bytes, 42u, 32u);
    write_u16(bytes, 44u, 1u);
    write_u32(bytes, 52u, 1u);
    write_u32(bytes, 56u, 84u);
    write_u32(bytes, 60u, 0x8C010000u);
    write_u32(bytes, 68u, 2u);
    write_u32(bytes, 72u, 2u);
    write_u32(bytes, 76u, 5u);
    bytes[84u] = 0x09u;
    bytes[85u] = 0x00u;
    return bytes;
}

std::vector<std::vector<std::uint8_t>> seeds(const Target target) {
    switch (target) {
    case Target::Decoder:
        return {{0x09u, 0x00u, 0x0Bu, 0x00u, 0x00u, 0xA0u}, {0xFFu, 0xFFu}};
    case Target::Loader:
        return {minimal_elf_seed(), {}, {0x7Fu, 'E', 'L', 'F'}};
    case Target::Ir:
        return {{0u, 1u, 2u, 3u, 4u, 5u}, {}, {0xFFu, 0x80u, 0x7Fu}};
    case Target::Runtime:
        return {{0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}, {0xFFu, 0x80u, 0x40u, 0x20u}, {}};
    case Target::All:
        break;
    }
    throw std::invalid_argument("Das kombinierte Fuzzziel besitzt kein eigenes Korpus.");
}

std::vector<std::uint8_t>
mutate(const std::span<const std::uint8_t> source, Random& random, const std::size_t maximum) {
    std::vector<std::uint8_t> bytes(source.begin(), source.end());
    if (bytes.size() > maximum) {
        bytes.resize(maximum);
    }
    const auto edits = 1u + random.index(8u);
    for (std::size_t edit = 0u; edit < edits; ++edit) {
        switch (random.index(5u)) {
        case 0u:
            if (bytes.empty()) {
                bytes.push_back(static_cast<std::uint8_t>(random.next()));
            } else {
                bytes[random.index(bytes.size())] ^=
                    static_cast<std::uint8_t>(1u << random.index(8u));
            }
            break;
        case 1u:
            if (bytes.empty()) {
                bytes.push_back(static_cast<std::uint8_t>(random.next()));
            } else {
                bytes[random.index(bytes.size())] = static_cast<std::uint8_t>(random.next());
            }
            break;
        case 2u:
            if (bytes.size() < maximum) {
                const auto position = random.index(bytes.size() + 1u);
                bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                             static_cast<std::uint8_t>(random.next()));
            }
            break;
        case 3u:
            if (!bytes.empty()) {
                bytes.erase(bytes.begin() +
                            static_cast<std::ptrdiff_t>(random.index(bytes.size())));
            }
            break;
        case 4u: {
            const auto size = random.index(maximum + 1u);
            bytes.resize(size, static_cast<std::uint8_t>(random.next()));
            break;
        }
        }
    }
    return bytes;
}

void exercise_decoder(const std::span<const std::uint8_t> bytes) {
    for (std::size_t offset = 0u; offset + 1u < bytes.size(); offset += 2u) {
        const auto opcode = static_cast<std::uint16_t>(bytes[offset]) |
                            (static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u);
        const auto decoded = katana::sh4::decode(opcode);
        static_cast<void>(katana::sh4::calculate_direct_branch_target(
            decoded, 0x8C000000u + static_cast<std::uint32_t>(offset)));
    }
}

void exercise_loader(const std::span<const std::uint8_t> bytes) {
    try {
        const auto image =
            katana::io::load_elf32_sh(bytes, std::filesystem::path("synthetic-fuzz.elf"));
        for (const auto& segment : image.segments()) {
            static_cast<void>(segment.end_address());
            if (!segment.bytes.empty()) {
                static_cast<void>(segment.byte_offset(segment.virtual_address));
            }
        }
    } catch (const std::runtime_error&) {
    } catch (const std::invalid_argument&) {
    } catch (const std::out_of_range&) {
    }
}

bool is_direct_control_flow(const katana::ir::Operation operation) noexcept {
    using katana::ir::Operation;
    return operation == Operation::Branch || operation == Operation::Call ||
           operation == Operation::BranchIfTrue || operation == Operation::BranchIfFalse;
}

katana::ir::Function ir_from_bytes(const std::span<const std::uint8_t> bytes) {
    using namespace katana::ir;
    constexpr auto operation_count = static_cast<std::size_t>(Operation::Return) + 1u;
    constexpr std::uint32_t base = 0x8C000000u;
    Function function;
    function.entry_address = base;
    BasicBlock block;
    block.start_address = base;
    const auto count = std::min<std::size_t>(bytes.size(), 128u);
    block.instructions.reserve(count);
    for (std::size_t index = 0u; index < count; ++index) {
        Instruction instruction;
        instruction.source_address = base + static_cast<std::uint32_t>(index * 2u);
        instruction.original_opcode = 0x0009u;
        instruction.operation = static_cast<Operation>(bytes[index] % operation_count);
        instruction.original_operation = instruction.operation;
        instruction.destination_register = static_cast<std::uint8_t>(bytes[index] & 0x0Fu);
        instruction.source_register = static_cast<std::uint8_t>((bytes[index] >> 4u) & 0x0Fu);
        instruction.branch_register = instruction.source_register;
        instruction.immediate = static_cast<std::int8_t>(bytes[index]);
        instruction.displacement = instruction.immediate;
        instruction.widths = operation_operand_widths(instruction.operation);
        instruction.status_effects = instruction_status_effects(instruction.operation);
        instruction.memory_effects = instruction_memory_effects(
            instruction.operation, instruction.destination_register, instruction.source_register);
        instruction.accumulator_effects = operation_accumulator_effects(instruction.operation);
        if (is_direct_control_flow(instruction.operation)) {
            instruction.target_address = base;
        }
        if (instruction.operation == Operation::LoadWordSignedPcRelative ||
            instruction.operation == Operation::LoadLongPcRelative ||
            instruction.operation == Operation::MoveAddressPcRelative) {
            instruction.effective_address = base;
        }
        block.instructions.push_back(std::move(instruction));
    }
    function.blocks.push_back(std::move(block));
    return function;
}

void exercise_ir(const std::span<const std::uint8_t> bytes) {
    auto function = ir_from_bytes(bytes);
    if (!katana::ir::verify_function(function).empty()) {
        return;
    }
    std::vector<katana::ir::Function> program;
    program.push_back(std::move(function));
    static_cast<void>(katana::ir::optimize_program(program));
    katana::ir::require_valid_program(program);
}

katana::runtime::BlockExit fuzz_backend_block(katana::runtime::CpuState&,
                                              katana::runtime::BlockExecutionContext&) {
    return {};
}

void require_runtime(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void exercise_runtime(const std::span<const std::uint8_t> bytes) {
    using namespace katana::runtime;
    const auto value = [&](const std::size_t index) {
        return index < bytes.size() ? bytes[index] : static_cast<std::uint8_t>(index * 37u);
    };

    katana::io::ExecutableImage image("synthetic-runtime-fuzz");
    const auto segment_count = 1u + static_cast<std::size_t>(value(0u) % 4u);
    for (std::size_t index = 0u; index < segment_count; ++index) {
        const auto size = 2u + static_cast<std::size_t>(value(index + 1u) % 31u);
        const auto address = 0x0C000000u + static_cast<std::uint32_t>(index * 0x1000u);
        image.add_segment(
            {"segment-" + std::to_string(index),
             address,
             index * 0x1000u,
             size,
             (index % 2u) == 0u ? katana::io::SegmentKind::Code : katana::io::SegmentKind::Data,
             {true, (index % 2u) != 0u, (index % 2u) == 0u},
             std::vector<std::uint8_t>(size, value(index + 2u))});
    }
    require_runtime(image.segments().size() == segment_count,
                    "Multi-Segment-Image verliert ein synthetisches Segment.");

    bool overlap_rejected = false;
    try {
        image.add_segment({"overlap",
                           0x0C000001u,
                           0u,
                           2u,
                           katana::io::SegmentKind::Code,
                           {true, false, true},
                           {0x09u, 0x00u}});
    } catch (const std::invalid_argument&) {
        overlap_rejected = true;
    }
    require_runtime(overlap_rejected,
                    "Ueberlappende Segmentprovenienz wird im Runtime-Fuzzer akzeptiert.");

    const std::array valid_aliases{
        katana::io::ProjectAliasGroup{0x8C000000u, 0x0C000000u, 0x1000u},
        katana::io::ProjectAliasGroup{0xAC001000u, 0x0C001000u, 0x1000u}};
    const std::array canonical_ranges{katana::io::ProjectAddressRange{0x0C000000u, 0x00010000u}};
    katana::io::require_valid_project_alias_groups(valid_aliases, canonical_ranges);
    const std::array cyclic_aliases{
        katana::io::ProjectAliasGroup{0x8C000000u, 0xAC000000u, 0x1000u},
        katana::io::ProjectAliasGroup{0xAC000000u, 0x8C000000u, 0x1000u}};
    const std::array cyclic_ranges{katana::io::ProjectAddressRange{0x8C000000u, 0x20001000u}};
    bool cycle_rejected = false;
    try {
        katana::io::require_valid_project_alias_groups(cyclic_aliases, cyclic_ranges);
    } catch (const std::invalid_argument&) {
        cycle_rejected = true;
    }
    require_runtime(cycle_rejected, "Aliaszyklus wird im Runtime-Fuzzer akzeptiert.");

    RuntimeAddressSpace address_space;
    if ((value(2u) & 1u) != 0u) {
        address_space.set_mode(AddressTranslationMode::Mmu);
        address_space.ldtlb({0x8C000000u, 0x0C000000u, true, true, true, true});
    }
    const auto guard = address_space.guard_for(0x8C000000u, value(3u));
    const auto variant = block_variant_key(guard, 0u);
    RuntimeBlockTable table;
    table.register_static({0x8C000000u,
                           0x0C000000u,
                           2u,
                           BlockEndKind::DynamicBranch,
                           variant,
                           fuzz_backend_block,
                           "image-segment",
                           false});
    table.register_runtime({0xAC000000u,
                            0x0C000000u,
                            2u,
                            BlockEndKind::Return,
                            variant,
                            fuzz_backend_block,
                            "rom-ram-alias",
                            true});
    require_runtime(table.lookup(0x8C000000u, variant) != nullptr &&
                        table.aliases(0x0C000000u).size() == 2u,
                    "Blocktabelle verliert exakten Block oder kanonische Aliasgruppe.");

    CpuState cpu;
    const auto indirect = dispatch_indirect(cpu,
                                            table,
                                            {IndirectDispatchKind::TailJump,
                                             0x8C000100u,
                                             0xCC000000u,
                                             0u,
                                             {0x8C000100u, 0x0C000100u},
                                             variant});
    require_runtime(indirect.alias_lookup && indirect.block != nullptr &&
                        indirect.physical_target == 0x0C000000u,
                    "Generischer indirekter Dispatch und Aliaslookup widersprechen sich.");

    address_space.bump_watchpoints();
    const auto watchpoint_variant =
        block_variant_key(address_space.guard_for(0x8C000000u, value(3u)), 0u);
    require_runtime(table.lookup(0x8C000000u, watchpoint_variant) == nullptr,
                    "Watchpointgeneration verwendet eine stale Blockvariante erneut.");

    ExecutableCodeTracker tracker;
    ExecutableBlockRegistration registration{"fuzz-runtime-block",
                                             0x0C000000u,
                                             2u,
                                             "synthetic-rom-ram-copy",
                                             {"callsite-a"},
                                             ExecutableBlockOrigin::RomRamCopy};
    require_runtime(tracker.register_block(registration) == BlockRegistrationResult::Inserted,
                    "Runtimeblock wird nicht erstmalig registriert.");
    require_runtime(tracker.register_block(registration) == BlockRegistrationResult::AlreadyValid,
                    "Identische gueltige Runtimeblockregistrierung ist nicht idempotent.");
    const auto source = static_cast<CodeWriteSource>(value(4u) % 3u);
    const auto invalidation = tracker.observe_write(0xAC000000u, 2u, source);
    require_runtime(!tracker.valid(registration.identity) &&
                        invalidation.invalidated_blocks ==
                            std::vector<std::string>{registration.identity},
                    "Schreibinvalidierung laesst einen stale Runtimeblock gueltig.");
    const auto invalidated_variant = block_variant_key(guard, tracker.page_generation(0x0C000000u));
    require_runtime(table.lookup(0x8C000000u, invalidated_variant) == nullptr,
                    "Seitengeneration erzwingt nach Schreibinvalidierung keinen neuen Lookup.");
    require_runtime(tracker.register_block(registration) == BlockRegistrationResult::Reactivated,
                    "Invalidierter ROM-RAM-Block wird nicht kontrolliert reaktiviert.");

    auto changed = registration;
    changed.physical_start += 2u;
    bool changed_rejected = false;
    try {
        static_cast<void>(tracker.register_block(changed));
    } catch (const std::invalid_argument&) {
        changed_rejected = true;
    }
    require_runtime(changed_rejected,
                    "Gleiche Provenienzidentitaet wechselt unbemerkt ihre physische Adresse.");

    CanonicalBlockDispatcher dispatcher(table);
    dispatcher.link("caller-a", "fuzz-runtime-block");
    dispatcher.link("caller-a", "fuzz-runtime-block");
    require_runtime(dispatcher.incoming_link_count("fuzz-runtime-block") == 1u &&
                        dispatcher.unlink_target("fuzz-runtime-block") ==
                            std::vector<std::string>{"caller-a"},
                    "Dispatchlinks werden dupliziert oder nicht deterministisch invalidiert.");
}

std::string_view target_name(const Target target) noexcept {
    switch (target) {
    case Target::Decoder:
        return "decoder";
    case Target::Loader:
        return "loader";
    case Target::Ir:
        return "ir";
    case Target::Runtime:
        return "runtime";
    case Target::All:
        return "all";
    }
    return "unknown";
}

void exercise(const Target target, const std::span<const std::uint8_t> bytes) {
    switch (target) {
    case Target::Decoder:
        exercise_decoder(bytes);
        return;
    case Target::Loader:
        exercise_loader(bytes);
        return;
    case Target::Ir:
        exercise_ir(bytes);
        return;
    case Target::Runtime:
        exercise_runtime(bytes);
        return;
    case Target::All:
        break;
    }
    throw std::invalid_argument("Das kombinierte Fuzzziel kann keinen Einzelfall ausfuehren.");
}

std::optional<std::string> crash_signature(const Target target,
                                           const std::span<const std::uint8_t> bytes) {
    try {
        exercise(target, bytes);
    } catch (const std::exception& error) {
        return std::string(typeid(error).name()) + ":" + error.what();
    } catch (...) {
        return "non-standard-exception";
    }
    return std::nullopt;
}

std::vector<std::uint8_t> minimize_crasher(const Target target,
                                           const std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> current(input.begin(), input.end());
    const auto signature = crash_signature(target, current);
    if (!signature) return current;

    std::size_t granularity = 2u;
    while (!current.empty()) {
        const auto chunk = (current.size() + granularity - 1u) / granularity;
        bool reduced = false;
        for (std::size_t start = 0u; start < current.size(); start += chunk) {
            auto candidate = current;
            const auto finish = std::min(start + chunk, candidate.size());
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(start),
                            candidate.begin() + static_cast<std::ptrdiff_t>(finish));
            if (crash_signature(target, candidate) == signature) {
                current = std::move(candidate);
                granularity = 2u;
                reduced = true;
                break;
            }
        }
        if (reduced) continue;
        if (granularity >= current.size()) break;
        granularity = std::min(current.size(), granularity * 2u);
    }
    for (std::size_t index = 0u; index < current.size(); ++index) {
        if (current[index] == 0u) continue;
        auto candidate = current;
        candidate[index] = 0u;
        if (crash_signature(target, candidate) == signature) {
            current = std::move(candidate);
        }
    }
    return current;
}

std::string hex_bytes(const std::span<const std::uint8_t> bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes)
        output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

void print_counterexample(const Target target,
                          const std::uint64_t seed,
                          const std::size_t iteration,
                          const std::size_t maximum,
                          const std::span<const std::uint8_t> input) {
    const auto byte = [&](const std::size_t index) {
        return index < input.size() ? input[index] : static_cast<std::uint8_t>(index * 37u);
    };
    std::cerr << "counterexample={\"schema\":\"katana-fuzz-counterexample\","
              << "\"version\":1,\"target\":\"" << target_name(target) << "\","
              << "\"seed\":\"0x" << std::hex << std::uppercase << seed << std::dec << "\","
              << "\"iteration\":" << iteration << ",\"max_input_size\":" << maximum << ','
              << "\"input_hex\":\"" << hex_bytes(input) << "\",\"manifest\":{";
    if (target == Target::Runtime) {
        std::cerr << "\"kind\":\"runtime\",\"segment_count\":"
                  << (1u + static_cast<unsigned>(byte(0u) % 4u)) << ','
                  << "\"mmu\":" << (((byte(2u) & 1u) != 0u) ? "true" : "false") << ','
                  << "\"write_source\":" << static_cast<unsigned>(byte(4u) % 3u);
    } else {
        std::cerr << "\"kind\":\"raw-bytes\"";
    }
    std::cerr << "}}\n";
}

std::uint64_t hash_bytes(std::uint64_t hash, const std::span<const std::uint8_t> bytes) noexcept {
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

void run_target(const Target target, const Options& options, const std::uint64_t seed) {
    auto corpus = seeds(target);
    Random random(seed);
    std::uint64_t digest = 1469598103934665603ull;
    for (const auto& input : corpus) {
        exercise(target, input);
        digest = hash_bytes(digest, input);
    }
    for (std::size_t iteration = 0u; iteration < options.iterations; ++iteration) {
        auto input = mutate(corpus[random.index(corpus.size())], random, options.max_input_size);
        try {
            exercise(target, input);
        } catch (...) {
            const auto minimized = minimize_crasher(target, input);
            std::cerr << "Fuzz-Crasher: target=" << target_name(target) << " seed=0x" << std::hex
                      << std::uppercase << seed << std::dec << " iteration=" << iteration
                      << " reproduce=\"katana-fuzz --target " << target_name(target) << " --seed 0x"
                      << std::hex << std::uppercase << seed << std::dec << " --iterations "
                      << (iteration + 1u) << " --max-input-size " << options.max_input_size
                      << "\"\n";
            print_counterexample(target, seed, iteration, options.max_input_size, minimized);
            throw;
        }
        digest = hash_bytes(digest, input);
        if (corpus.size() < 64u && (iteration % 4u) == 0u) {
            corpus.push_back(std::move(input));
        }
    }
    std::cout << "Fuzzziel " << target_name(target) << ": seed=0x" << std::hex << std::uppercase
              << seed << " digest=0x" << digest << std::dec << " iterationen=" << options.iterations
              << '\n';
}

std::uint64_t parse_unsigned(const std::string_view value, const char* option) {
    auto digits = value;
    int base = 10;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2u);
        base = 16;
    }
    std::uint64_t result = 0u;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), result, base);
    if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
        throw std::invalid_argument(std::string("Ungueltiger Wert fuer ") + option + ".");
    }
    return result;
}

Target parse_target(const std::string_view value) {
    if (value == "decoder") return Target::Decoder;
    if (value == "loader") return Target::Loader;
    if (value == "ir") return Target::Ir;
    if (value == "runtime") return Target::Runtime;
    if (value == "all") return Target::All;
    throw std::invalid_argument("--target erwartet decoder, loader, ir, runtime oder all.");
}

Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout << "katana-fuzz --target decoder|loader|ir|runtime|all --seed N "
                         "--iterations N --max-input-size N\n";
            std::exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(argument) + " braucht einen Wert.");
        }
        const std::string_view value(argv[++index]);
        if (argument == "--target") {
            options.target = parse_target(value);
        } else if (argument == "--seed") {
            options.seed = parse_unsigned(value, "--seed");
        } else if (argument == "--iterations") {
            const auto parsed = parse_unsigned(value, "--iterations");
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::out_of_range("--iterations ist fuer diesen Host zu gross.");
            }
            options.iterations = static_cast<std::size_t>(parsed);
        } else if (argument == "--max-input-size") {
            const auto parsed = parse_unsigned(value, "--max-input-size");
            if (parsed == 0u || parsed > 1024u * 1024u ||
                parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::out_of_range("--max-input-size muss zwischen 1 und 1048576 liegen.");
            }
            options.max_input_size = static_cast<std::size_t>(parsed);
        } else {
            throw std::invalid_argument("Unbekannte Option: " + std::string(argument));
        }
    }
    return options;
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        if (options.target == Target::All) {
            run_target(Target::Decoder, options, options.seed ^ 0xDEC0DE01u);
            run_target(Target::Loader, options, options.seed ^ 0x10ADE002u);
            run_target(Target::Ir, options, options.seed ^ 0x1A000003u);
            run_target(Target::Runtime, options, options.seed ^ 0x37080004u);
        } else {
            run_target(options.target, options, options.seed);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Fuzzlauf fehlgeschlagen: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
