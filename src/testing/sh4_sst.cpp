#include "katana/testing/sh4_sst.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <system_error>
#include <utility>

namespace katana::testing {
namespace {

constexpr std::uint32_t initial_state_tag = 1u;
constexpr std::uint32_t final_state_tag = 2u;
constexpr std::uint32_t cycles_tag = 3u;
constexpr std::uint32_t opcodes_tag = 4u;

class LittleEndianReader final {
  public:
    LittleEndianReader(const std::span<const std::uint8_t> bytes, std::string filename)
        : bytes_(bytes), filename_(std::move(filename)) {}

    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

    [[nodiscard]] std::uint32_t u32(const std::string_view field) {
        require(4u, field);
        const auto value = static_cast<std::uint32_t>(bytes_[position_]) |
                           (static_cast<std::uint32_t>(bytes_[position_ + 1u]) << 8u) |
                           (static_cast<std::uint32_t>(bytes_[position_ + 2u]) << 16u) |
                           (static_cast<std::uint32_t>(bytes_[position_ + 3u]) << 24u);
        position_ += 4u;
        return value;
    }

    [[nodiscard]] std::uint64_t u64(const std::string_view field) {
        require(8u, field);
        std::uint64_t value = 0u;
        for (std::size_t index = 0u; index < 8u; ++index)
            value |= static_cast<std::uint64_t>(bytes_[position_ + index]) << (index * 8u);
        position_ += 8u;
        return value;
    }

    void expect_u32(const std::uint32_t expected, const std::string_view field) {
        const auto offset = position_;
        const auto actual = u32(field);
        if (actual == expected) return;
        std::ostringstream message;
        message << field << " ist " << actual << " statt " << expected << '.';
        throw SstCorpusInvalid(message.str(), filename_, offset);
    }

  private:
    void require(const std::size_t count, const std::string_view field) const {
        if (position_ <= bytes_.size() && count <= bytes_.size() - position_) return;
        throw SstCorpusInvalid(std::string(field) + " ist unvollstaendig.", filename_, position_);
    }

    std::span<const std::uint8_t> bytes_;
    std::string filename_;
    std::size_t position_ = 0u;
};

struct FilenameMetadata {
    std::string filename;
    std::string opcode_pattern;
    bool sz = false;
    bool pr = false;
};

[[nodiscard]] FilenameMetadata parse_filename_metadata(const std::string_view filename) {
    static const std::regex pattern(R"(^([01nmid]{16})_sz([01])_pr([01])\.json\.bin$)",
                                    std::regex::ECMAScript | std::regex::optimize);
    const std::string text(filename);
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) {
        throw SstCorpusInvalid(
            "SST-Dateiname entspricht nicht " + std::string(sh4_sst_filename_regex) + '.', text);
    }
    return {text, match[1].str(), match[2].str() == "1", match[3].str() == "1"};
}

[[nodiscard]] bool valid_access_size(const std::uint32_t size) noexcept {
    return size == 1u || size == 2u || size == 4u || size == 8u;
}

[[nodiscard]] SstState parse_state(LittleEndianReader& reader, const std::uint32_t expected_tag) {
    reader.expect_u32(static_cast<std::uint32_t>(sh4_sst_state_chunk_size),
                      expected_tag == initial_state_tag ? "initial.size" : "final.size");
    reader.expect_u32(expected_tag,
                      expected_tag == initial_state_tag ? "initial.tag" : "final.tag");

    SstState state;
    for (auto& value : state.r)
        value = reader.u32("state.R");
    for (auto& value : state.r_bank)
        value = reader.u32("state.R_");
    for (auto& value : state.fp0)
        value = reader.u32("state.FP0");
    for (auto& value : state.fp1)
        value = reader.u32("state.FP1");
    state.pc = reader.u32("state.PC");
    state.gbr = reader.u32("state.GBR");
    state.sr = reader.u32("state.SR");
    state.ssr = reader.u32("state.SSR");
    state.spc = reader.u32("state.SPC");
    state.vbr = reader.u32("state.VBR");
    state.sgr = reader.u32("state.SGR");
    state.dbr = reader.u32("state.DBR");
    state.macl = reader.u32("state.MACL");
    state.mach = reader.u32("state.MACH");
    state.pr = reader.u32("state.PR");
    state.fpscr = reader.u32("state.FPSCR");
    state.fpul = reader.u32("state.FPUL");
    return state;
}

[[nodiscard]] std::array<SstCycle, sh4_sst_cycle_count> parse_cycles(LittleEndianReader& reader,
                                                                     const std::string& filename) {
    reader.expect_u32(static_cast<std::uint32_t>(sh4_sst_cycle_chunk_size), "cycles.size");
    reader.expect_u32(cycles_tag, "cycles.tag");
    reader.expect_u32(static_cast<std::uint32_t>(sh4_sst_cycle_count), "cycles.count");

    std::array<SstCycle, sh4_sst_cycle_count> cycles{};
    for (std::size_t index = 0u; index < cycles.size(); ++index) {
        const auto entry_offset = reader.position();
        auto& cycle = cycles[index];
        cycle.actions = reader.u32("cycle.actions");
        cycle.fetch_address = reader.u32("cycle.fetch_addr");
        cycle.fetch_value = reader.u32("cycle.fetch_val");
        cycle.write_address = reader.u32("cycle.write_addr");
        cycle.write_value = reader.u64("cycle.write_val");
        cycle.write_size = reader.u32("cycle.write_size");
        cycle.read_address = reader.u32("cycle.read_addr");
        cycle.read_value = reader.u64("cycle.read_val");
        cycle.read_size = reader.u32("cycle.read_size");

        if ((cycle.actions & ~SstCycle::valid_actions) != 0u) {
            throw SstCorpusInvalid("Cycle besitzt unbekannte Aktionsbits.", filename, entry_offset);
        }
        if (cycle.has_write() && !valid_access_size(cycle.write_size)) {
            throw SstCorpusInvalid(
                "Aktiver Schreibzugriff besitzt ungueltige Breite.", filename, entry_offset + 24u);
        }
        if (cycle.has_read() && !valid_access_size(cycle.read_size)) {
            throw SstCorpusInvalid(
                "Aktiver Lesezugriff besitzt ungueltige Breite.", filename, entry_offset + 40u);
        }
    }
    return cycles;
}

[[nodiscard]] std::array<std::uint16_t, sh4_sst_opcode_count>
parse_opcodes(LittleEndianReader& reader, const std::string& filename) {
    reader.expect_u32(static_cast<std::uint32_t>(sh4_sst_opcode_chunk_size), "opcodes.size");
    reader.expect_u32(opcodes_tag, "opcodes.tag");

    std::array<std::uint16_t, sh4_sst_opcode_count> opcodes{};
    for (auto& opcode : opcodes) {
        const auto offset = reader.position();
        const auto value = reader.u32("opcode");
        if (value > std::numeric_limits<std::uint16_t>::max())
            throw SstCorpusInvalid("Opcode passt nicht in 16 Bit.", filename, offset);
        opcode = static_cast<std::uint16_t>(value);
    }
    return opcodes;
}

void validate_filename_contract(const FilenameMetadata& metadata,
                                const SstTestCase& test,
                                const std::size_t record_offset) {
    const bool state_pr = (test.initial.fpscr & runtime::fpscr_pr_mask) != 0u;
    const bool state_sz = (test.initial.fpscr & runtime::fpscr_sz_mask) != 0u;
    if (state_pr != metadata.pr || state_sz != metadata.sz) {
        throw SstCorpusInvalid("Dateiname und initiale FPSCR.PR/SZ-Bits widersprechen sich.",
                               metadata.filename,
                               record_offset);
    }

    const auto opcode = test.opcodes[1];
    for (std::size_t index = 0u; index < metadata.opcode_pattern.size(); ++index) {
        const auto character = metadata.opcode_pattern[index];
        if (character != '0' && character != '1') continue;
        const auto bit = static_cast<std::uint16_t>(1u << (15u - index));
        const bool actual = (opcode & bit) != 0u;
        if (actual == (character == '1')) continue;
        throw SstCorpusInvalid("Testopcode verletzt die festen Bits des Dateinamens.",
                               metadata.filename,
                               record_offset + 768u + 4u);
    }
}

[[nodiscard]] std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (error) {
        throw SstInfrastructureError("SST-Dateigroesse konnte nicht gelesen werden.",
                                     path.filename().string());
    }
    if (file_size > std::numeric_limits<std::size_t>::max()) {
        throw SstInfrastructureError("SST-Datei ist fuer den Host zu gross.",
                                     path.filename().string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw SstInfrastructureError("SST-Datei konnte nicht geoeffnet werden.",
                                     path.filename().string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw SstInfrastructureError("SST-Datei konnte nicht vollstaendig gelesen werden.",
                                     path.filename().string());
    }
    return bytes;
}

void add_difference(SstStateComparison& comparison,
                    std::string path,
                    const std::uint64_t expected,
                    const std::uint64_t actual) {
    if (expected != actual) comparison.differences.push_back({std::move(path), expected, actual});
}

template <std::size_t Size>
void compare_words(SstStateComparison& comparison,
                   const std::string_view prefix,
                   const std::array<std::uint32_t, Size>& expected,
                   const std::array<std::uint32_t, Size>& actual) {
    for (std::size_t index = 0u; index < Size; ++index) {
        add_difference(comparison,
                       std::string(prefix) + '[' + std::to_string(index) + ']',
                       expected[index],
                       actual[index]);
    }
}

[[nodiscard]] bool upstream_compatible_float(const std::uint32_t expected,
                                             const std::uint32_t actual) noexcept {
    if (expected == actual) return true;
    const auto expected_float = std::bit_cast<float>(expected);
    const auto actual_float = std::bit_cast<float>(actual);
    if (expected_float == actual_float) return true;
    if (std::isnan(expected_float) && std::isnan(actual_float)) return true;

    if ((expected & 0x80000000u) == (actual & 0x80000000u)) {
        const auto larger = std::max(expected, actual);
        const auto smaller = std::min(expected, actual);
        if (larger - smaller <= 4u) return true;
    }
    if (std::isfinite(expected_float) && std::isfinite(actual_float) &&
        std::fabs(static_cast<double>(expected_float) - static_cast<double>(actual_float)) <
            0.0000001)
        return true;

    constexpr std::array special_pairs{
        std::pair{0x7F800000u, 0xFF7FFFFFu},
        std::pair{0x36865C49u, 0xB1E2C629u},
        std::pair{0x7FF84903u, 0x7FC00000u},
        std::pair{0xFF800000u, 0x7F7FFFFFu},
    };
    return std::any_of(special_pairs.begin(), special_pairs.end(), [&](const auto& pair) {
        return (expected == pair.first && actual == pair.second) ||
               (expected == pair.second && actual == pair.first);
    });
}

void compare_fpu_words(SstStateComparison& comparison,
                       const std::string_view prefix,
                       const std::array<std::uint32_t, 16u>& expected,
                       const std::array<std::uint32_t, 16u>& actual,
                       const FpuComparisonMode mode) {
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        const bool equal = mode == FpuComparisonMode::Strict
                               ? expected[index] == actual[index]
                               : upstream_compatible_float(expected[index], actual[index]);
        if (!equal) {
            comparison.differences.push_back(
                {std::string(prefix) + '[' + std::to_string(index) + ']',
                 expected[index],
                 actual[index]});
        }
    }
}

template <typename Value>
void compare_internal_scalar(SstStateComparison& comparison,
                             const std::string_view path,
                             const Value expected,
                             const Value actual) {
    add_difference(comparison,
                   std::string(path),
                   static_cast<std::uint64_t>(expected),
                   static_cast<std::uint64_t>(actual));
}

[[nodiscard]] std::uint64_t pointer_value(const void* const value) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

class StrictJsonReader final {
  public:
    StrictJsonReader(const std::string_view text, std::string filename)
        : text_(text), filename_(std::move(filename)) {}

