#include "katana/sh4/isa_coverage.hpp"

#include "katana/build_contract.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/native_aot_profile.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/external_evidence_contract.hpp"
#include "katana/sh4/instruction_metadata.hpp"

#include "katana/io/json_report.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace katana::sh4 {
namespace {

struct MutableCoverage {
    std::string name;
    std::size_t rule_count = 0;
    std::uint32_t opcode_count = 0;
    bool privileged = false;
    ControlFlowKind control_flow = ControlFlowKind::None;
};

enum class EvidenceJsonKind : std::uint8_t {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

struct EvidenceJsonValue {
    EvidenceJsonKind kind = EvidenceJsonKind::Null;
    bool boolean = false;
    std::string scalar;
    std::vector<EvidenceJsonValue> array;
    std::map<std::string, EvidenceJsonValue, std::less<>> object;

    [[nodiscard]] const EvidenceJsonValue* find(const std::string_view key) const noexcept {
        const auto found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }
};

class EvidenceJsonParser final {
  public:
    explicit EvidenceJsonParser(const std::string_view document) : document_(document) {
        constexpr std::size_t maximum_document_size = 16u * 1024u * 1024u;
        if (document_.size() > maximum_document_size) fail("Dokument ist groesser als 16 MiB.");
    }

    [[nodiscard]] EvidenceJsonValue parse() {
        skip_whitespace();
        auto result = value(0u);
        skip_whitespace();
        if (offset_ != document_.size()) fail("Unerwartete Daten hinter dem Wurzelwert.");
        return result;
    }

  private:
    [[noreturn]] void fail(const std::string& detail) const {
        throw std::invalid_argument("Ungueltiger externer SST-Evidencebericht bei Byte " +
                                    std::to_string(offset_) + ": " + detail);
    }

    void skip_whitespace() noexcept {
        while (offset_ < document_.size()) {
            const char character = document_[offset_];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n')
                break;
            ++offset_;
        }
    }

    [[nodiscard]] bool consume(const char expected) noexcept {
        if (offset_ >= document_.size() || document_[offset_] != expected) return false;
        ++offset_;
        return true;
    }

    void require(const char expected, const std::string_view description) {
        if (!consume(expected)) fail(std::string(description));
    }

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

    [[nodiscard]] std::uint32_t hex_quad() {
        if (document_.size() - offset_ < 4u) fail("Unvollstaendige Unicode-Escape-Sequenz.");
        std::uint32_t result = 0u;
        for (std::size_t index = 0u; index < 4u; ++index) {
            const unsigned char character = static_cast<unsigned char>(document_[offset_++]);
            result <<= 4u;
            if (character >= '0' && character <= '9')
                result |= character - '0';
            else if (character >= 'a' && character <= 'f')
                result |= character - 'a' + 10u;
            else if (character >= 'A' && character <= 'F')
                result |= character - 'A' + 10u;
            else
                fail("Ungueltige Unicode-Escape-Sequenz.");
        }
        return result;
    }

