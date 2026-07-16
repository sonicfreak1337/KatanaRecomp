#include "katana/codegen/port_export.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/naming.hpp"
#include "katana/codegen/project.hpp"
#include "katana/codegen/source_map.hpp"
#include "katana/io/input_output_error.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/runtime/abi.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace katana::codegen {
namespace {

bool valid_target_name(const std::string_view value) noexcept {
    if (value.empty() || !std::isalpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    const bool valid = std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
    });
    return valid && value != "katana-recomp" && value != "katana_runtime" &&
           value != "katana_core" && value != "katana_generated" && value != "all" &&
           value != "clean" && value != "install" && value != "test" && value != "help" &&
           value != "rebuild_cache";
}

constexpr std::string_view port_namespace = "katana_port_generated";

std::vector<katana::ir::Function>
select_functions(const std::span<const katana::ir::Function> program,
                 const TranslationUnitPartition& partition) {
    std::vector<katana::ir::Function> selected;
    selected.reserve(partition.function_indices.size());
    for (const auto index : partition.function_indices) {
        if (index >= program.size()) {
            throw std::out_of_range("Portpartition verweist auf eine fehlende Funktion.");
        }
        selected.push_back(program[index]);
    }
    std::sort(selected.begin(), selected.end(), [](const auto& left, const auto& right) {
        return left.entry_address < right.entry_address;
    });
    return selected;
}

std::string generated_header(const std::string& entry_namespace) {
    return "#pragma once\n\n"
           "#include \"katana/runtime/runtime.hpp\"\n\n"
           "namespace " +
           entry_namespace +
           " {\n"
           "void run(katana::runtime::CpuState& cpu);\n"
           "}\n";
}

std::string handwritten_main(const std::string& entry_namespace) {
    return "#include \"katana_port.hpp\"\n"
           "#include \"katana/runtime/gdi.hpp\"\n"
           "#include <exception>\n#include <filesystem>\n#include <iostream>\n#include "
           "<string_view>\n\n"
           "int main(const int argc, const char* const* argv) {\n"
           "    try {\n"
           "        if (argc == 3 && std::string_view(argv[1]) == \"--gdi\") {\n"
           "            const auto disc = "
           "katana::runtime::GdiDiscSource::open(std::filesystem::path(argv[2]));\n"
           "            std::cout << \"DiscSource bereit: \" << disc->identity() << '\\n';\n"
           "            return 0;\n"
           "        }\n"
           "        if (argc == 2 && std::string_view(argv[1]) == \"--run-generated\") {\n"
           "            katana::runtime::CpuState cpu;\n"
           "            " +
           entry_namespace +
           "::run(cpu);\n"
           "            std::cout << \"Generierter Einstieg beendet.\\n\";\n"
           "            return 0;\n"
           "        }\n"
           "        std::cout << \"Katana-Port bereit. Optionen: --gdi <Quelle>, "
           "--run-generated\\n\";\n"
           "        return 0;\n"
           "    } catch (const std::exception& error) {\n"
           "        std::cerr << \"Portlauf fehlgeschlagen: \" << error.what() << '\\n';\n"
           "        return 1;\n"
           "    }\n"
           "}\n";
}

std::string root_cmake() {
    return "cmake_minimum_required(VERSION 3.25)\n"
           "project(KatanaPort LANGUAGES CXX)\n"
           "set(KATANA_RUNTIME_ROOT \"\" CACHE PATH \"KatanaRecomp source root\")\n"
           "if(NOT TARGET katana_runtime)\n"
           "  if(KATANA_RUNTIME_ROOT STREQUAL \"\")\n"
           "    message(FATAL_ERROR \"Set KATANA_RUNTIME_ROOT to the compatible KatanaRecomp "
           "source tree\")\n"
           "  endif()\n"
           "  add_subdirectory(\"${KATANA_RUNTIME_ROOT}\" \"${CMAKE_BINARY_DIR}/katana-runtime\")\n"
           "endif()\n"
           "add_subdirectory(generated)\n"
           "target_link_libraries(katana_generated PUBLIC katana_runtime)\n"
           "include(\"${CMAKE_CURRENT_SOURCE_DIR}/generated/katana-port.cmake\")\n";
}

std::string port_cmake(const std::string& target_name) {
    return "add_executable(" + target_name +
           " \"${CMAKE_CURRENT_LIST_DIR}/../src/main.cpp\")\n"
           "target_compile_features(" +
           target_name +
           " PRIVATE cxx_std_20)\n"
           "target_include_directories(" +
           target_name +
           " PRIVATE \"${CMAKE_CURRENT_LIST_DIR}/include\")\n"
           "target_link_libraries(" +
           target_name + " PRIVATE katana_generated katana_runtime)\n";
}