    [[nodiscard]] bool consume(const char expected) {
        whitespace();
        if (position_ >= text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }

    void expect(const char expected, const std::string_view field) {
        if (consume(expected)) return;
        invalid(std::string(field) + " erwartet '" + expected + "'.");
    }

    [[nodiscard]] char peek() {
        whitespace();
        if (position_ >= text_.size()) invalid("Unerwartetes Ende der Waiver-Datei.");
        return text_[position_];
    }

    [[nodiscard]] std::string string(const std::string_view field) {
        whitespace();
        if (position_ >= text_.size() || text_[position_] != '"')
            invalid(std::string(field) + " muss ein JSON-String sein.");
        ++position_;
        std::string result;
        while (position_ < text_.size()) {
            const auto character = static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return result;
            if (character < 0x20u)
                invalid(std::string(field) + " enthaelt ein unmaskiertes Steuerzeichen.");
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= text_.size())
                invalid(std::string(field) + " endet in einer Escape-Sequenz.");
            const auto escape = text_[position_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                result.push_back(escape);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u':
                append_unicode_escape(result, field);
                break;
            default:
                invalid(std::string(field) + " enthaelt eine ungueltige Escape-Sequenz.");
            }
        }
        invalid(std::string(field) + " besitzt kein schliessendes Anfuehrungszeichen.");
    }

    [[nodiscard]] std::uint32_t u32(const std::string_view field) {
        whitespace();
        const auto start = position_;
        if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9')
            invalid(std::string(field) + " muss eine nichtnegative Ganzzahl sein.");
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
                invalid(std::string(field) + " besitzt eine ungueltige fuehrende Null.");
            return 0u;
        }
        std::uint64_t value = 0u;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            value = value * 10u + static_cast<unsigned>(text_[position_] - '0');
            if (value > std::numeric_limits<std::uint32_t>::max())
                invalid(std::string(field) + " ist groesser als uint32.");
            ++position_;
        }
        if (position_ == start) invalid(std::string(field) + " ist leer.");
        return static_cast<std::uint32_t>(value);
    }

    void null_value(const std::string_view field) {
        whitespace();
        if (text_.substr(position_, 4u) != "null")
            invalid(std::string(field) + " muss null oder ein Objekt sein.");
        position_ += 4u;
    }

    void require_eof() {
        whitespace();
        if (position_ != text_.size()) invalid("Waiver-Datei besitzt nachlaufende JSON-Daten.");
    }

    [[noreturn]] void invalid(std::string message) const {
        throw SstCorpusInvalid(std::move(message), filename_, position_);
    }

  private:
    static void append_utf8(std::string& output, const std::uint32_t codepoint) {
        if (codepoint <= 0x7Fu) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFu) {
            output.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        } else if (codepoint <= 0xFFFFu) {
            output.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        } else {
            output.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        }
    }

    [[nodiscard]] std::uint32_t hex_quad(const std::string_view field) {
        if (position_ > text_.size() || 4u > text_.size() - position_)
            invalid(std::string(field) + " besitzt eine unvollstaendige Unicode-Escape-Sequenz.");
        std::uint32_t value = 0u;
        for (std::size_t index = 0u; index < 4u; ++index) {
            const auto character = text_[position_++];
            value <<= 4u;
            if (character >= '0' && character <= '9') {
                value |= static_cast<unsigned>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<unsigned>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<unsigned>(character - 'A' + 10);
            } else {
                invalid(std::string(field) + " besitzt eine ungueltige Unicode-Hexziffer.");
            }
        }
        return value;
    }

    void append_unicode_escape(std::string& output, const std::string_view field) {
        auto codepoint = hex_quad(field);
        if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
            if (position_ > text_.size() || 2u > text_.size() - position_ ||
                text_[position_] != '\\' || text_[position_ + 1u] != 'u') {
                invalid(std::string(field) + " besitzt ein einzelnes hohes UTF-16-Surrogat.");
            }
            position_ += 2u;
            const auto low = hex_quad(field);
            if (low < 0xDC00u || low > 0xDFFFu)
                invalid(std::string(field) + " besitzt ein ungueltiges UTF-16-Surrogatpaar.");
            codepoint = 0x10000u + ((codepoint - 0xD800u) << 10u) + (low - 0xDC00u);
        } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
            invalid(std::string(field) + " besitzt ein einzelnes niedriges UTF-16-Surrogat.");
        }
        append_utf8(output, codepoint);
    }

    void whitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                            text_[position_] == '\r' || text_[position_] == '\n')) {
            ++position_;
        }
    }

    std::string_view text_;
    std::string filename_;
    std::size_t position_ = 0u;
};

