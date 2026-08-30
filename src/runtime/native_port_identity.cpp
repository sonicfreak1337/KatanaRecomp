#include "katana/runtime/native_port_identity.hpp"

#include "katana/io/input_provenance.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace katana::runtime {
namespace {

class NativePortIdentityMaterial final {
  public:
    void u8(const std::uint8_t value) {
        bytes_.push_back(static_cast<char>(value));
    }

    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void boolean(const bool value) { u8(value ? 1u : 0u); }

    template <typename Enum>
    void enumeration(const Enum value) {
        static_assert(std::is_enum_v<Enum>);
        u32(static_cast<std::uint32_t>(value));
    }

    void count(const std::size_t value) {
        u64(static_cast<std::uint64_t>(value));
    }

    void text(const std::string_view value) {
        count(value.size());
        bytes_.append(value);
    }

    [[nodiscard]] std::string finish() && { return std::move(bytes_); }

  private:
    std::string bytes_;
};

} // namespace

std::string native_port_definition_export_identity(
    const NativePortDefinition& definition) {
    validate_native_port_definition(definition);
    NativePortIdentityMaterial material;
    material.text("katana.native-port-export.identity");
    material.u32(5u);
    material.u32(definition.contract_version);
    material.text(definition.project_id);
    material.text(definition.project_version);
    material.text(definition.executable.content_identity);
    material.text(definition.executable.executable_name);
    material.text(definition.executable.executable_byte_identity);
    material.u32(definition.bootstrap.entry_point);
    material.u32(definition.bootstrap.stack_pointer);
    material.u32(definition.bootstrap.vector_base);
    material.u32(definition.bootstrap.status_register);
    material.u32(definition.bootstrap.fpscr);
    material.u32(definition.bootstrap.cache_control_value);
    material.u32(definition.bootstrap.post_entry_point);
    material.count(definition.bootstrap.post_aot_roots.size());
    for (const auto root : definition.bootstrap.post_aot_roots)
        material.u32(root);
    material.count(definition.bootstrap.post_aot_continuations.size());
    for (const auto& continuation :
         definition.bootstrap.post_aot_continuations) {
        material.u32(continuation.function_entry);
        material.u32(continuation.resume_address);
    }
    material.enumeration(definition.bootstrap.time_policy);
    material.text(definition.bootstrap.symbol);
    material.text(definition.bootstrap.post_cpu_state_identity);
    material.count(definition.bootstrap.writes.size());
    for (const auto& write : definition.bootstrap.writes) {
        material.u32(write.guest_address);
        material.u32(write.byte_size);
        material.text(write.pre_write_identity);
        material.text(write.post_write_identity);
        material.enumeration(write.policy);
    }
    material.text(definition.acceptance.milestone_id);
    material.u32(definition.acceptance.witness_hook_guest_address);
    material.count(definition.checkpoint_runtime_image_ids.size());
    for (const auto image_id : definition.checkpoint_runtime_image_ids)
        material.text(image_id);

    material.count(definition.images.size());
    for (const auto& image : definition.images) {
        material.text(image.image_id);
        material.text(image.content_relative_path);
        material.text(image.byte_identity);
        material.u64(image.file_offset);
        material.u32(image.guest_address);
        material.u32(image.byte_size);
        material.boolean(image.writable);
    }

    material.count(definition.hooks.size());
    for (const auto& hook : definition.hooks) {
        material.u32(hook.guest_address);
        material.u32(hook.covered_size);
        material.enumeration(hook.kind);
        material.enumeration(hook.requirement);
        material.enumeration(hook.original_policy);
        material.text(hook.symbol);
        material.text(hook.code_identity);
        material.enumeration(hook.code_source);
        material.text(hook.code_source_identity);
        material.text(hook.provider_implementation_identity);
    }

    material.count(definition.hardware_resolutions.size());
    for (const auto& resolution : definition.hardware_resolutions) {
        material.u32(resolution.instruction_address);
        material.u32(resolution.hook_guest_address);
    }

    material.count(definition.provider_semantic_contracts.size());
    for (const auto& contract : definition.provider_semantic_contracts) {
        material.u32(contract.contract_version);
        material.u32(contract.hook_guest_address);
        material.boolean(contract.authoritative);
        material.text(contract.provider_symbol);
        material.text(contract.semantic_identity);
        material.text(contract.expected_owner_semantic_identity);
        material.text(contract.provider_implementation_identity);
        material.count(contract.guards.size());
        for (const auto& guard : contract.guards) {
            material.u32(guard.order);
            material.text(guard.expression);
            material.text(guard.path_identity);
        }
        material.count(contract.effects.size());
        for (const auto& effect : contract.effects) {
            material.u32(effect.order);
            material.enumeration(effect.operation);
            material.enumeration(effect.resource_kind);
            material.u32(effect.width);
            material.boolean(effect.canonical_address_known);
            material.u32(effect.canonical_address);
            material.u32(effect.write_mask);
            material.u32(effect.clear_mask);
            material.text(effect.region);
            material.text(effect.register_name);
            material.text(effect.resource);
            material.text(effect.address_expression);
            material.text(effect.value_expression);
            material.text(effect.result_expression);
            material.text(effect.path_identity);
        }
        material.enumeration(contract.result.action);
        material.u32(contract.result.gpr_write_mask);
        material.u32(contract.result.special_write_mask);
        material.u32(contract.result.status_write_mask);
        material.text(contract.result.target_expression);
        material.text(contract.result.error_expression);
        material.text(contract.result.cpu_state_expression);
        material.text(contract.result.title_state_expression);
    }
    material.enumeration(definition.provider_semantic_coverage);
    material.enumeration(definition.input_ownership);
    material.u32(definition.frame_timing.simulation_rate_hz);
    material.u32(definition.frame_timing.default_presentation_rate_hz);
    material.u32(definition.frame_timing.maximum_presentation_rate_hz);
    return io::sha256_bytes(std::move(material).finish());
}

} // namespace katana::runtime