std::string port_metadata(const PortExportOptions& options,
                          const std::size_t function_count,
                          const std::span<const TranslationUnitPartition> partitions,
                          const std::uint32_t entry_address,
                          const std::size_t boot_size,
                          const std::string_view project_identity) {
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-port-project", "port-project");
    output << ",\"contract_version\":" << port_project_contract_version
           << ",\"target_name\":" << katana::io::quote_json(options.target_name)
           << ",\"runtime_abi\":" << katana::runtime::abi_version
           << ",\"backend_abi\":" << backend_interface_abi_version
           << ",\"project_identity\":" << katana::io::quote_json(project_identity)
           << ",\"entry_address\":" << entry_address << ",\"boot_size\":" << boot_size
           << ",\"function_count\":" << function_count << ",\"partitions\":[";
    for (std::size_t index = 0u; index < partitions.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& partition = partitions[index];
        output << "{\"index\":" << partition.index
               << ",\"functions\":" << partition.function_indices.size()
               << ",\"instructions\":" << partition.instruction_count
               << ",\"first_entry\":" << partition.first_entry_address
               << ",\"last_entry\":" << partition.last_entry_address << '}';
    }
    output << "]}";
    return output.str();
}

void write_user_file_once(const std::filesystem::path& root,
                          const std::filesystem::path& relative,
                          const std::string_view content) {
    auto candidate = root;
    for (const auto& component : relative) {
        candidate /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::runtime_error("Port-Bootstrappfad enthaelt einen symbolischen Link.");
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("Port-Bootstrappfad konnte nicht geprueft werden.");
        }
    }
    const auto path = root / relative;
    if (std::filesystem::exists(path)) return;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw katana::io::InputOutputError("Port-Bootstrapdatei konnte nicht geoeffnet werden.");
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output)
        throw katana::io::InputOutputError("Port-Bootstrapdatei konnte nicht geschrieben werden.");
}

bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::filesystem::path resolve_existing_parents(std::filesystem::path path) {
    std::vector<std::filesystem::path> missing;
    std::error_code error;
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        if (error) throw std::runtime_error("Port-Ausgabepfad konnte nicht geprueft werden.");
        missing.push_back(path.filename());
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    auto resolved = std::filesystem::canonical(path);
    for (auto iterator = missing.rbegin(); iterator != missing.rend(); ++iterator)
        resolved /= *iterator;
    return resolved.lexically_normal();
}

} // namespace

