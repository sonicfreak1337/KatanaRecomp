#include "closure_witness_import.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace katana::cli {
namespace {

enum class JsonKind : std::uint8_t { Null, Boolean, String, Array, Object };

struct JsonValue final {
    JsonKind kind = JsonKind::Null;
    bool boolean = false;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue, std::less<>> object;
};

class JsonError final : public std::runtime_error {
  public:
    explicit JsonError(const std::string& message) : std::runtime_error(message) {}
};

class JsonReader final {
  public:
    JsonReader(const std::string_view input, const ClosureWitnessImportLimits& limits)
        : input_(input), limits_(limits) {}

    [[nodiscard]] JsonValue parse() {
        if (input_.size() > limits_.max_document_bytes)
            fail("document exceeds bounded byte budget");
        for (std::size_t begin = 0u; begin < input_.size();) {
            const auto end = input_.find('\n', begin);
            const auto length = (end == std::string_view::npos ? input_.size() : end) - begin;
            if (length > limits_.max_line_bytes) fail("line exceeds bounded byte budget");
            if (end == std::string_view::npos) break;
            begin = end + 1u;
        }
        skip_space();
        auto result = value(0u);
        skip_space();
        if (offset_ != input_.size()) fail("trailing JSON data");
        return result;
    }

  private:
    [[noreturn]] void fail(const std::string_view message) const {
        throw JsonError(std::string(message) + " at byte " + std::to_string(offset_));
    }

    void skip_space() noexcept {
        while (offset_ < input_.size()) {
            const char ch = input_[offset_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            ++offset_;
        }
    }

    bool consume(const char expected) noexcept {
        if (offset_ >= input_.size() || input_[offset_] != expected) return false;
        ++offset_;
        return true;
    }

    void require(const char expected, const std::string_view message) {
        if (!consume(expected)) fail(message);
    }

    [[nodiscard]] std::string string_value() {
        require('"', "expected JSON string");
        std::string result;
        while (offset_ < input_.size()) {
            const auto ch = static_cast<unsigned char>(input_[offset_++]);
            if (ch == '"') return result;
            if (ch < 0x20u) fail("control character in JSON string");
            if (ch != '\\') {
                result.push_back(static_cast<char>(ch));
            } else {
                if (offset_ >= input_.size()) fail("unterminated JSON escape");
                const char escape = input_[offset_++];
                switch (escape) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: fail("unsupported JSON escape in identity document");
                }
            }
            if (result.size() > limits_.max_string_bytes)
                fail("JSON string exceeds bounded byte budget");
        }
        fail("unterminated JSON string");
    }

    void literal(const std::string_view expected) {
        if (input_.substr(offset_, expected.size()) != expected) fail("invalid JSON literal");
        offset_ += expected.size();
    }

    [[nodiscard]] JsonValue value(const std::size_t depth) {
        if (depth > limits_.max_json_depth) fail("JSON nesting exceeds bounded depth");
        if (++nodes_ > limits_.max_json_nodes) fail("JSON node budget exceeded");
        skip_space();
        if (offset_ >= input_.size()) fail("missing JSON value");
        if (input_[offset_] == '"') {
            JsonValue result;
            result.kind = JsonKind::String;
            result.string = string_value();
            return result;
        }
        if (input_[offset_] == 't') {
            literal("true");
            JsonValue result;
            result.kind = JsonKind::Boolean;
            result.boolean = true;
            return result;
        }
        if (input_[offset_] == 'f') {
            literal("false");
            JsonValue result;
            result.kind = JsonKind::Boolean;
            return result;
        }
        if (input_[offset_] == 'n') {
            literal("null");
            return {};
        }
        if (consume('{')) {
            JsonValue result;
            result.kind = JsonKind::Object;
            skip_space();
            if (consume('}')) return result;
            while (true) {
                if (offset_ >= input_.size() || input_[offset_] != '"')
                    fail("object key must be a string");
                auto key = string_value();
                skip_space();
                require(':', "expected colon after object key");
                skip_space();
                auto inserted = result.object.emplace(std::move(key), value(depth + 1u));
                if (!inserted.second) fail("duplicate object key");
                skip_space();
                if (consume('}')) return result;
                require(',', "expected comma or object end");
                skip_space();
            }
        }
        if (consume('[')) {
            JsonValue result;
            result.kind = JsonKind::Array;
            skip_space();
            if (consume(']')) return result;
            while (true) {
                result.array.push_back(value(depth + 1u));
                skip_space();
                if (consume(']')) return result;
                require(',', "expected comma or array end");
                skip_space();
            }
        }
        fail("unsupported JSON value; v5 fields use typed strings and booleans");
    }