    [[nodiscard]] std::string string() {
        require('"', "JSON-String erwartet.");
        std::string result;
        constexpr std::size_t maximum_string_size = 1024u * 1024u;
        while (offset_ < document_.size()) {
            const unsigned char character = static_cast<unsigned char>(document_[offset_++]);
            if (character == '"') return result;
            if (character < 0x20u) fail("Steuerzeichen in JSON-String.");
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                if (result.size() > maximum_string_size) fail("JSON-String ist zu gross.");
                continue;
            }
            if (offset_ >= document_.size()) fail("Unvollstaendige Escape-Sequenz.");
            switch (document_[offset_++]) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
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
            case 'u': {
                auto codepoint = hex_quad();
                if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                    if (!consume('\\') || !consume('u'))
                        fail("Hohes Unicode-Surrogat besitzt keinen Partner.");
                    const auto low = hex_quad();
                    if (low < 0xDC00u || low > 0xDFFFu)
                        fail("Ungueltiges niedriges Unicode-Surrogat.");
                    codepoint = 0x10000u + ((codepoint - 0xD800u) << 10u) + (low - 0xDC00u);
                } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                    fail("Niedriges Unicode-Surrogat ohne hohen Partner.");
                }
                append_utf8(result, codepoint);
                break;
            }
            default:
                fail("Unbekannte Escape-Sequenz.");
            }
            if (result.size() > maximum_string_size) fail("JSON-String ist zu gross.");
        }
        fail("Nicht abgeschlossener JSON-String.");
    }

    [[nodiscard]] EvidenceJsonValue number() {
        const auto begin = offset_;
        static_cast<void>(consume('-'));
        if (offset_ >= document_.size()) fail("Unvollstaendige JSON-Zahl.");
        if (consume('0')) {
            if (offset_ < document_.size() &&
                std::isdigit(static_cast<unsigned char>(document_[offset_])) != 0)
                fail("JSON-Zahl besitzt eine fuehrende Null.");
        } else {
            if (document_[offset_] < '1' || document_[offset_] > '9') fail("JSON-Zahl erwartet.");
            while (offset_ < document_.size() &&
                   std::isdigit(static_cast<unsigned char>(document_[offset_])) != 0)
                ++offset_;
        }
        if (consume('.')) {
            const auto fraction_begin = offset_;
            while (offset_ < document_.size() &&
                   std::isdigit(static_cast<unsigned char>(document_[offset_])) != 0)
                ++offset_;
            if (offset_ == fraction_begin) fail("Leerer Bruchteil in JSON-Zahl.");
        }
        if (offset_ < document_.size() &&
            (document_[offset_] == 'e' || document_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < document_.size() &&
                (document_[offset_] == '+' || document_[offset_] == '-'))
                ++offset_;
            const auto exponent_begin = offset_;
            while (offset_ < document_.size() &&
                   std::isdigit(static_cast<unsigned char>(document_[offset_])) != 0)
                ++offset_;
            if (offset_ == exponent_begin) fail("Leerer Exponent in JSON-Zahl.");
        }
        EvidenceJsonValue result;
        result.kind = EvidenceJsonKind::Number;
        result.scalar = std::string(document_.substr(begin, offset_ - begin));
        return result;
    }

    void literal(const std::string_view expected) {
        if (document_.substr(offset_, expected.size()) != expected)
            fail("Unbekanntes JSON-Literal.");
        offset_ += expected.size();
    }

    [[nodiscard]] EvidenceJsonValue value(const std::size_t depth) {
        constexpr std::size_t maximum_depth = 64u;
        constexpr std::size_t maximum_nodes = 1'000'000u;
        if (depth > maximum_depth) fail("JSON-Verschachtelung ist zu tief.");
        if (++node_count_ > maximum_nodes) fail("JSON enthaelt zu viele Werte.");
        skip_whitespace();
        if (offset_ >= document_.size()) fail("JSON-Wert fehlt.");

        if (document_[offset_] == '"') {
            EvidenceJsonValue result;
            result.kind = EvidenceJsonKind::String;
            result.scalar = string();
            return result;
        }
        if (document_[offset_] == '-' ||
            std::isdigit(static_cast<unsigned char>(document_[offset_])) != 0)
            return number();
        if (document_[offset_] == 'n') {
            literal("null");
            return {};
        }
        if (document_[offset_] == 't') {
            literal("true");
            EvidenceJsonValue result;
            result.kind = EvidenceJsonKind::Boolean;
            result.boolean = true;
            return result;
        }
        if (document_[offset_] == 'f') {
            literal("false");
            EvidenceJsonValue result;
            result.kind = EvidenceJsonKind::Boolean;
            return result;
        }
        if (consume('[')) {
            EvidenceJsonValue result;
            result.kind = EvidenceJsonKind::Array;
            skip_whitespace();
            if (consume(']')) return result;
            while (true) {
                result.array.push_back(value(depth + 1u));
                skip_whitespace();
                if (consume(']')) return result;
                require(',', "Komma oder Arrayende erwartet.");
                skip_whitespace();
            }
        }
        if (consume('{')) {
            EvidenceJsonValue result;
            result.kind = EvidenceJsonKind::Object;
            skip_whitespace();
            if (consume('}')) return result;
            while (true) {
                if (offset_ >= document_.size() || document_[offset_] != '"')
                    fail("Objektschluessel muss ein String sein.");
                auto key = string();
                if (result.find(key) != nullptr)
                    fail("Doppelter Objektschluessel ist nicht erlaubt.");
                skip_whitespace();
                require(':', "Doppelpunkt nach Objektschluessel erwartet.");
                skip_whitespace();
                result.object.emplace(std::move(key), value(depth + 1u));
                skip_whitespace();
                if (consume('}')) return result;
                require(',', "Komma oder Objektende erwartet.");
                skip_whitespace();
            }
        }
        fail("Unbekannter JSON-Wert.");
    }

    std::string_view document_;
    std::size_t offset_ = 0u;
    std::size_t node_count_ = 0u;
};

[[nodiscard]] const EvidenceJsonValue& evidence_member(const EvidenceJsonValue& object,
                                                       const std::string_view name) {
    if (object.kind != EvidenceJsonKind::Object)
        throw std::invalid_argument("Externer SST-Evidencebericht erwartet ein JSON-Objekt.");
    const auto* value = object.find(name);
    if (value == nullptr)
        throw std::invalid_argument("Externer SST-Evidencebericht vermisst Pflichtfeld '" +
                                    std::string(name) + "'.");
    return *value;
}

[[nodiscard]] std::string evidence_string(const EvidenceJsonValue& object,
                                          const std::string_view name) {
    const auto& value = evidence_member(object, name);
    if (value.kind != EvidenceJsonKind::String)
        throw std::invalid_argument("SST-Evidencefeld '" + std::string(name) +
                                    "' muss ein String sein.");
    return value.scalar;
}