void require_unique_key(StrictJsonReader& reader, bool& seen, const std::string_view key) {
    if (seen) reader.invalid("JSON-Schluessel " + std::string(key) + " ist doppelt.");
    seen = true;
}

[[nodiscard]] ResultClassification waiver_classification(StrictJsonReader& reader,
                                                         const std::string& value) {
    constexpr std::array allowed{
        ResultClassification::NotApplicableReferenceAlignment,
        ResultClassification::NotApplicableReferenceException,
        ResultClassification::NotApplicableReferenceMmio,
        ResultClassification::NotApplicableReferenceKnownBug,
        ResultClassification::NotApplicableKatanaRestricted,
        ResultClassification::NotApplicableAccessShape,
    };
    const auto found = std::find_if(allowed.begin(), allowed.end(), [&](const auto item) {
        return value == result_classification_name(item);
    });
    if (found == allowed.end())
        reader.invalid("Waiver-Klassifikation ist nicht als Nichtanwendbarkeitsgrund erlaubt.");
    return *found;
}

[[nodiscard]] std::vector<std::uint32_t> parse_case_indices(StrictJsonReader& reader) {
    reader.expect('[', "case_indices");
    std::vector<std::uint32_t> indices;
    if (reader.consume(']')) return indices;
    while (true) {
        indices.push_back(reader.u32("case_indices[]"));
        if (reader.consume(']')) break;
        reader.expect(',', "case_indices");
    }
    return indices;
}

[[nodiscard]] SstCaseRange parse_case_range(StrictJsonReader& reader) {
    reader.expect('{', "case_range");
    SstCaseRange range;
    bool first_seen = false;
    bool last_seen = false;
    if (reader.consume('}')) reader.invalid("case_range darf nicht leer sein.");
    while (true) {
        const auto key = reader.string("case_range key");
        reader.expect(':', "case_range");
        if (key == "first") {
            require_unique_key(reader, first_seen, key);
            range.first = reader.u32(key);
        } else if (key == "last") {
            require_unique_key(reader, last_seen, key);
            range.last = reader.u32(key);
        } else {
            reader.invalid("Unbekannter case_range-Schluessel: " + key);
        }
        if (reader.consume('}')) break;
        reader.expect(',', "case_range");
    }
    if (!first_seen || !last_seen) reader.invalid("case_range braucht first und last.");
    return range;
}

[[nodiscard]] SstWaiver parse_waiver(StrictJsonReader& reader) {
    reader.expect('{', "waiver");
    SstWaiver waiver;
    bool commit_seen = false;
    bool filename_seen = false;
    bool indices_seen = false;
    bool range_seen = false;
    bool classification_seen = false;
    bool reason_seen = false;
    bool evidence_seen = false;
    if (reader.consume('}')) reader.invalid("Waiver darf nicht leer sein.");
    while (true) {
        const auto key = reader.string("waiver key");
        reader.expect(':', "waiver");
        if (key == "corpus_commit") {
            require_unique_key(reader, commit_seen, key);
            waiver.corpus_commit = reader.string(key);
        } else if (key == "filename") {
            require_unique_key(reader, filename_seen, key);
            waiver.filename = reader.string(key);
        } else if (key == "case_indices") {
            require_unique_key(reader, indices_seen, key);
            waiver.case_indices = parse_case_indices(reader);
        } else if (key == "case_range") {
            require_unique_key(reader, range_seen, key);
            if (reader.peek() == 'n') {
                reader.null_value(key);
            } else {
                waiver.case_range = parse_case_range(reader);
            }
        } else if (key == "classification") {
            require_unique_key(reader, classification_seen, key);
            waiver.classification = waiver_classification(reader, reader.string(key));
        } else if (key == "reason") {
            require_unique_key(reader, reason_seen, key);
            waiver.reason = reader.string(key);
        } else if (key == "evidence") {
            require_unique_key(reader, evidence_seen, key);
            waiver.evidence = reader.string(key);
        } else {
            reader.invalid("Unbekannter Waiver-Schluessel: " + key);
        }
        if (reader.consume('}')) break;
        reader.expect(',', "waiver");
    }

    if (!commit_seen || !filename_seen || !classification_seen || !reason_seen || !evidence_seen) {
        reader.invalid(
            "Waiver braucht corpus_commit, filename, classification, reason und evidence.");
    }
    if (waiver.corpus_commit != sh4_sst_corpus_commit)
        reader.invalid("Waiver gilt nicht fuer den gepinnten SST-Corpus-Commit.");
    static_cast<void>(parse_filename_metadata(waiver.filename));
    const bool has_indices = indices_seen && !waiver.case_indices.empty();
    const bool has_range = range_seen && waiver.case_range.has_value();
    if (has_indices == has_range)
        reader.invalid("Waiver braucht exakt case_indices oder case_range.");
    if (has_indices) {
        if (!std::is_sorted(waiver.case_indices.begin(), waiver.case_indices.end()) ||
            std::adjacent_find(waiver.case_indices.begin(), waiver.case_indices.end()) !=
                waiver.case_indices.end()) {
            reader.invalid("case_indices muessen streng aufsteigend und eindeutig sein.");
        }
        if (waiver.case_indices.back() >= sh4_sst_corpus_records_per_file)
            reader.invalid("Waiver-Testindex liegt ausserhalb der 500 Corpusfaelle.");
    } else if (waiver.case_range->first > waiver.case_range->last ||
               waiver.case_range->last >= sh4_sst_corpus_records_per_file) {
        reader.invalid("Waiver-Testbereich ist leer oder liegt ausserhalb der 500 Corpusfaelle.");
    }
    const auto has_text = [](const std::string_view text) {
        return std::any_of(text.begin(), text.end(), [](const char character) {
            return character != ' ' && character != '\t' && character != '\r' && character != '\n';
        });
    };
    if (!has_text(waiver.reason) || !has_text(waiver.evidence))
        reader.invalid("Waiver braucht nichtleere Begruendung und zusaetzliche Evidenz.");
    return waiver;
}

void reject_overlapping_waivers(StrictJsonReader& reader, const std::vector<SstWaiver>& waivers) {
    std::map<std::string, std::array<bool, sh4_sst_corpus_records_per_file>> selected;
    for (const auto& waiver : waivers) {
        auto& cases = selected[waiver.filename];
        const auto select = [&](const std::uint32_t index) {
            if (cases[index])
                reader.invalid("Mehrere Waiver ueberlappen fuer denselben Corpusfall.");
            cases[index] = true;
        };
        if (!waiver.case_indices.empty()) {
            for (const auto index : waiver.case_indices)
                select(index);
        } else {
            for (auto index = waiver.case_range->first; index <= waiver.case_range->last; ++index)
                select(index);
        }
    }
}