    std::string_view input_;
    const ClosureWitnessImportLimits& limits_;
    std::size_t offset_ = 0u;
    std::size_t nodes_ = 0u;
};

[[nodiscard]] const JsonValue& member(const JsonValue& value, const std::string_view name) {
    if (value.kind != JsonKind::Object) throw JsonError("expected object");
    const auto found = value.object.find(name);
    if (found == value.object.end())
        throw JsonError("missing required field '" + std::string(name) + "'");
    return found->second;
}

void exact_keys(const JsonValue& value,
                const std::initializer_list<std::string_view> expected) {
    if (value.kind != JsonKind::Object) throw JsonError("expected object");
    if (value.object.size() != expected.size()) throw JsonError("unknown or missing field");
    for (const auto key : expected) {
        if (value.object.find(key) == value.object.end())
            throw JsonError("unknown or missing field '" + std::string(key) + "'");
    }
}

[[nodiscard]] std::string string_member(const JsonValue& object, const std::string_view name) {
    const auto& value = member(object, name);
    if (value.kind != JsonKind::String)
        throw JsonError("field '" + std::string(name) + "' must be string");
    return value.string;
}

[[nodiscard]] bool bool_member(const JsonValue& object, const std::string_view name) {
    const auto& value = member(object, name);
    if (value.kind != JsonKind::Boolean)
        throw JsonError("field '" + std::string(name) + "' must be boolean");
    return value.boolean;
}

[[nodiscard]] bool safe_identity(const std::string_view value, const std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
        return ch >= 0x20u && std::isspace(ch) == 0 && ch != '/' &&
               ch != '\\' && ch != '"';
    });
}

[[nodiscard]] bool decimal_u64(const std::string_view value, std::uint64_t& output) noexcept {
    if (value.empty() || (value.size() > 1u && value.front() == '0')) return false;
    output = 0u;
    for (const auto ch : value) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (output > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return false;
        output = output * 10u + digit;
    }
    return true;
}

void require_identity(const std::string_view name, const std::string& value) {
    if (!safe_identity(value, 4096u))
        throw JsonError("field '" + std::string(name) + "' is not a bounded identity token");
}

ClosureReference reference(const JsonValue& value, const std::string_view name) {
    exact_keys(value, {"address", "identity"});
    ClosureReference result{string_member(value, "address"), string_member(value, "identity")};
    std::uint64_t ignored = 0u;
    if (!decimal_u64(result.address, ignored))
        throw JsonError("reference address must be canonical decimal uint64 string");
    require_identity(name, result.identity);
    return result;
}

ClosurePointer pointer(const JsonValue& value) {
    exact_keys(value, {"address_present", "address", "identity", "value"});
    ClosurePointer result;
    result.address_present = bool_member(value, "address_present");
    result.address = string_member(value, "address");
    result.identity = string_member(value, "identity");
    result.value = string_member(value, "value");
    std::uint64_t ignored = 0u;
    if (!decimal_u64(result.value, ignored))
        throw JsonError("pointer value must be a canonical decimal uint64 string");
    if (result.address_present) {
        if (!decimal_u64(result.address, ignored))
            throw JsonError("present pointer address must be canonical decimal uint64 string");
        require_identity("pointer.identity", result.identity);
    } else if (!result.address.empty() || !result.identity.empty()) {
        throw JsonError("absent pointer address must have empty address and identity");
    }
    return result;
}

ClosureSlot slot(const JsonValue& value) {
    exact_keys(value, {"slot_present", "address", "identity"});
    ClosureSlot result;
    result.slot_present = bool_member(value, "slot_present");
    result.address = string_member(value, "address");
    result.identity = string_member(value, "identity");
    if (result.slot_present) {
        std::uint64_t ignored = 0u;
        if (!decimal_u64(result.address, ignored))
            throw JsonError("present slot address must be canonical decimal uint64 string");
        require_identity("slot.identity", result.identity);
    } else if (!result.address.empty() || !result.identity.empty()) {
        throw JsonError("absent slot must have empty address and identity");
    }
    return result;
}