[[nodiscard]] std::uint64_t evidence_unsigned(const EvidenceJsonValue& object,
                                              const std::string_view name) {
    const auto& value = evidence_member(object, name);
    if (value.kind != EvidenceJsonKind::Number || value.scalar.empty() ||
        !std::all_of(value.scalar.begin(), value.scalar.end(), [](const unsigned char character) {
            return character >= '0' && character <= '9';
        })) {
        throw std::invalid_argument("SST-Evidencefeld '" + std::string(name) +
                                    "' muss eine vorzeichenlose Ganzzahl sein.");
    }
    std::uint64_t result = 0u;
    for (const char character : value.scalar) {
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
            throw std::invalid_argument("SST-Evidencefeld '" + std::string(name) +
                                        "' ist zu gross.");
        result = result * 10u + digit;
    }
    return result;
}

[[nodiscard]] std::uint32_t evidence_u32(const EvidenceJsonValue& object,
                                         const std::string_view name) {
    const auto value = evidence_unsigned(object, name);
    if (value == 0u || value > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("SST-Evidencefeld '" + std::string(name) +
                                    "' liegt ausserhalb des Vertragsbereichs.");
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool evidence_boolean(const EvidenceJsonValue& object, const std::string_view name) {
    const auto& value = evidence_member(object, name);
    if (value.kind != EvidenceJsonKind::Boolean)
        throw std::invalid_argument("SST-Evidencefeld '" + std::string(name) +
                                    "' muss ein Boolean sein.");
    return value.boolean;
}

[[nodiscard]] bool lowercase_hex(const std::string_view value, const std::size_t size) noexcept {
    return value.size() == size &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool safe_contract_token(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 64u &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_' ||
                      character == '.';
           });
}

void require_exact_string(const EvidenceJsonValue& object,
                          const std::string_view name,
                          const std::string_view expected) {
    if (evidence_string(object, name) != expected)
        throw std::invalid_argument("Inkompatibler externer SST-Evidencevertrag: '" +
                                    std::string(name) + "'.");
}

[[nodiscard]] std::optional<std::uint64_t>
expected_external_scope_vectors(const std::string_view scope) noexcept {
    if (scope == "smoke") return external_evidence_contract::smoke_vector_count;
    if (scope == "full") return external_evidence_contract::full_vector_count;
    return std::nullopt;
}

inline constexpr std::array<std::string_view, 20u> external_classification_names{
    "pass",
    "fail-state",
    "fail-control-flow",
    "fail-delay-slot",
    "fail-memory-address",
    "fail-memory-width",
    "fail-memory-value",
    "fail-memory-order",
    "fail-extra-side-effect",
    "fail-unbound-target",
    "fail-unexpected-exception",
    "not-applicable-reference-alignment",
    "not-applicable-reference-exception",
    "not-applicable-reference-mmio",
    "not-applicable-reference-known-bug",
    "not-applicable-katana-restricted",
    "not-applicable-access-shape",
    "corpus-invalid",
    "harness-invalid",
    "infrastructure-error",
};

void require_evidence_output_contract(const ExternalIsaEvidence& evidence) {
    const auto expected_scope_vectors = expected_external_scope_vectors(evidence.scope);
    if (evidence.source != external_evidence_contract::source ||
        !lowercase_hex(evidence.katana_commit, 40u) ||
        !lowercase_hex(evidence.corpus_commit, 40u) ||
        !lowercase_hex(evidence.corpus_manifest_sha256, 64u) ||
        !safe_contract_token(evidence.backend_profile) || !expected_scope_vectors ||
        evidence.expected_scope_vectors != *expected_scope_vectors ||
        (evidence.memory_profile != "native-product-memory" &&
         evidence.memory_profile != "flat-semantic-memory") ||
        (evidence.fpu_comparison_mode != "strict" &&
         evidence.fpu_comparison_mode != "upstream-compatible") ||
        evidence.backend_profile_version == 0u || evidence.runtime_abi == 0u ||
        evidence.backend_abi == 0u || evidence.counts.total == 0u ||
        evidence.counts.total > evidence.expected_scope_vectors ||
        (evidence.complete_scope && evidence.counts.total != evidence.expected_scope_vectors) ||
        evidence.counts.applicable > evidence.counts.total ||
        evidence.counts.not_applicable != evidence.counts.total - evidence.counts.applicable ||
        evidence.counts.passed > evidence.counts.applicable ||
        evidence.counts.failed != evidence.counts.applicable - evidence.counts.passed) {
        throw std::invalid_argument("Externe SST-Evidence verletzt den Ausgabe-Vertrag.");
    }
    constexpr std::array<std::string_view, 9u> known_stale_reasons{
        "untrusted-build-source",
        "katana-commit-mismatch",
        "corpus-commit-mismatch",
        "corpus-manifest-mismatch",
        "runtime-abi-mismatch",
        "backend-abi-mismatch",
        "backend-profile-mismatch",
        "backend-profile-version-mismatch",
        "incomplete-scope",
    };
    const auto incomplete_reason = std::find(evidence.stale_reasons.begin(),
                                             evidence.stale_reasons.end(),
                                             "incomplete-scope") != evidence.stale_reasons.end();
    if (evidence.stale != !evidence.stale_reasons.empty() ||
        incomplete_reason == evidence.complete_scope ||
        !std::all_of(
            evidence.stale_reasons.begin(), evidence.stale_reasons.end(), [&](const auto& reason) {
                return std::find(known_stale_reasons.begin(), known_stale_reasons.end(), reason) !=
                       known_stale_reasons.end();
            })) {
        throw std::invalid_argument("Externe SST-Evidence besitzt ungueltige Stale-Gruende.");
    }
}

AlphaIsaLayerSupport supported_layers() {
    return {AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported};
}

AlphaIsaLayerSupport restricted_runtime_layers() {
    return {AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported,
            AlphaIsaSupport::Restricted};
}

std::vector<AlphaIsaFamilyEntry> alpha_families() {
    using Support = AlphaIsaSupport;
    return {
        {"integer-core",
         "Integer arithmetic and logic",
         Support::Supported,
         supported_layers(),
         "Bit-exact 32-bit integer, T-bit and accumulator semantics through generated code.",
         "",
         "Decode, lower, emit and execute success plus wraparound and flag boundaries."},
        {"memory-transfer",
         "Memory transfers and addressing",
         Support::Supported,
         supported_layers(),
         "Width, sign extension, addressing update and typed memory exceptions are explicit.",
         "",
         "Success, alignment, region-boundary and delay-slot exception vectors."},
        {"control-flow",
         "Direct and indirect control flow",
         Support::Supported,
         supported_layers(),
         "Delay slots, PR, resolved targets and controlled unknown-target failure are explicit.",
         "",
         "Taken/not-taken, call/return, delay-slot and unknown-target vectors."},
        {"system-control",
         "Status, exception and system control",
         Support::Restricted,
         restricted_runtime_layers(),
         "SR/FPSCR transfers, traps and exception return preserve structured CPU state.",
         "Complete SLEEP wakeup and unimplemented privileged MMU/cache-control operations "
         "remain restricted; user-mode privilege violations trap before side effects.",
         "User/privileged mode, bank switch, exception, delay-slot and rejection vectors."},
        {"cache-store-queue",
         "Cache, prefetch and store queues",
         Support::Restricted,
         restricted_runtime_layers(),
         "PREF and store-queue transfers use explicit platform-service boundaries.",
         "Operand-cache RAM and complete cache coherency behavior are not enabled.",
         "RAM/TA queue success, invalid queue, cache invalidation and denied-profile vectors."},
        {"fpu",
         "Floating point",
         Support::Restricted,
         restricted_runtime_layers(),
         "FPSCR mode, register banks, scalar/vector operations and disabled-FPU faults are "
         "explicit.",
         "Complete SH-4 floating-point exception cause, enable and sticky-flag behavior is "
         "incomplete.",
         "Bit-pattern, rounding, DN, bank/mode, disabled-FPU and invalid-combination vectors."},
        {"unknown-opcode",
         "Unknown or unimplemented encodings",
         Support::Rejected,
         {},
         "Unknown 16-bit encodings are never decoded or executed as successful no-ops.",
         "No semantics are claimed until decoder, IR, backend and runtime contracts exist.",
         "Stable unknown-opcode diagnostic and non-success execution vector."}};
}

bool fpu_kind(const InstructionKind kind) {
    return kind >= InstructionKind::FmovRegister && kind <= InstructionKind::Fschg;
}

bool system_kind(const InstructionKind kind) {
    switch (kind) {
    case InstructionKind::StoreSpecialRegister:
    case InstructionKind::StoreSpecialRegisterPreDecrement:
    case InstructionKind::LoadSpecialRegister:
    case InstructionKind::LoadSpecialRegisterPostIncrement:
    case InstructionKind::TrapAlways:
    case InstructionKind::ReturnFromException:
    case InstructionKind::Sleep:
    case InstructionKind::LoadTlb:
        return true;
    default:
        return false;
    }
}

bool memory_kind(const InstructionKind kind) {
    return kind >= InstructionKind::MovByteStore && kind <= InstructionKind::MoveAddressPcRelative;
}

std::string
family_id(const InstructionKind kind, const bool privileged, const ControlFlowKind control_flow) {
    if (fpu_kind(kind)) return "fpu";
    if (kind == InstructionKind::Prefetch || kind == InstructionKind::Ocbi ||
        kind == InstructionKind::Ocbp || kind == InstructionKind::Ocbwb ||
        kind == InstructionKind::MovcaLong)
        return "cache-store-queue";
    if (privileged || system_kind(kind)) return "system-control";
    if (control_flow != ControlFlowKind::None || kind == InstructionKind::Rts)
        return "control-flow";
    if (memory_kind(kind)) return "memory-transfer";
    return "integer-core";
}

const AlphaIsaFamilyEntry& family(const std::vector<AlphaIsaFamilyEntry>& families,
                                  const std::string& id) {
    const auto found = std::find_if(
        families.begin(), families.end(), [&](const auto& entry) { return entry.id == id; });
    if (found == families.end()) throw std::logic_error("Alpha-ISA-Familie fehlt.");
    return *found;
}

std::string special_kind_name(const InstructionKind kind) {
    switch (kind) {
    case InstructionKind::StoreSpecialRegister:
        return "StoreSpecialRegister";
    case InstructionKind::StoreSpecialRegisterPreDecrement:
        return "StoreSpecialRegisterPreDecrement";
    case InstructionKind::LoadSpecialRegister:
        return "LoadSpecialRegister";
    case InstructionKind::LoadSpecialRegisterPostIncrement:
        return "LoadSpecialRegisterPostIncrement";
    default:
        return "Unknown";
    }
}

} // namespace

