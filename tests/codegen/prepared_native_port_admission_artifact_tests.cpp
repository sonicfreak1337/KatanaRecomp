#include "katana/codegen/prepared_native_port_admission_artifact.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using katana::codegen::PreparedNativePortAdmissionArtifact;
using katana::codegen::PreparedNativePortAdmissionArtifactIdentity;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

PreparedNativePortAdmissionArtifact sample_artifact() {
    PreparedNativePortAdmissionArtifact artifact;
    artifact.identity.analysis_artifact_identity = std::string(64u, '2');
    artifact.identity.analysis_archive_sha256 = std::string(64u, 'a');
    artifact.identity.game_project_identity =
        "sha256:" + std::string(64u, '3');
    artifact.identity.native_port_identity = std::string(64u, '4');
    artifact.identity.native_port_artifact_identity =
        "native-port-artifact-a";
    artifact.identity.admission_implementation_identity =
        std::string(64u, '5');
    artifact.identity.analyzer_abi = 17u;
    artifact.identity.backend_abi = 23u;
    artifact.identity.key =
        katana::codegen::
            prepared_native_port_admission_artifact_identity_key(
                artifact.identity);
    artifact.emitted_program_digest = std::string(64u, 'b');
    artifact.function_digests = {
        {0x1000u,
         std::string(64u, 'c'),
         {{0x1000u, std::string(64u, 'd')},
          {0x1010u, std::string(64u, 'e')}}},
        {0x2000u,
         std::string(64u, 'f'),
         {{0x2000u, std::string(64u, '1')}}},
    };
    artifact.admission_payload = {1u, 2u, 3u, 4u};
    return artifact;
}

template <typename Mutate>
void require_identity_field_changes_key(
    const PreparedNativePortAdmissionArtifactIdentity& baseline,
    Mutate&& mutate,
    const std::string& field) {
    auto changed = baseline;
    mutate(changed);
    changed.key.clear();
    const auto key = katana::codegen::
        prepared_native_port_admission_artifact_identity_key(changed);
    require(key != baseline.key, field + " did not change the cache key");
}

} // namespace

int main() {
    try {
        const auto artifact = sample_artifact();
        require(
            katana::codegen::
                prepared_native_port_admission_artifact_cacheable(artifact),
            "sample artifact was not cacheable");

        const auto bytes = katana::codegen::
            serialize_prepared_native_port_admission_artifact(artifact);
        const auto parsed = katana::codegen::
            parse_prepared_native_port_admission_artifact(
                artifact.identity.key, bytes);
        require(
            parsed.state == katana::codegen::
                                PreparedNativePortAdmissionArtifactState::Hit,
            "round trip did not hit");
        require(parsed.artifact == artifact, "round trip changed artifact");

        const auto wrong_key = katana::codegen::
            parse_prepared_native_port_admission_artifact(
                std::string(64u, '9'), bytes);
        require(
            wrong_key.state ==
                katana::codegen::
                    PreparedNativePortAdmissionArtifactState::Miss,
            "wrong key was not a clean miss");

        auto corrupted = bytes;
        corrupted.back() ^= 0xFFu;
        const auto corrupt = katana::codegen::
            parse_prepared_native_port_admission_artifact(
                artifact.identity.key, corrupted);
        require(
            corrupt.state ==
                katana::codegen::
                    PreparedNativePortAdmissionArtifactState::Corrupt,
            "corrupt payload was not rejected");

        const auto& identity = artifact.identity;
        require_identity_field_changes_key(
            identity,
            [](auto& value) {
                value.analysis_artifact_identity += "-changed";
            },
            "analysis artifact identity");
        require_identity_field_changes_key(
            identity,
            [](auto& value) { value.analysis_archive_sha256[0] = '9'; },
            "analysis archive digest");
        require_identity_field_changes_key(
            identity,
            [](auto& value) { value.game_project_identity += "-changed"; },
            "GameProject identity");
        require_identity_field_changes_key(
            identity,
            [](auto& value) { value.native_port_identity += "-changed"; },
            "NativePort identity");
        require_identity_field_changes_key(
            identity,
            [](auto& value) {
                value.native_port_artifact_identity += "-changed";
            },
            "NativePort artifact identity");
        require_identity_field_changes_key(
            identity,
            [](auto& value) {
                value.admission_implementation_identity += "-changed";
            },
            "admission implementation identity");
        require_identity_field_changes_key(
            identity,
            [](auto& value) { ++value.analyzer_abi; },
            "analyzer ABI");
        require_identity_field_changes_key(
            identity,
            [](auto& value) { ++value.backend_abi; },
            "backend ABI");

        auto partial = artifact;
        partial.identity.native_port_artifact_identity.clear();
        partial.identity.key = katana::codegen::
            prepared_native_port_admission_artifact_identity_key(
                partial.identity);
        require(
            !katana::codegen::
                 prepared_native_port_admission_artifact_cacheable(partial),
            "partial binding was cacheable");
        bool rejected = false;
        try {
            static_cast<void>(katana::codegen::
                serialize_prepared_native_port_admission_artifact(partial));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "partial binding was not rejected");

        std::cout << "prepared native-port admission artifact tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "prepared native-port admission artifact test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
