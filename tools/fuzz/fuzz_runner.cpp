#include "katana/io/elf32_sh_loader.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class Target {
    Decoder,
    Loader,
    Ir,
    All
};

struct Options {
    Target target = Target::All;
    std::uint64_t seed = 0x3703u;
    std::size_t iterations = 256u;
    std::size_t max_input_size = 4096u;
};

class Random {
public:
    explicit Random(const std::uint64_t seed)
        : state_(seed ^ 0x9E3779B97F4A7C15ull) {}

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

void write_u16(
    std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    const std::uint16_t value
) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void write_u32(
    std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    const std::uint32_t value
) {
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
        case Target::All:
            break;
    }
    throw std::invalid_argument("Das kombinierte Fuzzziel besitzt kein eigenes Korpus.");
}

std::vector<std::uint8_t> mutate(
    const std::span<const std::uint8_t> source,
    Random& random,
    const std::size_t maximum
) {
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
                    bytes[random.index(bytes.size())] ^= static_cast<std::uint8_t>(
                        1u << random.index(8u)
                    );
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
                    bytes.insert(
                        bytes.begin() + static_cast<std::ptrdiff_t>(position),
                        static_cast<std::uint8_t>(random.next())
                    );
                }
                break;
            case 3u:
                if (!bytes.empty()) {
                    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(random.index(bytes.size())));
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
            decoded,
            0x8C000000u + static_cast<std::uint32_t>(offset)
        ));
    }
}

void exercise_loader(const std::span<const std::uint8_t> bytes) {
    try {
        const auto image = katana::io::load_elf32_sh(
            bytes,
            std::filesystem::path("synthetic-fuzz.elf")
        );
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
            instruction.operation,
            instruction.destination_register,
            instruction.source_register
        );
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

std::string_view target_name(const Target target) noexcept {
    switch (target) {
        case Target::Decoder: return "decoder";
        case Target::Loader: return "loader";
        case Target::Ir: return "ir";
        case Target::All: return "all";
    }
    return "unknown";
}

void exercise(const Target target, const std::span<const std::uint8_t> bytes) {
    switch (target) {
        case Target::Decoder: exercise_decoder(bytes); return;
        case Target::Loader: exercise_loader(bytes); return;
        case Target::Ir: exercise_ir(bytes); return;
        case Target::All: break;
    }
    throw std::invalid_argument("Das kombinierte Fuzzziel kann keinen Einzelfall ausfuehren.");
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
            std::cerr << "Fuzz-Crasher: target=" << target_name(target)
                      << " seed=0x" << std::hex << std::uppercase << seed << std::dec
                      << " iteration=" << iteration << " reproduce=\"katana-fuzz --target "
                      << target_name(target) << " --seed 0x" << std::hex << std::uppercase
                      << seed << std::dec << " --iterations " << (iteration + 1u)
                      << " --max-input-size " << options.max_input_size << "\"\n";
            throw;
        }
        digest = hash_bytes(digest, input);
        if (corpus.size() < 64u && (iteration % 4u) == 0u) {
            corpus.push_back(std::move(input));
        }
    }
    std::cout << "Fuzzziel " << target_name(target) << ": seed=0x"
              << std::hex << std::uppercase << seed << " digest=0x" << digest
              << std::dec << " iterationen=" << options.iterations << '\n';
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
    if (value == "all") return Target::All;
    throw std::invalid_argument("--target erwartet decoder, loader, ir oder all.");
}

Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout << "katana-fuzz --target decoder|loader|ir|all --seed N "
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

}

int main(const int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        if (options.target == Target::All) {
            run_target(Target::Decoder, options, options.seed ^ 0xDEC0DE01u);
            run_target(Target::Loader, options, options.seed ^ 0x10ADE002u);
            run_target(Target::Ir, options, options.seed ^ 0x1A000003u);
        } else {
            run_target(options.target, options, options.seed);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Fuzzlauf fehlgeschlagen: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