AlphaIsaSupport alpha_isa_intersection(const AlphaIsaLayerSupport layers) noexcept {
    const std::array values{layers.decoder, layers.ir, layers.backend, layers.runtime};
    if (std::find(values.begin(), values.end(), AlphaIsaSupport::Rejected) != values.end())
        return AlphaIsaSupport::Rejected;
    if (std::find(values.begin(), values.end(), AlphaIsaSupport::Restricted) != values.end())
        return AlphaIsaSupport::Restricted;
    return AlphaIsaSupport::Supported;
}

ExternalIsaEvidence parse_external_isa_evidence_json(const std::string_view document) {
    const auto root = EvidenceJsonParser(document).parse();
    if (root.kind != EvidenceJsonKind::Object)
        throw std::invalid_argument("Externer SST-Evidencebericht braucht ein Wurzelobjekt.");

    require_exact_string(root, "schema", external_evidence_contract::source_schema);
    require_exact_string(root, "report_type", external_evidence_contract::source_report_type);
    if (evidence_unsigned(root, "report_version") !=
        external_evidence_contract::source_report_version)
        throw std::invalid_argument("Inkompatible Version des externen SST-Evidenceberichts.");

    const auto status = evidence_string(root, "status");
    if (status != "success" && status != "failure")
        throw std::invalid_argument("SST-Evidencefeld 'status' ist ungueltig.");

    const auto require_kind = [&](const std::string_view name, const EvidenceJsonKind kind) {
        if (evidence_member(root, name).kind != kind)
            throw std::invalid_argument("SST-Evidencefeld '" + std::string(name) +
                                        "' besitzt einen ungueltigen JSON-Typ.");
    };
    require_kind("compiler", EvidenceJsonKind::String);
    require_kind("build_type", EvidenceJsonKind::String);
    require_kind("host_platform", EvidenceJsonKind::String);
    require_kind("lto", EvidenceJsonKind::Boolean);
    require_kind("classification_counts", EvidenceJsonKind::Object);
    require_kind("used_files", EvidenceJsonKind::Array);
    require_kind("represented_opcodes", EvidenceJsonKind::Array);
    require_kind("katana_opcodes_without_external_evidence", EvidenceJsonKind::Array);
    require_kind("first_counterexamples", EvidenceJsonKind::Array);

    ExternalIsaEvidence result;
    result.source = external_evidence_contract::source;
    result.katana_commit = evidence_string(root, "katana_commit");
    result.corpus_commit = evidence_string(root, "corpus_commit");
    result.corpus_manifest_sha256 = evidence_string(root, "corpus_manifest_sha256");
    result.runtime_abi = evidence_u32(root, "runtime_abi");
    result.backend_abi = evidence_u32(root, "backend_abi");
    result.backend_profile = evidence_string(root, "backend_profile");
    result.backend_profile_version = evidence_u32(root, "backend_profile_version");
    result.scope = evidence_string(root, "scope");
    const auto expected_scope_vectors = expected_external_scope_vectors(result.scope);
    if (!expected_scope_vectors)
        throw std::invalid_argument("SST-Evidencefeld 'scope' muss 'smoke' oder 'full' sein.");

    const auto& selection = evidence_member(root, "selection");
    if (selection.kind != EvidenceJsonKind::Object)
        throw std::invalid_argument("SST-Evidencefeld 'selection' muss ein Objekt sein.");
    result.complete_scope = evidence_boolean(selection, "complete_scope");
    result.expected_scope_vectors = evidence_unsigned(selection, "expected_scope_vectors");
    if (result.expected_scope_vectors != *expected_scope_vectors)
        throw std::invalid_argument("SST-Evidence besitzt einen ungueltigen Scope-Vektornenner.");

    const auto has_nullable_string = [&](const std::string_view name) {
        const auto& value = evidence_member(selection, name);
        if (value.kind == EvidenceJsonKind::Null) return false;
        if (value.kind != EvidenceJsonKind::String || value.scalar.empty())
            throw std::invalid_argument("SST-Evidencefilter '" + std::string(name) +
                                        "' muss null oder ein nicht-leerer String sein.");
        return true;
    };
    const auto nullable_unsigned =
        [&](const std::string_view name) -> std::optional<std::uint64_t> {
        const auto& value = evidence_member(selection, name);
        if (value.kind == EvidenceJsonKind::Null) return std::nullopt;
        return evidence_unsigned(selection, name);
    };
    const auto has_file_filter = has_nullable_string("file");
    const auto case_filter = nullable_unsigned("case");
    const auto opcode_filter = nullable_unsigned("opcode");
    const auto has_family_filter = has_nullable_string("family");
    if (case_filter && *case_filter > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("SST-Evidencefilter 'case' ist ausserhalb von uint32.");
    if (opcode_filter && *opcode_filter > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument("SST-Evidencefilter 'opcode' ist ausserhalb von uint16.");

    bool has_shard_filter = false;
    const auto& shard = evidence_member(selection, "shard");
    if (shard.kind != EvidenceJsonKind::Null) {
        if (shard.kind != EvidenceJsonKind::Object)
            throw std::invalid_argument(
                "SST-Evidencefilter 'shard' muss null oder ein Objekt sein.");
        const auto shard_index = evidence_unsigned(shard, "index");
        const auto shard_count = evidence_unsigned(shard, "count");
        if (shard_count == 0u || shard_index >= shard_count)
            throw std::invalid_argument("SST-Evidencefilter 'shard' ist ungueltig.");
        has_shard_filter = true;
    }
    const auto fail_fast = evidence_boolean(selection, "fail_fast");

    result.memory_profile = evidence_string(root, "memory_profile");
    result.fpu_comparison_mode = evidence_string(root, "fpu_comparison_mode");
    if (result.fpu_comparison_mode != "strict" &&
        result.fpu_comparison_mode != "upstream-compatible")
        throw std::invalid_argument("SST-Evidencefeld 'fpu_comparison_mode' ist ungueltig.");
    if (!lowercase_hex(result.katana_commit, 40u) || !lowercase_hex(result.corpus_commit, 40u) ||
        !lowercase_hex(result.corpus_manifest_sha256, 64u))
        throw std::invalid_argument("SST-Evidence besitzt einen ungueltigen Commit oder Hash.");
    if (!safe_contract_token(result.backend_profile))
        throw std::invalid_argument("SST-Evidence besitzt ein ungueltiges Backend-Profil.");
    if (result.memory_profile != "native-product-memory" &&
        result.memory_profile != "flat-semantic-memory")
        throw std::invalid_argument("SST-Evidencefeld 'memory_profile' ist ungueltig.");

    const auto& counts = evidence_member(root, "counts");
    if (counts.kind != EvidenceJsonKind::Object)
        throw std::invalid_argument("SST-Evidencefeld 'counts' muss ein Objekt sein.");
    result.counts.total = evidence_unsigned(counts, "total");
    result.counts.applicable = evidence_unsigned(counts, "applicable");
    result.counts.passed = evidence_unsigned(counts, "passed");
    result.counts.failed = evidence_unsigned(counts, "failed");
    if (result.counts.total > result.expected_scope_vectors ||
        result.counts.applicable > result.counts.total ||
        result.counts.passed > result.counts.applicable ||
        result.counts.failed != result.counts.applicable - result.counts.passed)
        throw std::invalid_argument("SST-Evidence besitzt widerspruechliche Vektorenzaehler.");
    if (result.complete_scope &&
        (result.counts.total != result.expected_scope_vectors || has_file_filter || case_filter ||
         opcode_filter || has_family_filter || has_shard_filter || fail_fast))
        throw std::invalid_argument(
            "Vollstaendige SST-Evidence darf weder gekuerzt noch gefiltert sein.");
    result.counts.not_applicable = result.counts.total - result.counts.applicable;
    if ((status == "success") != (result.counts.failed == 0u))
        throw std::invalid_argument("SST-Evidencestatus widerspricht dem Fail-Zaehler.");

    const auto& classification_counts = evidence_member(root, "classification_counts");
    if (classification_counts.object.size() != external_classification_names.size())
        throw std::invalid_argument(
            "SST-Evidencefeld 'classification_counts' besitzt nicht den exakten "
            "Klassifikationssatz.");
    std::array<std::uint64_t, external_classification_names.size()> classification_values{};
    for (std::size_t index = 0u; index < external_classification_names.size(); ++index)
        classification_values[index] =
            evidence_unsigned(classification_counts, external_classification_names[index]);
    const auto classification_sum = [&](const std::size_t first, const std::size_t last) {
        std::uint64_t total = 0u;
        for (auto index = first; index <= last; ++index) {
            if (classification_values[index] > result.counts.total - total)
                throw std::invalid_argument(
                    "SST-Evidencefeld 'classification_counts' ueberschreitet den Nenner.");
            total += classification_values[index];
        }
        return total;
    };
    const auto classified_total = classification_sum(0u, external_classification_names.size() - 1u);
    const auto classified_failures = classification_sum(1u, 10u);
    const auto classified_not_applicable = classification_sum(11u, 16u);
    const auto classified_invalid = classification_sum(17u, 19u);
    if (classified_total != result.counts.total ||
        classification_values[0] != result.counts.passed ||
        classified_failures != result.counts.failed ||
        classified_not_applicable != result.counts.not_applicable || classified_invalid != 0u)
        throw std::invalid_argument(
            "SST-Evidencefeld 'classification_counts' widerspricht den Vektorenzaehlern.");

    const auto& waivers = evidence_member(root, "waivers");
    if (waivers.kind != EvidenceJsonKind::Array ||
        !std::all_of(waivers.array.begin(), waivers.array.end(), [](const auto& waiver) {
            return waiver.kind == EvidenceJsonKind::Object;
        }))
        throw std::invalid_argument("SST-Evidencefeld 'waivers' muss eine Objektliste sein.");
    result.waiver_count = static_cast<std::uint64_t>(waivers.array.size());

    const auto mark_stale = [&](const bool mismatch, const std::string_view reason) {
        if (mismatch) result.stale_reasons.emplace_back(reason);
    };
    mark_stale(build_contract::katana_git_commit == "0000000000000000000000000000000000000000" ||
                   result.katana_commit == "0000000000000000000000000000000000000000",
               "untrusted-build-source");
    mark_stale(result.katana_commit != build_contract::katana_git_commit, "katana-commit-mismatch");
    mark_stale(result.corpus_commit != external_evidence_contract::corpus_commit,
               "corpus-commit-mismatch");
    mark_stale(result.corpus_manifest_sha256 != external_evidence_contract::corpus_manifest_sha256,
               "corpus-manifest-mismatch");
    mark_stale(result.runtime_abi != build_contract::runtime_abi_version, "runtime-abi-mismatch");
    mark_stale(result.backend_abi != build_contract::backend_interface_abi_version,
               "backend-abi-mismatch");
    mark_stale(result.backend_profile !=
                   codegen::native_aot_emission_profile_name(
                       codegen::NativeAotEmissionProfile::ExternalConformance),
               "backend-profile-mismatch");
    mark_stale(result.backend_profile_version != codegen::native_aot_emission_profile_version,
               "backend-profile-version-mismatch");
    mark_stale(!result.complete_scope, "incomplete-scope");
    result.stale = !result.stale_reasons.empty();
    require_evidence_output_contract(result);
    return result;
}

IsaCoverageReport build_isa_coverage_report() {
    std::map<InstructionKind, MutableCoverage> entries;

    for (const auto& metadata : instruction_metadata()) {
        auto& entry = entries[metadata.kind];
        entry.name = std::string(metadata.name);
        ++entry.rule_count;
        entry.privileged = entry.privileged || metadata.is_privileged;
        if (metadata.control_flow != ControlFlowKind::None)
            entry.control_flow = metadata.control_flow;
    }
    for (const auto& metadata : special_register_encoding_metadata()) {
        auto& entry = entries[metadata.kind];
        entry.name = special_kind_name(metadata.kind);
        ++entry.rule_count;
        entry.privileged = entry.privileged || metadata.is_privileged;
    }

    IsaCoverageReport report;
    report.families = alpha_families();
    for (std::uint32_t opcode = 0; opcode <= 0xFFFFu; ++opcode) {
        const auto decoded = decode(static_cast<std::uint16_t>(opcode));
        if (!decoded.is_known()) {
            ++report.unknown_opcode_count;
            continue;
        }
        ++report.known_opcode_count;
        ++entries[decoded.kind].opcode_count;
    }

    report.instructions.reserve(entries.size());
    for (const auto& [kind, entry] : entries) {
        const auto id = family_id(kind, entry.privileged, entry.control_flow);
        const auto& contract = family(report.families, id);
        const auto operation = katana::ir::lowering_operation_for_instruction(kind);
        const AlphaIsaLayerSupport layers{
            entry.rule_count != 0u && entry.opcode_count != 0u ? AlphaIsaSupport::Supported
                                                               : AlphaIsaSupport::Rejected,
            operation != katana::ir::Operation::Unknown ? AlphaIsaSupport::Supported
                                                        : AlphaIsaSupport::Rejected,
            katana::codegen::cpp_backend_supports_operation(operation) ? AlphaIsaSupport::Supported
                                                                       : AlphaIsaSupport::Rejected,
            contract.layers.runtime};
        report.instructions.push_back({kind,
                                       entry.name,
                                       entry.rule_count,
                                       entry.opcode_count,
                                       entry.privileged,
                                       id,
                                       layers,
                                       alpha_isa_intersection(layers),
                                       contract.limitation,
                                       contract.test_requirement});
    }
    return report;
}

std::string format_isa_coverage_report(const IsaCoverageReport& report) {
    std::ostringstream output;
    output
        << "KatanaRecomp SH-4 ISA-Abdeckung\n"
        << "Name                              Familie              Status       Regeln  Opcodes\n";
    for (const auto& entry : report.instructions) {
        output << std::left << std::setw(34) << entry.name << std::setw(21) << entry.family_id
               << std::setw(13) << alpha_isa_support_name(entry.support) << std::right
               << std::setw(7) << entry.encoding_rule_count << std::setw(9)
               << entry.decoded_opcode_count << '\n';
    }
    output << "Bekannte Opcodes:   " << report.known_opcode_count << '\n'
           << "Unbekannte Opcodes: " << report.unknown_opcode_count << '\n'
           << "Instruktionsarten:  " << report.instructions.size() << '\n'
           << "Alpha-Vertrag:      " << alpha_isa_contract_version << "\n\n"
           << "Familien und Grenzen:\n";
    for (const auto& entry : report.families) {
        output << "- " << entry.id << ": " << alpha_isa_support_name(entry.support) << " - "
               << entry.semantic_contract;
        if (!entry.limitation.empty()) output << " Grenze: " << entry.limitation;
        output << " Tests: " << entry.test_requirement << '\n';
    }
    return output.str();
}

const char* alpha_isa_support_name(const AlphaIsaSupport support) noexcept {
    switch (support) {
    case AlphaIsaSupport::Supported:
        return "supported";
    case AlphaIsaSupport::Restricted:
        return "restricted";
    case AlphaIsaSupport::Rejected:
        return "rejected";
    }
    return "rejected";
}

std::string format_alpha_isa_json(const IsaCoverageReport& report,
                                  const std::optional<ExternalIsaEvidence>& external_evidence) {
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-alpha-isa", "alpha-isa");
    output << ",\"contract_version\":" << alpha_isa_contract_version
           << ",\"known_opcode_count\":" << report.known_opcode_count
           << ",\"unknown_opcode_count\":" << report.unknown_opcode_count << ",\"families\":[";
    for (std::size_t index = 0u; index < report.families.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& entry = report.families[index];
        output << "{\"id\":" << katana::io::quote_json(entry.id)
               << ",\"name\":" << katana::io::quote_json(entry.name)
               << ",\"status\":" << katana::io::quote_json(alpha_isa_support_name(entry.support))
               << ",\"layers\":{\"decoder\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.decoder))
               << ",\"ir\":" << katana::io::quote_json(alpha_isa_support_name(entry.layers.ir))
               << ",\"backend\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.backend))
               << ",\"runtime\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.runtime)) << '}'
               << ",\"semantic_contract\":" << katana::io::quote_json(entry.semantic_contract)
               << ",\"limitation\":" << katana::io::quote_json(entry.limitation)
               << ",\"test_requirement\":" << katana::io::quote_json(entry.test_requirement) << '}';
    }
    output << "],\"instructions\":[";
    for (std::size_t index = 0u; index < report.instructions.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& entry = report.instructions[index];
        output << "{\"name\":" << katana::io::quote_json(entry.name)
               << ",\"family\":" << katana::io::quote_json(entry.family_id)
               << ",\"status\":" << katana::io::quote_json(alpha_isa_support_name(entry.support))
               << ",\"layers\":{\"decoder\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.decoder))
               << ",\"ir\":" << katana::io::quote_json(alpha_isa_support_name(entry.layers.ir))
               << ",\"backend\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.backend))
               << ",\"runtime\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.runtime)) << '}'
               << ",\"encoding_rules\":" << entry.encoding_rule_count
               << ",\"decoded_opcodes\":" << entry.decoded_opcode_count
               << ",\"privileged\":" << (entry.contains_privileged_encoding ? "true" : "false")
               << ",\"limitation\":" << katana::io::quote_json(entry.limitation)
               << ",\"test_requirement\":" << katana::io::quote_json(entry.test_requirement) << '}';
    }
    output << "],\"external_evidence\":";
    if (!external_evidence) {
        output << "null";
    } else {
        const auto& evidence = *external_evidence;
        require_evidence_output_contract(evidence);
        output << "{\"evidence_version\":" << external_evidence_contract::version
               << ",\"source\":" << katana::io::quote_json(evidence.source)
               << ",\"katana_commit\":" << katana::io::quote_json(evidence.katana_commit)
               << ",\"corpus_commit\":" << katana::io::quote_json(evidence.corpus_commit)
               << ",\"corpus_manifest_sha256\":"
               << katana::io::quote_json(evidence.corpus_manifest_sha256)
               << ",\"backend_profile\":" << katana::io::quote_json(evidence.backend_profile)
               << ",\"backend_profile_version\":" << evidence.backend_profile_version
               << ",\"runtime_abi\":" << evidence.runtime_abi
               << ",\"backend_abi\":" << evidence.backend_abi
               << ",\"scope\":" << katana::io::quote_json(evidence.scope)
               << ",\"complete_scope\":" << (evidence.complete_scope ? "true" : "false")
               << ",\"expected_scope_vectors\":" << evidence.expected_scope_vectors
               << ",\"memory_profile\":" << katana::io::quote_json(evidence.memory_profile)
               << ",\"fpu_comparison_mode\":"
               << katana::io::quote_json(evidence.fpu_comparison_mode)
               << ",\"counts\":{\"total\":" << evidence.counts.total
               << ",\"applicable\":" << evidence.counts.applicable
               << ",\"passed\":" << evidence.counts.passed
               << ",\"failed\":" << evidence.counts.failed
               << ",\"not_applicable\":" << evidence.counts.not_applicable << '}'
               << ",\"waivers\":" << evidence.waiver_count
               << ",\"stale\":" << (evidence.stale ? "true" : "false") << ",\"stale_reasons\":[";
        for (std::size_t index = 0u; index < evidence.stale_reasons.size(); ++index) {
            if (index != 0u) output << ',';
            output << katana::io::quote_json(evidence.stale_reasons[index]);
        }
        output << "]}";
    }
    output << '}';
    return output.str();
}

} // namespace katana::sh4