PortExportResult export_dreamcast_port_project(const PreparedPortProgram& prepared,
                                               const std::filesystem::path& output_root,
                                               const PortExportOptions& options) {
    if (output_root.empty() || !valid_target_name(options.target_name) ||
        options.tool_version.empty() || prepared.entry_address == 0u || prepared.program.empty()) {
        throw std::invalid_argument(
            "Portexport braucht vorbereitetes IR, Einstieg, Ausgabe, Zielkennung und "
            "Werkzeugversion.");
    }
    if (!prepared.analysis.recursive.diagnostics.empty()) {
        throw std::runtime_error("Portanalyse enthaelt unbekannte Instruktionen.");
    }
    const auto unresolved = std::count_if(prepared.analysis.indirect_control_flow.begin(),
                                          prepared.analysis.indirect_control_flow.end(),
                                          [](const auto& resolution) {
                                              return resolution.status ==
                                                     katana::analysis::ResolutionStatus::Unresolved;
                                          });
    if (unresolved != 0u) {
        throw std::runtime_error("Portanalyse ist unvollstaendig: " + std::to_string(unresolved) +
                                 " ungeloeste Kontrollflussstellen.");
    }
    katana::ir::require_valid_program(prepared.program);
    const auto partitions =
        partition_translation_units(prepared.program, options.partition_options);
    if (partitions.empty()) throw std::runtime_error("Portcodegen erzeugte keine Partition.");

    std::vector<std::uint32_t> global_entries;
    global_entries.reserve(prepared.program.size());
    for (const auto& function : prepared.program)
        global_entries.push_back(function.entry_address);
    const CppBackend backend;
    std::vector<ProjectArtifact> artifacts;
    artifacts.reserve(partitions.size() + 9u);
    for (const auto& partition : partitions) {
        auto functions = select_functions(prepared.program, partition);
        const auto contains_program_entry =
            std::any_of(functions.begin(), functions.end(), [&prepared](const auto& function) {
                return function.entry_address == prepared.entry_address;
            });
        const BackendRequest request{functions,
                                     functions.front().entry_address,
                                     {},
                                     global_entries,
                                     port_namespace,
                                     contains_program_entry,
                                     true,
                                     prepared.entry_address};
        auto source = backend.emit(request).joined_text();
        artifacts.push_back({std::filesystem::path("code") /
                                 deterministic_translation_unit_name(partition, prepared.program),
                             std::move(source)});
    }
    const auto entry_partition =
        std::find_if(partitions.begin(), partitions.end(), [&prepared](const auto& partition) {
            return std::any_of(partition.function_indices.begin(),
                               partition.function_indices.end(),
                               [&prepared](const auto index) {
                                   return prepared.program[index].entry_address ==
                                          prepared.entry_address;
                               });
        });
    if (entry_partition == partitions.end()) {
        throw std::runtime_error("Portcodegen besitzt keine Einstiegspartition.");
    }
    const auto entry_namespace = std::string(port_namespace);
    const auto source_map = build_address_source_map(prepared.image, artifacts);
    const auto control_flow_graph = katana::analysis::build_control_flow_graph(prepared.analysis);
    const auto call_graph = katana::analysis::build_call_graph(prepared.analysis);
    katana::io::BuildProvenance provenance;
    provenance.tool_version = options.tool_version;
    provenance.manifest_version = port_project_contract_version;
    provenance.manifest_sha256 = katana::io::sha256_bytes(
        options.target_name + ":" + std::to_string(port_project_contract_version));
    provenance.ir_version = 2u;
    provenance.runtime_abi = katana::runtime::abi_version;
    provenance.backend_name = "cpp";
    provenance.backend_abi = backend_interface_abi_version;
    provenance.inputs.assign(prepared.inputs.begin(), prepared.inputs.end());

    artifacts.push_back({"include/katana_port.hpp", generated_header(entry_namespace)});
    artifacts.push_back({"katana-port.cmake", port_cmake(options.target_name)});
    artifacts.push_back({"metadata/port-project.json",
                         port_metadata(options,
                                       prepared.program.size(),
                                       partitions,
                                       prepared.entry_address,
                                       prepared.boot_size,
                                       prepared.project_identity)});
    artifacts.push_back(
        {"metadata/provenance.json", katana::io::format_build_provenance_json(provenance)});
    artifacts.push_back({"metadata/source-map.json", serialize_address_source_map(source_map)});
    artifacts.push_back(
        {"metadata/cfg.json", katana::analysis::serialize_analysis_graph_json(control_flow_graph)});
    artifacts.push_back(
        {"metadata/cfg.dot", katana::analysis::serialize_analysis_graph_dot(control_flow_graph)});
    artifacts.push_back(
        {"metadata/callgraph.json", katana::analysis::serialize_analysis_graph_json(call_graph)});
    artifacts.push_back(
        {"metadata/callgraph.dot", katana::analysis::serialize_analysis_graph_dot(call_graph)});

    const auto absolute_root = std::filesystem::absolute(output_root).lexically_normal();
    const auto resolved_root = resolve_existing_parents(absolute_root);
    if (!options.forbidden_source_root.empty()) {
        const auto source_root = std::filesystem::canonical(options.forbidden_source_root);
        if (path_is_within(resolved_root, source_root)) {
            throw std::invalid_argument(
                "Port-Ausgabe muss ausserhalb des KatanaRecomp-Quellbaums liegen.");
        }
    }
    std::error_code root_error;
    const auto root_status = std::filesystem::symlink_status(absolute_root, root_error);
    if (!root_error && std::filesystem::is_symlink(root_status)) {
        throw std::runtime_error("Port-Ausgabeziel darf kein symbolischer Link sein.");
    }
    if (root_error && root_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("Port-Ausgabeziel konnte nicht geprueft werden.");
    }
    std::filesystem::create_directories(absolute_root);
    const auto canonical_root = std::filesystem::canonical(absolute_root);
    if (!options.forbidden_source_root.empty() &&
        path_is_within(canonical_root, std::filesystem::canonical(options.forbidden_source_root))) {
        throw std::invalid_argument("Kanonische Port-Ausgabe liegt im KatanaRecomp-Quellbaum.");
    }
    const auto write = write_codegen_project(canonical_root / "generated", std::move(artifacts));
    write_user_file_once(canonical_root, "CMakeLists.txt", root_cmake());
    write_user_file_once(canonical_root, ".gitignore", "/build/\n");
    write_user_file_once(canonical_root, "src/main.cpp", handwritten_main(entry_namespace));

    return {canonical_root,
            prepared.program.size(),
            partitions.size(),
            write.written_files.size(),
            write.removed_files.size(),
            {"gdi-validated",
             "boot-image-loaded",
             "analysis-complete",
             "ir-lowered",
             "partitioned-codegen-complete",
             "port-project-written"}};
}

PortExportResult export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                                               const std::filesystem::path& output_root,
                                               const PortExportOptions& options) {
    if (gdi_path.empty()) {
        throw std::invalid_argument("Portexport braucht eine GDI-Quelle.");
    }
    const auto disc = katana::platform::load_dreamcast_gdi_boot(gdi_path);
    auto image = katana::platform::make_dreamcast_disc_executable(disc);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    auto program = katana::ir::lower_program(analysis);
    static_cast<void>(katana::ir::optimize_program(program));
    std::vector<katana::io::InputProvenance> inputs;
    inputs.push_back(katana::io::capture_input_provenance("gdi-descriptor", gdi_path));
    for (const auto& track : disc.source->descriptor().tracks) {
        inputs.push_back(katana::io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path));
    }
    return export_dreamcast_port_project({image,
                                          analysis,
                                          program,
                                          inputs,
                                          katana::platform::dreamcast_disc_boot_address,
                                          disc.boot_file.size(),
                                          {}},
                                         output_root,
                                         options);
}

} // namespace katana::codegen