[[nodiscard]] bool lowercase_sha256(const std::string_view value) noexcept {
    return value.size() == 64u && std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

void write_string_array(std::ostringstream& output, std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    output << '[';
    for (std::size_t index = 0u; index < values.size(); ++index) {
        if (index != 0u) output << ',';
        output << io::quote_json(values[index]);
    }
    output << ']';
}

void write_opcode_array(std::ostringstream& output, std::vector<std::uint16_t> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    output << '[';
    for (std::size_t index = 0u; index < values.size(); ++index) {
        if (index != 0u) output << ',';
        output << values[index];
    }
    output << ']';
}

void write_waiver_json(std::ostringstream& output, const SstWaiver& waiver) {
    output << "{\"corpus_commit\":" << io::quote_json(waiver.corpus_commit)
           << ",\"filename\":" << io::quote_json(waiver.filename);
    if (!waiver.case_indices.empty()) {
        output << ",\"case_indices\":[";
        for (std::size_t index = 0u; index < waiver.case_indices.size(); ++index) {
            if (index != 0u) output << ',';
            output << waiver.case_indices[index];
        }
        output << ']';
    } else if (waiver.case_range) {
        output << ",\"case_range\":{\"first\":" << waiver.case_range->first
               << ",\"last\":" << waiver.case_range->last << '}';
    }
    output << ",\"classification\":"
           << io::quote_json(result_classification_name(waiver.classification))
           << ",\"reason\":" << io::quote_json(waiver.reason)
           << ",\"evidence\":" << io::quote_json(waiver.evidence) << '}';
}

} // namespace

const char* result_classification_name(const ResultClassification value) noexcept {
    switch (value) {
    case ResultClassification::Pass:
        return "pass";
    case ResultClassification::FailState:
        return "fail-state";
    case ResultClassification::FailControlFlow:
        return "fail-control-flow";
    case ResultClassification::FailDelaySlot:
        return "fail-delay-slot";
    case ResultClassification::FailMemoryAddress:
        return "fail-memory-address";
    case ResultClassification::FailMemoryWidth:
        return "fail-memory-width";
    case ResultClassification::FailMemoryValue:
        return "fail-memory-value";
    case ResultClassification::FailMemoryOrder:
        return "fail-memory-order";
    case ResultClassification::FailExtraSideEffect:
        return "fail-extra-side-effect";
    case ResultClassification::FailUnboundTarget:
        return "fail-unbound-target";
    case ResultClassification::FailUnexpectedException:
        return "fail-unexpected-exception";
    case ResultClassification::NotApplicableReferenceAlignment:
        return "not-applicable-reference-alignment";
    case ResultClassification::NotApplicableReferenceException:
        return "not-applicable-reference-exception";
    case ResultClassification::NotApplicableReferenceMmio:
        return "not-applicable-reference-mmio";
    case ResultClassification::NotApplicableReferenceKnownBug:
        return "not-applicable-reference-known-bug";
    case ResultClassification::NotApplicableKatanaRestricted:
        return "not-applicable-katana-restricted";
    case ResultClassification::NotApplicableAccessShape:
        return "not-applicable-access-shape";
    case ResultClassification::CorpusInvalid:
        return "corpus-invalid";
    case ResultClassification::HarnessInvalid:
        return "harness-invalid";
    case ResultClassification::InfrastructureError:
        return "infrastructure-error";
    }
    return "infrastructure-error";
}

const char* memory_profile_name(const MemoryProfile value) noexcept {
    switch (value) {
    case MemoryProfile::NativeProductMemory:
        return "native-product-memory";
    case MemoryProfile::FlatSemanticMemory:
        return "flat-semantic-memory";
    }
    return "flat-semantic-memory";
}

const char* fpu_comparison_mode_name(const FpuComparisonMode value) noexcept {
    switch (value) {
    case FpuComparisonMode::Strict:
        return "strict";
    case FpuComparisonMode::UpstreamCompatible:
        return "upstream-compatible";
    }
    return "strict";
}

SstError::SstError(const ResultClassification classification,
                   std::string message,
                   std::string filename,
                   const std::optional<std::size_t> offset)
    : std::runtime_error(std::move(message)), classification_(classification),
      filename_(std::move(filename)), offset_(offset) {}

ResultClassification SstError::classification() const noexcept {
    return classification_;
}

const std::string& SstError::filename() const noexcept {
    return filename_;
}

const std::optional<std::size_t>& SstError::offset() const noexcept {
    return offset_;
}

SstCorpusInvalid::SstCorpusInvalid(std::string message,
                                   std::string filename,
                                   const std::optional<std::size_t> offset)
    : SstError(
          ResultClassification::CorpusInvalid, std::move(message), std::move(filename), offset) {}

SstHarnessInvalid::SstHarnessInvalid(std::string message)
    : SstError(ResultClassification::HarnessInvalid, std::move(message)) {}

SstInfrastructureError::SstInfrastructureError(std::string message, std::string filename)
    : SstError(ResultClassification::InfrastructureError, std::move(message), std::move(filename)) {
}

SstCorpusFile parse_sh4_sst_bytes(const std::span<const std::uint8_t> bytes,
                                  const std::string_view filename,
                                  const SstParserOptions options) {
    if (options.expected_record_count == 0u) {
        throw SstHarnessInvalid("SST-Parser braucht mindestens einen erwarteten Datensatz.");
    }
    if (options.expected_record_count >
        std::numeric_limits<std::size_t>::max() / sh4_sst_record_size) {
        throw SstHarnessInvalid("Erwartete SST-Datensatzanzahl ist zu gross.");
    }
    const auto expected_size = options.expected_record_count * sh4_sst_record_size;
    if (bytes.size() != expected_size) {
        std::ostringstream message;
        message << "SST-Datei besitzt " << bytes.size() << " statt " << expected_size
                << " Bytes; unvollstaendige und nachlaufende Daten sind ungueltig.";
        throw SstCorpusInvalid(
            message.str(), std::string(filename), std::min(bytes.size(), expected_size));
    }

    std::optional<FilenameMetadata> metadata;
    if (options.validate_filename) metadata = parse_filename_metadata(filename);
    LittleEndianReader reader(bytes, std::string(filename));
    SstCorpusFile corpus;
    corpus.filename = std::string(filename);
    corpus.cases.reserve(options.expected_record_count);
    for (std::size_t index = 0u; index < options.expected_record_count; ++index) {
        const auto record_offset = reader.position();
        reader.expect_u32(static_cast<std::uint32_t>(sh4_sst_record_size), "record.size");
        SstTestCase test;
        test.initial = parse_state(reader, initial_state_tag);
        test.final = parse_state(reader, final_state_tag);
        test.cycles = parse_cycles(reader, corpus.filename);
        test.opcodes = parse_opcodes(reader, corpus.filename);
        if (reader.position() - record_offset != sh4_sst_record_size) {
            throw SstCorpusInvalid("SST-Datensatz endet nicht an seiner deklarierten Grenze.",
                                   corpus.filename,
                                   reader.position());
        }
        if (metadata) validate_filename_contract(*metadata, test, record_offset);
        corpus.cases.push_back(std::move(test));
    }
    if (reader.position() != bytes.size()) {
        throw SstCorpusInvalid("SST-Datei enthaelt nach dem letzten Datensatz weitere Bytes.",
                               corpus.filename,
                               reader.position());
    }
    return corpus;
}

SstCorpusFile parse_sh4_sst_file(const std::filesystem::path& path,
                                 const SstParserOptions options) {
    const auto filename = path.filename().string();
    const auto bytes = read_file_bytes(path);
    return parse_sh4_sst_bytes(bytes, filename, options);
}