ClosureWitnessFlags flags(const JsonValue& value) {
    exact_keys(value, {"immutable", "bounded", "complete", "runtime_observation", "reproof_required"});
    return {bool_member(value, "immutable"), bool_member(value, "bounded"),
            bool_member(value, "complete"), bool_member(value, "runtime_observation"),
            bool_member(value, "reproof_required")};
}

ClosureWitness witness(const JsonValue& value, const ClosureWitnessImportLimits& limits) {
    exact_keys(value, {"kind", "source", "callsite", "pointer", "target", "alias", "slot", "flags"});
    ClosureWitness result;
    result.kind = string_member(value, "kind");
    if (!safe_identity(result.kind, limits.max_string_bytes))
        throw JsonError("witness kind is not a bounded typed token");
    result.source = reference(member(value, "source"), "source.identity");
    result.callsite = reference(member(value, "callsite"), "callsite.identity");
    result.pointer = pointer(member(value, "pointer"));
    result.target = reference(member(value, "target"), "target.identity");
    result.alias = reference(member(value, "alias"), "alias.identity");
    result.slot = slot(member(value, "slot"));
    result.flags = flags(member(value, "flags"));
    return result;
}

void quote(std::string& output, const std::string_view value) {
    output.push_back('"');
    for (const char ch : value) {
        switch (ch) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output.push_back(ch); break;
        }
    }
    output.push_back('"');
}

void field(std::string& output, const std::string_view key, const std::string_view value) {
    quote(output, key); output.push_back(':'); quote(output, value);
}

void emit_reference(std::string& output, const ClosureReference& value) {
    output.push_back('{'); field(output, "address", value.address); output.push_back(',');
    field(output, "identity", value.identity); output.push_back('}');
}

void emit_witness(std::string& output,
                  const ClosureWitness& value,
                  const bool force_incomplete) {
    output.push_back('{');
    field(output, "kind", value.kind); output.push_back(',');
    output += "\"source\":{\"address\":"; quote(output, value.source.address);
    output += ",\"identity\":"; quote(output, value.source.identity); output.push_back('}');
    output += ",\"callsite\":"; emit_reference(output, value.callsite);
    output += ",\"pointer\":{\"address_present\":";
    output += value.pointer.address_present ? "true" : "false";
    output += ",\"address\":"; quote(output, value.pointer.address);
    output += ",\"identity\":"; quote(output, value.pointer.identity);
    output += ",\"value\":"; quote(output, value.pointer.value); output.push_back('}');
    output += ",\"target\":"; emit_reference(output, value.target);
    output += ",\"alias\":"; emit_reference(output, value.alias);
    output += ",\"slot\":{\"slot_present\":";
    output += value.slot.slot_present ? "true" : "false";
    output += ",\"address\":"; quote(output, value.slot.address);
    output += ",\"identity\":"; quote(output, value.slot.identity); output.push_back('}');
    output += ",\"flags\":{\"immutable\":"; output += value.flags.immutable ? "true" : "false";
    output += ",\"bounded\":"; output += value.flags.bounded ? "true" : "false";
    output += ",\"complete\":";
    output += (value.flags.complete && !force_incomplete) ? "true" : "false";
    output += ",\"runtime_observation\":"; output += value.flags.runtime_observation ? "true" : "false";
    output += ",\"reproof_required\":"; output += value.flags.reproof_required ? "true" : "false";
    output += "}}";
}

} // namespace

bool closure_witness_is_admissible(const ClosureWitnessDocument& document) noexcept {
    // The import boundary has no current CFA/FunctionMap/JumpTable/exact-owner
    // authority. Wire flags are observations, never a closure proof.
    static_cast<void>(document);
    return false;
}

