#include "katana/io/input_provenance.hpp"

#include "katana/io/input_output_error.hpp"
#include "katana/io/json_report.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace katana::io {
namespace {

constexpr std::array<std::uint32_t, 64u> round_constants{
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u,
    0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu,
    0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu,
    0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu, 0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu,
    0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u, 0x19A4C116u,
    0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u,
    0xC67178F2u};

class Sha256 final {
  public:
    void update(const std::string_view bytes) {
        if (finalized_) throw std::logic_error("SHA-256 wurde bereits abgeschlossen.");
        if (bytes.size() > (std::numeric_limits<std::uint64_t>::max() - total_bytes_)) {
            throw std::length_error("SHA-256-Eingabe ist zu gross.");
        }
        total_bytes_ += bytes.size();
        for (const auto byte : bytes) {
            buffer_[buffer_size_++] = static_cast<std::uint8_t>(byte);
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_);
                buffer_size_ = 0u;
            }
        }
    }

    std::string finish() {
        if (finalized_) throw std::logic_error("SHA-256 wurde bereits abgeschlossen.");
        finalized_ = true;
        const auto bit_count = total_bytes_ * 8u;
        buffer_[buffer_size_++] = 0x80u;
        if (buffer_size_ > 56u) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                      buffer_.end(),
                      std::uint8_t{0u});
            transform(buffer_);
            buffer_size_ = 0u;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                  buffer_.begin() + 56,
                  std::uint8_t{0u});
        for (std::size_t index = 0u; index < 8u; ++index) {
            buffer_[63u - index] = static_cast<std::uint8_t>(bit_count >> (index * 8u));
        }
        transform(buffer_);
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto word : state_)
            output << std::setw(8) << word;
        return output.str();
    }

  private:
    void transform(const std::array<std::uint8_t, 64u>& block) noexcept {
        std::array<std::uint32_t, 64u> words{};
        for (std::size_t index = 0u; index < 16u; ++index) {
            const auto offset = index * 4u;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24u) |
                           (static_cast<std::uint32_t>(block[offset + 1u]) << 16u) |
                           (static_cast<std::uint32_t>(block[offset + 2u]) << 8u) |
                           static_cast<std::uint32_t>(block[offset + 3u]);
        }
        for (std::size_t index = 16u; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15u], 7) ^ std::rotr(words[index - 15u], 18) ^
                            (words[index - 15u] >> 3u);
            const auto s1 = std::rotr(words[index - 2u], 17) ^ std::rotr(words[index - 2u], 19) ^
                            (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
        }
        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t index = 0u; index < words.size(); ++index) {
            const auto sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto first = h + sigma1 + choice + round_constants[index] + words[index];
            const auto sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto second = sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8u> state_{0x6A09E667u,
                                         0xBB67AE85u,
                                         0x3C6EF372u,
                                         0xA54FF53Au,
                                         0x510E527Fu,
                                         0x9B05688Cu,
                                         0x1F83D9ABu,
                                         0x5BE0CD19u};
    std::array<std::uint8_t, 64u> buffer_{};
    std::size_t buffer_size_ = 0u;
    std::uint64_t total_bytes_ = 0u;
    bool finalized_ = false;
};

bool stable_token(const std::string_view value) noexcept {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' || character == '_' ||
               character == '-';
    });
}

bool sha256_text(const std::string_view value) noexcept {
    return value.size() == 64u &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f') ||
                      (character >= 'A' && character <= 'F');
           });
}

void validate_provenance(const BuildProvenance& provenance) {
    if (!stable_token(provenance.tool_version) || provenance.manifest_version == 0u ||
        !sha256_text(provenance.manifest_sha256) ||
        (!provenance.directives_sha256.empty() && !sha256_text(provenance.directives_sha256)) ||
        provenance.ir_version == 0u || provenance.runtime_abi == 0u ||
        !stable_token(provenance.backend_name) || provenance.backend_abi == 0u) {
        throw std::invalid_argument("Buildprovenienz ist unvollstaendig.");
    }
    for (const auto& input : provenance.inputs) {
        if (!stable_token(input.role) || !sha256_text(input.sha256)) {
            throw std::invalid_argument("Eingabeprovenienz ist unvollstaendig.");
        }
    }
}

std::vector<InputProvenance> portable_inputs(const BuildProvenance& provenance) {
    auto inputs = provenance.inputs;
    std::sort(inputs.begin(), inputs.end(), [](const auto& left, const auto& right) {
        if (left.role != right.role) return left.role < right.role;
        if (left.sha256 != right.sha256) return left.sha256 < right.sha256;
        return left.size < right.size;
    });
    return inputs;
}

} // namespace

std::string sha256_bytes(const std::string_view bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

InputProvenance capture_input_provenance(std::string role, const std::filesystem::path& path) {
    if (!stable_token(role)) {
        throw std::invalid_argument("Eingabeprovenienz braucht eine portable Rolle.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw InputOutputError("Provenienzeingabe konnte nicht geoeffnet werden.");
    Sha256 hash;
    std::uint64_t size = 0u;
    std::vector<char> buffer(64u * 1024u);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
            size += static_cast<std::uint64_t>(count);
        }
    }
    if (!input.eof()) throw InputOutputError("Provenienzeingabe konnte nicht gelesen werden.");
    return {
        std::move(role), size, hash.finish(), std::filesystem::absolute(path).lexically_normal()};
}

std::string make_portable_build_identity(const BuildProvenance& provenance) {
    validate_provenance(provenance);
    return sha256_bytes(format_build_provenance_json(provenance));
}

std::string format_build_provenance_json(const BuildProvenance& provenance) {
    validate_provenance(provenance);
    std::ostringstream output;
    write_json_report_header(output, "katana-build-provenance", "build-provenance");
    output << ",\"provenance_version\":" << input_provenance_schema_version
           << ",\"tool_version\":" << quote_json(provenance.tool_version)
           << ",\"manifest_version\":" << provenance.manifest_version
           << ",\"manifest_sha256\":" << quote_json(provenance.manifest_sha256)
           << ",\"directives_sha256\":" << quote_json(provenance.directives_sha256)
           << ",\"ir_version\":" << provenance.ir_version
           << ",\"runtime_abi\":" << provenance.runtime_abi
           << ",\"backend\":" << quote_json(provenance.backend_name)
           << ",\"backend_abi\":" << provenance.backend_abi << ",\"inputs\":[";
    const auto inputs = portable_inputs(provenance);
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        if (index != 0u) output << ',';
        output << "{\"role\":" << quote_json(inputs[index].role)
               << ",\"size\":" << inputs[index].size
               << ",\"sha256\":" << quote_json(inputs[index].sha256) << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace katana::io