SstManifest calculate_sh4_sst_manifest(const std::filesystem::path& directory) {
    try {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error) || error) {
            throw SstInfrastructureError("SST-Corpuspfad ist kein lesbares Verzeichnis.");
        }

        std::vector<std::filesystem::path> paths;
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             !error && iterator != end;
             iterator.increment(error)) {
            const auto& entry = *iterator;
            const auto filename = entry.path().filename().string();
            if (!std::string_view(filename).ends_with(".json.bin")) continue;
            std::error_code type_error;
            if (!entry.is_regular_file(type_error) || type_error) {
                throw SstInfrastructureError("SST-Manifesteintrag ist keine regulaere Datei.",
                                             filename);
            }
            if (filename.find_first_of("\t\r\n") != std::string::npos) {
                throw SstCorpusInvalid(
                    "SST-Dateiname kann nicht kanonisch im Manifest dargestellt werden.", filename);
            }
            paths.push_back(entry.path());
        }
        if (error) {
            throw SstInfrastructureError("SST-Corpusverzeichnis konnte nicht gelesen werden.");
        }
        std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
            return left.filename().string() < right.filename().string();
        });
        if (paths.empty()) {
            throw SstCorpusInvalid("SST-Corpus enthaelt keine .json.bin-Dateien.");
        }

        SstManifest manifest;
        manifest.entries.reserve(paths.size());
        for (const auto& path : paths) {
            const auto provenance = io::capture_input_provenance("sh4-sst-corpus-file", path);
            manifest.entries.push_back(
                {path.filename().string(), provenance.size, provenance.sha256});
        }
        for (const auto& entry : manifest.entries) {
            manifest.canonical_text += entry.filename;
            manifest.canonical_text.push_back('\t');
            manifest.canonical_text += std::to_string(entry.size);
            manifest.canonical_text.push_back('\t');
            manifest.canonical_text += entry.sha256;
            manifest.canonical_text.push_back('\n');
        }
        manifest.sha256 = io::sha256_bytes(manifest.canonical_text);
        return manifest;
    } catch (const SstError&) {
        throw;
    } catch (const std::exception& error) {
        throw SstInfrastructureError(std::string("SST-Manifest konnte nicht berechnet werden: ") +
                                     error.what());
    }
}

void require_pinned_sh4_sst_manifest(const SstManifest& manifest) {
    if (manifest.entries.size() != sh4_sst_corpus_file_count) {
        throw SstCorpusInvalid("SST-Manifest besitzt nicht exakt 233 Corpusdateien.");
    }
    for (const auto& entry : manifest.entries) {
        if (entry.size != sh4_sst_corpus_file_size) {
            throw SstCorpusInvalid("SST-Corpusdatei besitzt nicht exakt 394000 Bytes.",
                                   entry.filename);
        }
    }
    if (manifest.sha256 != sh4_sst_expected_manifest_sha256) {
        throw SstCorpusInvalid("SST-Manifest-Hash stimmt nicht mit dem gepinnten Corpus ueberein.");
    }
}

SstWaiverFile parse_sh4_sst_waivers_json(const std::string_view json,
                                         const std::string_view filename) {
    StrictJsonReader reader(json, std::string(filename));
    reader.expect('{', "waiver root");
    bool schema_seen = false;
    bool version_seen = false;
    bool commit_seen = false;
    bool waivers_seen = false;
    std::string schema;
    SstWaiverFile file;
    if (reader.consume('}')) reader.invalid("Waiver-Datei darf nicht leer sein.");
    while (true) {
        const auto key = reader.string("waiver root key");
        reader.expect(':', "waiver root");
        if (key == "schema") {
            require_unique_key(reader, schema_seen, key);
            schema = reader.string(key);
        } else if (key == "version") {
            require_unique_key(reader, version_seen, key);
            file.version = reader.u32(key);
        } else if (key == "corpus_commit") {
            require_unique_key(reader, commit_seen, key);
            file.corpus_commit = reader.string(key);
        } else if (key == "waivers") {
            require_unique_key(reader, waivers_seen, key);
            reader.expect('[', "waivers");
            if (!reader.consume(']')) {
                while (true) {
                    file.waivers.push_back(parse_waiver(reader));
                    if (reader.consume(']')) break;
                    reader.expect(',', "waivers");
                }
            }
        } else {
            reader.invalid("Unbekannter Waiver-Root-Schluessel: " + key);
        }
        if (reader.consume('}')) break;
        reader.expect(',', "waiver root");
    }
    reader.require_eof();
    if (!schema_seen || !version_seen || !commit_seen || !waivers_seen)
        reader.invalid("Waiver-Datei braucht schema, version, corpus_commit und waivers.");
    if (schema != "katana-sh4-sst-waivers")
        reader.invalid("Waiver-Datei besitzt ein unbekanntes Schema.");
    if (file.version != 1u)
        reader.invalid("Waiver-Datei besitzt eine nicht unterstuetzte Version.");
    if (file.corpus_commit != sh4_sst_corpus_commit)
        reader.invalid("Waiver-Datei gilt nicht fuer den gepinnten SST-Corpus-Commit.");
    reject_overlapping_waivers(reader, file.waivers);
    return file;
}

SstWaiverFile parse_sh4_sst_waivers_file(const std::filesystem::path& path) {
    const auto bytes = read_file_bytes(path);
    const auto json = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return parse_sh4_sst_waivers_json(json, path.filename().string());
}

void import_sst_state(const SstState& source, runtime::CpuState& destination) noexcept {
    destination.r = source.r;
    destination.r_bank = source.r_bank;
    destination.pc = source.pc;
    destination.gbr = source.gbr;
    destination.sr = source.sr;
    destination.t = (source.sr & runtime::sr_t_mask) != 0u;
    destination.s = (source.sr & runtime::sr_s_mask) != 0u;
    destination.q = (source.sr & runtime::sr_q_mask) != 0u;
    destination.m = (source.sr & runtime::sr_m_mask) != 0u;
    destination.ssr = source.ssr;
    destination.spc = source.spc;
    destination.vbr = source.vbr;
    destination.sgr = source.sgr;
    destination.dbr = source.dbr;
    destination.macl = source.macl;
    destination.mach = source.mach;
    destination.pr = source.pr;
    destination.fpscr = source.fpscr;
    destination.fpul = source.fpul;
    if ((source.fpscr & runtime::fpscr_fr_mask) == 0u) {
        destination.fr = source.fp0;
        destination.xf = source.fp1;
    } else {
        destination.fr = source.fp1;
        destination.xf = source.fp0;
    }
}

SstState export_sst_state(const runtime::CpuState& source) noexcept {
    SstState destination;
    destination.r = source.r;
    destination.r_bank = source.r_bank;
    if ((source.read_fpscr() & runtime::fpscr_fr_mask) == 0u) {
        destination.fp0 = source.fr;
        destination.fp1 = source.xf;
    } else {
        destination.fp0 = source.xf;
        destination.fp1 = source.fr;
    }
    destination.pc = source.pc;
    destination.gbr = source.gbr;
    destination.sr = source.read_sr();
    destination.ssr = source.ssr;
    destination.spc = source.spc;
    destination.vbr = source.vbr;
    destination.sgr = source.sgr;
    destination.dbr = source.dbr;
    destination.macl = source.macl;
    destination.mach = source.mach;
    destination.pr = source.pr;
    destination.fpscr = source.read_fpscr();
    destination.fpul = source.fpul;
    return destination;
}

SstStateComparison compare_sst_states(const SstState& expected,
                                      const SstState& actual,
                                      const FpuComparisonMode fpu_mode) {
    SstStateComparison comparison;
    compare_words(comparison, "R", expected.r, actual.r);
    compare_words(comparison, "R_", expected.r_bank, actual.r_bank);
    compare_fpu_words(comparison, "FP0", expected.fp0, actual.fp0, fpu_mode);
    compare_fpu_words(comparison, "FP1", expected.fp1, actual.fp1, fpu_mode);
    add_difference(comparison, "PC", expected.pc, actual.pc);
    add_difference(comparison, "GBR", expected.gbr, actual.gbr);
    add_difference(comparison, "SR", expected.sr, actual.sr);
    add_difference(comparison, "SSR", expected.ssr, actual.ssr);
    add_difference(comparison, "SPC", expected.spc, actual.spc);
    add_difference(comparison, "VBR", expected.vbr, actual.vbr);
    add_difference(comparison, "SGR", expected.sgr, actual.sgr);
    add_difference(comparison, "DBR", expected.dbr, actual.dbr);
    add_difference(comparison, "MACL", expected.macl, actual.macl);
    add_difference(comparison, "MACH", expected.mach, actual.mach);
    add_difference(comparison, "PR", expected.pr, actual.pr);
    add_difference(comparison, "FPSCR", expected.fpscr, actual.fpscr);
    add_difference(comparison, "FPUL", expected.fpul, actual.fpul);
    return comparison;
}