ClosureWitnessImportResult import_closure_witness_v5(
    const std::string_view document, const ClosureWitnessImportLimits& limits) {
    ClosureWitnessImportResult result;
    try {
        JsonReader reader(document, limits);
        const auto root = reader.parse();
        exact_keys(root, {"schema", "version", "binding", "witnesses", "drop_count", "truncated", "invalid", "closure_admitted"});
        if (string_member(root, "schema") != closure_witness_v5_schema ||
            string_member(root, "version") != closure_witness_v5_version)
            throw JsonError("unsupported closure-witness schema/version");

        const auto& binding = member(root, "binding");
        exact_keys(binding, {"analysis_artifact_key", "content_identity", "boot_byte_identity",
                             "project_identity", "analysis_contract_identity",
                             "image_analysis_key", "game_project_identity", "native_port_identity",
                             "native_port_artifact_identity", "analysis_implementation_identity",
                             "analysis_cache_implementation_identity", "ir_product_implementation_identity",
                             "codegen_implementation_identity", "analyzer_abi", "backend_abi",
                             "analysis_mode", "disc_volume_start_lba", "disc_extent_lba_bias",
                             "runtime_generation"});
        auto& imported = result.document.binding;
        imported.analysis_artifact_key = string_member(binding, "analysis_artifact_key");
        imported.content_identity = string_member(binding, "content_identity");
        imported.boot_byte_identity = string_member(binding, "boot_byte_identity");
        imported.project_identity = string_member(binding, "project_identity");
        imported.analysis_contract_identity = string_member(binding, "analysis_contract_identity");
        imported.image_analysis_key = string_member(binding, "image_analysis_key");
        imported.game_project_identity = string_member(binding, "game_project_identity");
        imported.native_port_identity = string_member(binding, "native_port_identity");
        imported.native_port_artifact_identity = string_member(binding, "native_port_artifact_identity");
        imported.analysis_implementation_identity = string_member(binding, "analysis_implementation_identity");
        imported.analysis_cache_implementation_identity = string_member(binding, "analysis_cache_implementation_identity");
        imported.ir_product_implementation_identity = string_member(binding, "ir_product_implementation_identity");
        imported.codegen_implementation_identity = string_member(binding, "codegen_implementation_identity");
        imported.analyzer_abi = string_member(binding, "analyzer_abi");
        imported.backend_abi = string_member(binding, "backend_abi");
        imported.analysis_mode = string_member(binding, "analysis_mode");
        imported.disc_volume_start_lba = string_member(binding, "disc_volume_start_lba");
        imported.disc_extent_lba_bias = string_member(binding, "disc_extent_lba_bias");
        imported.runtime_generation = string_member(binding, "runtime_generation");
        const std::array<std::pair<std::string_view, const std::string*>, 19u> identities{{
            {"analysis_artifact_key", &imported.analysis_artifact_key},
            {"content_identity", &imported.content_identity},
            {"boot_byte_identity", &imported.boot_byte_identity},
            {"project_identity", &imported.project_identity},
            {"analysis_contract_identity", &imported.analysis_contract_identity},
            {"image_analysis_key", &imported.image_analysis_key},
            {"game_project_identity", &imported.game_project_identity},
            {"native_port_identity", &imported.native_port_identity},
            {"native_port_artifact_identity", &imported.native_port_artifact_identity},
            {"analysis_implementation_identity", &imported.analysis_implementation_identity},
            {"analysis_cache_implementation_identity", &imported.analysis_cache_implementation_identity},
            {"ir_product_implementation_identity", &imported.ir_product_implementation_identity},
            {"codegen_implementation_identity", &imported.codegen_implementation_identity},
            {"analyzer_abi", &imported.analyzer_abi},
            {"backend_abi", &imported.backend_abi},
            {"analysis_mode", &imported.analysis_mode},
            {"disc_volume_start_lba", &imported.disc_volume_start_lba},
            {"disc_extent_lba_bias", &imported.disc_extent_lba_bias},
            {"runtime_generation", &imported.runtime_generation},
        }};
        for (const auto& [name, value] : identities) require_identity(name, *value);
        std::uint64_t ignored = 0u;
        if (!decimal_u64(imported.analyzer_abi, ignored) ||
            !decimal_u64(imported.backend_abi, ignored) ||
            !decimal_u64(imported.analysis_mode, ignored) ||
            !decimal_u64(imported.disc_volume_start_lba, ignored) ||
            !decimal_u64(imported.disc_extent_lba_bias, ignored) ||
            !decimal_u64(imported.runtime_generation, ignored))
            throw JsonError("binding numeric identity fields must be decimal uint64 strings");

        const auto& witnesses = member(root, "witnesses");
        if (witnesses.kind != JsonKind::Array || witnesses.array.empty() ||
            witnesses.array.size() > limits.max_witnesses)
            throw JsonError("witness array is outside bounded non-empty range");
        result.document.witnesses.reserve(witnesses.array.size());
        for (const auto& item : witnesses.array)
            result.document.witnesses.push_back(witness(item, limits));
        for (auto& item : result.document.witnesses)
            item.flags.reproof_required = true;

        const auto drops = string_member(root, "drop_count");
        if (!decimal_u64(drops, result.document.drop_count))
            throw JsonError("drop_count must be canonical decimal uint64 string");
        result.document.truncated = bool_member(root, "truncated");
        result.document.invalid = bool_member(root, "invalid");
        const auto claimed = bool_member(root, "closure_admitted");
        if (result.document.drop_count != 0u || result.document.truncated || result.document.invalid) {
            for (auto& item : result.document.witnesses) item.flags.complete = false;
        }
        result.parsed = true;
        result.valid = !result.document.invalid && result.document.drop_count == 0u &&
                       !result.document.truncated;
        result.closure_admitted = closure_witness_is_admissible(result.document);
        if (claimed)
            throw JsonError("wire closure_admitted=true requires local static reproof");
        result.reproof_required = true;
        return result;
    } catch (const std::exception& error) {
        result.parsed = false;
        result.valid = false;
        result.closure_admitted = false;
        result.error = error.what();
        return result;
    }
}