SstStateComparison import_and_verify_sst_state(const SstState& source,
                                               runtime::CpuState& destination) {
    import_sst_state(source, destination);
    return compare_sst_states(source, export_sst_state(destination), FpuComparisonMode::Strict);
}

void require_sst_setup_round_trip(const SstStateComparison& comparison) {
    if (comparison.matches()) return;
    const auto& first = comparison.differences.front();
    throw SstHarnessInvalid("SST-Zustandsadapter verlor beim Setup das Feld " + first.path + '.');
}

SstInternalStateSnapshot initialize_sst_internal_canaries(runtime::CpuState& cpu) noexcept {
    cpu.tra = 0x53535401u;
    cpu.tea = 0x53535402u;
    cpu.expevt = 0x53535403u;
    cpu.intevt = 0x53535404u;
    cpu.pteh = 0u;
    cpu.ptel = 0u;
    cpu.ptea = 0u;
    cpu.ttb = 0x53535405u;
    cpu.mmucr = 0u;
    for (std::size_t index = 0u; index < cpu.utlb.size(); ++index) {
        cpu.utlb[index] = {0x53540000u + static_cast<std::uint32_t>(index),
                           0x53550000u + static_cast<std::uint32_t>(index),
                           0x53560000u + static_cast<std::uint32_t>(index)};
    }
    cpu.tlb_load_count = 0x5353540000000001ull;
    cpu.trap_pending = false;
    cpu.exception_generation = 0x5353540000000003ull;
    cpu.last_exception_cause = runtime::ExceptionCause::None;
    cpu.exception_in_delay_slot = false;
    cpu.last_exception_instruction_pc = 0x53535406u;
    cpu.last_exception_instruction_physical_pc = 0x53535407u;
    cpu.last_exception_owner_pc = 0x53535408u;
    cpu.last_exception_generation = 0x5353540000000002ull;
    cpu.sleeping = false;
    cpu.last_prefetch_address = 0x53535409u;
    cpu.prefetch_count = 0x5353540000000004ull;
    cpu.attempted_guest_instructions = 0x5353540000000020ull;
    cpu.retired_guest_instructions = 0x5353540000000010ull;
    cpu.total_guest_cycles = 0x5353540000000030ull;
    cpu.pending_guest_cycles = 0u;
    cpu.active_instruction_pc = 0x5353540Au;
    cpu.active_instruction_physical_pc = 0x5353540Bu;
    cpu.active_block_virtual_start = 0x5353540Cu;
    cpu.active_block_physical_start = 0x5353540Du;
    cpu.active_block_size = 0u;
    cpu.last_prefetch_was_store_queue = false;
    return capture_sst_internal_state(cpu);
}

SstInternalStateSnapshot capture_sst_internal_state(const runtime::CpuState& cpu) noexcept {
    SstInternalStateSnapshot snapshot;
    snapshot.tra = cpu.tra;
    snapshot.tea = cpu.tea;
    snapshot.expevt = cpu.expevt;
    snapshot.intevt = cpu.intevt;
    snapshot.pteh = cpu.pteh;
    snapshot.ptel = cpu.ptel;
    snapshot.ptea = cpu.ptea;
    snapshot.ttb = cpu.ttb;
    snapshot.mmucr = cpu.mmucr;
    snapshot.utlb = cpu.utlb;
    snapshot.tlb_load_count = cpu.tlb_load_count;
    snapshot.trap_pending = cpu.trap_pending;
    snapshot.exception_generation = cpu.exception_generation;
    snapshot.last_exception_cause = cpu.last_exception_cause;
    snapshot.exception_in_delay_slot = cpu.exception_in_delay_slot;
    snapshot.last_exception_instruction_pc = cpu.last_exception_instruction_pc;
    snapshot.last_exception_instruction_physical_pc = cpu.last_exception_instruction_physical_pc;
    snapshot.last_exception_owner_pc = cpu.last_exception_owner_pc;
    snapshot.last_exception_generation = cpu.last_exception_generation;
    snapshot.sleeping = cpu.sleeping;
    snapshot.last_prefetch_address = cpu.last_prefetch_address;
    snapshot.prefetch_count = cpu.prefetch_count;
    snapshot.attempted_guest_instructions = cpu.attempted_guest_instructions;
    snapshot.retired_guest_instructions = cpu.retired_guest_instructions;
    snapshot.total_guest_cycles = cpu.total_guest_cycles;
    snapshot.pending_guest_cycles = cpu.pending_guest_cycles;
    snapshot.active_instruction_pc = cpu.active_instruction_pc;
    snapshot.active_instruction_physical_pc = cpu.active_instruction_physical_pc;
    snapshot.active_block_virtual_start = cpu.active_block_virtual_start;
    snapshot.active_block_physical_start = cpu.active_block_physical_start;
    snapshot.active_block_size = cpu.active_block_size;
    snapshot.last_prefetch_was_store_queue = cpu.last_prefetch_was_store_queue;
    snapshot.manual_reset_sink = cpu.manual_reset_sink;
    snapshot.address_space_identity = cpu.address_space.get();
    snapshot.gdrom_services_identity = cpu.gdrom_services;
    snapshot.g1_bus_identity = cpu.g1_bus;
    snapshot.memory_size = cpu.memory.size();
    snapshot.memory_region_count = cpu.memory.region_count();
    snapshot.memory_alignment = cpu.memory.alignment_policy();
    snapshot.memory_lookup = cpu.memory.lookup_mode();
    snapshot.memory_watchpoint_count = cpu.memory.watchpoint_count();
    snapshot.memory_trace_handler = cpu.memory.has_trace_handler();
    snapshot.memory_mmio_tracking = cpu.memory.mmio_access_tracking_enabled();
    snapshot.memory_mmio_trace_handler = cpu.memory.has_mmio_trace_handler();
    snapshot.memory_guest_write_observer = cpu.memory.has_guest_write_observer();
    snapshot.memory_guest_access_sink = cpu.memory.has_guest_memory_access_sink();
    return snapshot;
}

SstStateComparison compare_sst_internal_state(const SstInternalStateSnapshot& expected,
                                              const runtime::CpuState& actual) {
    const auto observed = capture_sst_internal_state(actual);
    SstStateComparison comparison;
#define KATANA_COMPARE_INTERNAL(field)                                                             \
    compare_internal_scalar(comparison, "internal." #field, expected.field, observed.field)
    KATANA_COMPARE_INTERNAL(tra);
    KATANA_COMPARE_INTERNAL(tea);
    KATANA_COMPARE_INTERNAL(expevt);
    KATANA_COMPARE_INTERNAL(intevt);
    KATANA_COMPARE_INTERNAL(pteh);
    KATANA_COMPARE_INTERNAL(ptel);
    KATANA_COMPARE_INTERNAL(ptea);
    KATANA_COMPARE_INTERNAL(ttb);
    KATANA_COMPARE_INTERNAL(mmucr);
    for (std::size_t index = 0u; index < expected.utlb.size(); ++index) {
        const auto prefix = "internal.utlb[" + std::to_string(index) + "].";
        add_difference(
            comparison, prefix + "pteh", expected.utlb[index].pteh, observed.utlb[index].pteh);
        add_difference(
            comparison, prefix + "ptel", expected.utlb[index].ptel, observed.utlb[index].ptel);
        add_difference(
            comparison, prefix + "ptea", expected.utlb[index].ptea, observed.utlb[index].ptea);
    }
    KATANA_COMPARE_INTERNAL(tlb_load_count);
    KATANA_COMPARE_INTERNAL(trap_pending);
    KATANA_COMPARE_INTERNAL(exception_generation);
    KATANA_COMPARE_INTERNAL(last_exception_cause);
    KATANA_COMPARE_INTERNAL(exception_in_delay_slot);
    KATANA_COMPARE_INTERNAL(last_exception_instruction_pc);
    KATANA_COMPARE_INTERNAL(last_exception_instruction_physical_pc);
    KATANA_COMPARE_INTERNAL(last_exception_owner_pc);
    KATANA_COMPARE_INTERNAL(last_exception_generation);
    KATANA_COMPARE_INTERNAL(sleeping);
    KATANA_COMPARE_INTERNAL(last_prefetch_address);
    KATANA_COMPARE_INTERNAL(prefetch_count);
    KATANA_COMPARE_INTERNAL(attempted_guest_instructions);
    KATANA_COMPARE_INTERNAL(retired_guest_instructions);
    KATANA_COMPARE_INTERNAL(total_guest_cycles);
    KATANA_COMPARE_INTERNAL(pending_guest_cycles);
    KATANA_COMPARE_INTERNAL(active_instruction_pc);
    KATANA_COMPARE_INTERNAL(active_instruction_physical_pc);
    KATANA_COMPARE_INTERNAL(active_block_virtual_start);
    KATANA_COMPARE_INTERNAL(active_block_physical_start);
    KATANA_COMPARE_INTERNAL(active_block_size);
    KATANA_COMPARE_INTERNAL(last_prefetch_was_store_queue);
    KATANA_COMPARE_INTERNAL(memory_size);
    KATANA_COMPARE_INTERNAL(memory_region_count);
    KATANA_COMPARE_INTERNAL(memory_alignment);
    KATANA_COMPARE_INTERNAL(memory_lookup);
    KATANA_COMPARE_INTERNAL(memory_watchpoint_count);
    KATANA_COMPARE_INTERNAL(memory_trace_handler);
    KATANA_COMPARE_INTERNAL(memory_mmio_tracking);
    KATANA_COMPARE_INTERNAL(memory_mmio_trace_handler);
    KATANA_COMPARE_INTERNAL(memory_guest_write_observer);
    KATANA_COMPARE_INTERNAL(memory_guest_access_sink);
#undef KATANA_COMPARE_INTERNAL

    if (expected.manual_reset_sink.context != observed.manual_reset_sink.context) {
        add_difference(comparison,
                       "internal.manual_reset_sink.context",
                       pointer_value(expected.manual_reset_sink.context),
                       pointer_value(observed.manual_reset_sink.context));
    }
    if (expected.manual_reset_sink.callback != observed.manual_reset_sink.callback) {
        comparison.differences.push_back(
            {"internal.manual_reset_sink.callback",
             expected.manual_reset_sink.callback == nullptr ? 0u : 1u,
             observed.manual_reset_sink.callback == nullptr ? 0u : 1u});
    }
    add_difference(comparison,
                   "internal.address_space",
                   pointer_value(expected.address_space_identity),
                   pointer_value(observed.address_space_identity));
    add_difference(comparison,
                   "internal.gdrom_services",
                   pointer_value(expected.gdrom_services_identity),
                   pointer_value(observed.gdrom_services_identity));
    add_difference(comparison,
                   "internal.g1_bus",
                   pointer_value(expected.g1_bus_identity),
                   pointer_value(observed.g1_bus_identity));
    return comparison;
}

SstStateComparison compare_sst_internal_state_after_success(
    const SstInternalStateSnapshot& before,
    const runtime::CpuState& after,
    const SstSuccessfulExecutionExpectation& expected_progress) {
    auto comparison = compare_sst_internal_state(before, after);
    constexpr std::array allowed_progress_fields{
        std::string_view{"internal.attempted_guest_instructions"},
        std::string_view{"internal.retired_guest_instructions"},
        std::string_view{"internal.total_guest_cycles"},
        std::string_view{"internal.pending_guest_cycles"},
        std::string_view{"internal.active_instruction_pc"},
        std::string_view{"internal.active_instruction_physical_pc"},
    };
    std::erase_if(comparison.differences, [](const SstStateDifference& difference) {
        return std::find(allowed_progress_fields.begin(),
                         allowed_progress_fields.end(),
                         difference.path) != allowed_progress_fields.end();
    });

    constexpr auto executed_instructions = static_cast<std::uint64_t>(sh4_sst_cycle_count);
    add_difference(comparison,
                   "internal.attempted_guest_instructions",
                   before.attempted_guest_instructions + executed_instructions,
                   after.attempted_guest_instructions);
    add_difference(comparison,
                   "internal.retired_guest_instructions",
                   before.retired_guest_instructions + executed_instructions,
                   after.retired_guest_instructions);

    const auto elapsed_before = before.total_guest_cycles + before.pending_guest_cycles;
    const auto elapsed_after = after.total_guest_cycles + after.pending_guest_cycles;
    add_difference(comparison,
                   "internal.elapsed_guest_cycles",
                   elapsed_before + expected_progress.guest_cycle_delta,
                   elapsed_after);
    if (after.total_guest_cycles < before.total_guest_cycles) {
        comparison.differences.push_back(
            {"internal.total_guest_cycles", before.total_guest_cycles, after.total_guest_cycles});
    }
    add_difference(comparison,
                   "internal.active_instruction_pc",
                   expected_progress.last_instruction_pc,
                   after.active_instruction_pc);
    add_difference(comparison,
                   "internal.active_instruction_physical_pc",
                   expected_progress.last_instruction_physical_pc,
                   after.active_instruction_physical_pc);
    return comparison;
}

std::string format_sh4_sst_report_json(const SstReportBasis& report) {
    if (report.katana_commit.empty() || report.corpus_commit.empty() || report.scope.empty()) {
        throw SstHarnessInvalid(
            "SST-Bericht braucht Katana-Commit, Corpus-Commit und expliziten Scope.");
    }
    if (report.backend_profile != "external-conformance") {
        throw SstHarnessInvalid(
            "SST-Bericht muss das Backendprofil external-conformance ausweisen.");
    }
    if (!lowercase_sha256(report.corpus_manifest_sha256))
        throw SstHarnessInvalid("SST-Bericht braucht einen lowercase SHA-256-Corpushash.");
    const auto expected_scope_vectors = [&]() -> std::uint64_t {
        if (report.scope == "smoke")
            return katana::sh4::external_evidence_contract::smoke_vector_count;
        if (report.scope == "full")
            return katana::sh4::external_evidence_contract::full_vector_count;
        throw SstHarnessInvalid("SST-Bericht besitzt einen unbekannten Scope.");
    }();
    if (report.selection.expected_scope_vectors != expected_scope_vectors ||
        report.total_vectors > expected_scope_vectors) {
        throw SstHarnessInvalid("SST-Bericht besitzt einen widerspruechlichen Scope-Nenner.");
    }
    if (report.selection.shard.has_value() != report.selection.shard_count.has_value()) {
        throw SstHarnessInvalid("SST-Bericht braucht Shardindex und Shardanzahl gemeinsam.");
    }
    if (report.selection.shard_count &&
        (*report.selection.shard_count == 0u ||
         *report.selection.shard >= *report.selection.shard_count)) {
        throw SstHarnessInvalid("SST-Bericht besitzt eine ungueltige Shardauswahl.");
    }
    const bool selection_filtered = report.selection.filename || report.selection.case_index ||
                                    report.selection.opcode || report.selection.family ||
                                    report.selection.shard || report.selection.shard_count;
    if (report.selection.complete_scope && (selection_filtered || report.selection.fail_fast ||
                                            report.total_vectors != expected_scope_vectors)) {
        throw SstHarnessInvalid(
            "SST-Bericht markiert eine unvollstaendige Auswahl als kompletten Scope.");
    }
    if (report.applicable_vectors > report.total_vectors ||
        report.passed_vectors > report.applicable_vectors ||
        report.failed_vectors > report.applicable_vectors - report.passed_vectors) {
        throw SstHarnessInvalid("SST-Bericht besitzt widerspruechliche Vektorenzaehler.");
    }

    constexpr std::array classifications{
        ResultClassification::Pass,
        ResultClassification::FailState,
        ResultClassification::FailControlFlow,
        ResultClassification::FailDelaySlot,
        ResultClassification::FailMemoryAddress,
        ResultClassification::FailMemoryWidth,
        ResultClassification::FailMemoryValue,
        ResultClassification::FailMemoryOrder,
        ResultClassification::FailExtraSideEffect,
        ResultClassification::FailUnboundTarget,
        ResultClassification::FailUnexpectedException,
        ResultClassification::NotApplicableReferenceAlignment,
        ResultClassification::NotApplicableReferenceException,
        ResultClassification::NotApplicableReferenceMmio,
        ResultClassification::NotApplicableReferenceKnownBug,
        ResultClassification::NotApplicableKatanaRestricted,
        ResultClassification::NotApplicableAccessShape,
        ResultClassification::CorpusInvalid,
        ResultClassification::HarnessInvalid,
        ResultClassification::InfrastructureError,
    };
    std::array<std::uint64_t, classifications.size()> counts{};
    std::array<bool, classifications.size()> seen{};
    for (const auto& entry : report.classifications) {
        const auto found =
            std::find(classifications.begin(), classifications.end(), entry.classification);
        if (found == classifications.end())
            throw SstHarnessInvalid("SST-Bericht enthaelt eine unbekannte Klassifikation.");
        const auto index = static_cast<std::size_t>(found - classifications.begin());
        if (seen[index])
            throw SstHarnessInvalid("SST-Bericht enthaelt eine Klassifikation doppelt.");
        seen[index] = true;
        counts[index] = entry.count;
    }
    if (!seen[0]) counts[0] = report.passed_vectors;
    if (counts[0] != report.passed_vectors)
        throw SstHarnessInvalid("SST-Passzaehler widerspricht classification_counts.pass.");
    const auto sum_range = [&](const std::size_t first, const std::size_t last) {
        std::uint64_t total = 0u;
        for (auto index = first; index <= last; ++index)
            total += counts[index];
        return total;
    };
    const auto classified_failures = sum_range(1u, 10u);
    const auto classified_not_applicable = sum_range(11u, 16u);
    const auto classified_invalid = sum_range(17u, 19u);
    if (classified_failures != report.failed_vectors ||
        report.passed_vectors + classified_failures != report.applicable_vectors ||
        report.applicable_vectors + classified_not_applicable != report.total_vectors ||
        classified_invalid != 0u) {
        throw SstHarnessInvalid(
            "SST-Bericht besitzt keinen vollstaendigen, widerspruchsfreien Nenner.");
    }

    std::ostringstream output;
    io::write_json_report_header(output,
                                 "katana-sh4-sst-conformance",
                                 "sh4-sst-conformance",
                                 report.failed_vectors == 0u ? "success" : "failure");
    output << ",\"katana_commit\":" << io::quote_json(report.katana_commit)
           << ",\"corpus_commit\":" << io::quote_json(report.corpus_commit)
           << ",\"corpus_manifest_sha256\":" << io::quote_json(report.corpus_manifest_sha256)
           << ",\"compiler\":" << io::quote_json(report.compiler)
           << ",\"build_type\":" << io::quote_json(report.build_type)
           << ",\"host_platform\":" << io::quote_json(report.host_platform)
           << ",\"lto\":" << (report.lto ? "true" : "false")
           << ",\"runtime_abi\":" << report.runtime_abi << ",\"backend_abi\":" << report.backend_abi
           << ",\"backend_profile\":" << io::quote_json(report.backend_profile)
           << ",\"backend_profile_version\":" << report.backend_profile_version
           << ",\"generated_native_code_forms\":" << report.generated_native_code_forms
           << ",\"scope\":" << io::quote_json(report.scope)
           << ",\"memory_profile\":" << io::quote_json(memory_profile_name(report.memory_profile))
           << ",\"fpu_comparison_mode\":"
           << io::quote_json(fpu_comparison_mode_name(report.fpu_comparison))
           << ",\"selection\":{\"complete_scope\":"
           << (report.selection.complete_scope ? "true" : "false")
           << ",\"expected_scope_vectors\":" << report.selection.expected_scope_vectors
           << ",\"file\":";
    if (report.selection.filename) {
        output << io::quote_json(*report.selection.filename);
    } else {
        output << "null";
    }
    output << ",\"case\":";
    if (report.selection.case_index) {
        output << *report.selection.case_index;
    } else {
        output << "null";
    }
    output << ",\"opcode\":";
    if (report.selection.opcode) {
        output << *report.selection.opcode;
    } else {
        output << "null";
    }
    output << ",\"family\":";
    if (report.selection.family) {
        output << io::quote_json(*report.selection.family);
    } else {
        output << "null";
    }
    output << ",\"shard\":";
    if (report.selection.shard && report.selection.shard_count) {
        output << "{\"index\":" << *report.selection.shard
               << ",\"count\":" << *report.selection.shard_count << '}';
    } else {
        output << "null";
    }
    output << ",\"fail_fast\":" << (report.selection.fail_fast ? "true" : "false") << '}'
           << ",\"counts\":{\"total\":" << report.total_vectors
           << ",\"applicable\":" << report.applicable_vectors
           << ",\"passed\":" << report.passed_vectors << ",\"failed\":" << report.failed_vectors
           << '}' << ",\"classification_counts\":{";
    for (std::size_t index = 0u; index < classifications.size(); ++index) {
        if (index != 0u) output << ',';
        output << io::quote_json(result_classification_name(classifications[index])) << ':'
               << counts[index];
    }
    output << "},\"used_files\":";
    write_string_array(output, report.used_files);
    output << ",\"represented_opcodes\":";
    write_opcode_array(output, report.represented_opcodes);
    output << ",\"katana_opcodes_without_external_evidence\":";
    write_opcode_array(output, report.katana_opcodes_without_external_evidence);

    auto waivers = report.waivers;
    std::sort(waivers.begin(), waivers.end(), [](const auto& left, const auto& right) {
        if (left.filename != right.filename) return left.filename < right.filename;
        const auto left_index = !left.case_indices.empty()
                                    ? left.case_indices.front()
                                    : left.case_range.value_or(SstCaseRange{}).first;
        const auto right_index = !right.case_indices.empty()
                                     ? right.case_indices.front()
                                     : right.case_range.value_or(SstCaseRange{}).first;
        return left_index < right_index;
    });
    output << ",\"waivers\":[";
    for (std::size_t index = 0u; index < waivers.size(); ++index) {
        if (index != 0u) output << ',';
        write_waiver_json(output, waivers[index]);
    }
    output << "],\"first_counterexamples\":[";
    const auto write_memory = [&](const std::vector<SstMemoryObservation>& observations) {
        output << '[';
        for (std::size_t index = 0u; index < observations.size(); ++index) {
            if (index != 0u) output << ',';
            const auto& item = observations[index];
            output << "{\"executing_guest_pc\":" << item.executing_guest_pc
                   << ",\"order\":" << item.order << ",\"operation\":"
                   << io::quote_json(item.operation == SstMemoryOperation::Read ? "read" : "write")
                   << ",\"address\":" << item.address << ",\"width\":" << item.width
                   << ",\"value\":" << item.value << '}';
        }
        output << ']';
    };
    for (std::size_t index = 0u; index < report.first_counterexamples.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& item = report.first_counterexamples[index];
        output << "{\"filename\":" << io::quote_json(item.filename)
               << ",\"case_index\":" << item.case_index << ",\"opcode\":" << item.opcode
               << ",\"classification\":"
               << io::quote_json(result_classification_name(item.classification))
               << ",\"detail\":" << io::quote_json(item.detail) << ",\"state_differences\":[";
        for (std::size_t difference = 0u; difference < item.state_differences.size();
             ++difference) {
            if (difference != 0u) output << ',';
            const auto& state = item.state_differences[difference];
            output << "{\"path\":" << io::quote_json(state.path)
                   << ",\"expected\":" << state.expected << ",\"actual\":" << state.actual << '}';
        }
        output << "],\"expected_memory\":";
        write_memory(item.expected_memory);
        output << ",\"actual_memory\":";
        write_memory(item.actual_memory);
        output << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace katana::testing