std::string serialize_closure_witness_v5(const ClosureWitnessDocument& document) {
    std::string output;
    output.reserve(1024u + document.witnesses.size() * 512u);
    output.push_back('{');
    field(output, "schema", closure_witness_v5_schema); output.push_back(',');
    field(output, "version", closure_witness_v5_version); output.push_back(',');
    output += "\"binding\":{";
    field(output, "analysis_artifact_key", document.binding.analysis_artifact_key); output.push_back(',');
    field(output, "content_identity", document.binding.content_identity); output.push_back(',');
    field(output, "boot_byte_identity", document.binding.boot_byte_identity); output.push_back(',');
    field(output, "project_identity", document.binding.project_identity); output.push_back(',');
    field(output, "analysis_contract_identity", document.binding.analysis_contract_identity); output.push_back(',');
    field(output, "image_analysis_key", document.binding.image_analysis_key); output.push_back(',');
    field(output, "game_project_identity", document.binding.game_project_identity); output.push_back(',');
    field(output, "native_port_identity", document.binding.native_port_identity); output.push_back(',');
    field(output, "native_port_artifact_identity", document.binding.native_port_artifact_identity); output.push_back(',');
    field(output, "analysis_implementation_identity", document.binding.analysis_implementation_identity); output.push_back(',');
    field(output, "analysis_cache_implementation_identity", document.binding.analysis_cache_implementation_identity); output.push_back(',');
    field(output, "ir_product_implementation_identity", document.binding.ir_product_implementation_identity); output.push_back(',');
    field(output, "codegen_implementation_identity", document.binding.codegen_implementation_identity); output.push_back(',');
    field(output, "analyzer_abi", document.binding.analyzer_abi); output.push_back(',');
    field(output, "backend_abi", document.binding.backend_abi); output.push_back(',');
    field(output, "analysis_mode", document.binding.analysis_mode); output.push_back(',');
    field(output, "disc_volume_start_lba", document.binding.disc_volume_start_lba); output.push_back(',');
    field(output, "disc_extent_lba_bias", document.binding.disc_extent_lba_bias); output.push_back(',');
    field(output, "runtime_generation", document.binding.runtime_generation); output.push_back('}');
    output += ",\"witnesses\":[";
    for (std::size_t index = 0u; index < document.witnesses.size(); ++index) {
        if (index != 0u) output.push_back(',');
        emit_witness(output, document.witnesses[index],
                     document.drop_count != 0u || document.truncated || document.invalid);
    }
    output += "],\"drop_count\":"; quote(output, std::to_string(document.drop_count));
    output += ",\"truncated\":"; output += document.truncated ? "true" : "false";
    output += ",\"invalid\":"; output += document.invalid ? "true" : "false";
    output += ",\"closure_admitted\":";
    output += "false";
    output.push_back('}');
    return output;
}

} // namespace katana::cli
