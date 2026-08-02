if(NOT DEFINED KATANA_COMPONENT_IDENTITY_ROOT OR
   NOT DEFINED KATANA_COMPONENT_IDENTITY_INPUTS OR
   NOT DEFINED KATANA_COMPONENT_IDENTITY_OUTPUT OR
   NOT DEFINED KATANA_COMPONENT_IDENTITY_STAMP)
    message(FATAL_ERROR "Component identity generation is missing required paths")
endif()

include("${KATANA_COMPONENT_IDENTITY_INPUTS}")

function(katana_expand_component_dependency_closure
         output_variable
         include_implementation_counterparts)
    set(katana_component_pending ${ARGN})
    set(katana_component_closure "")
    while(katana_component_pending)
        list(POP_FRONT katana_component_pending katana_component_relative)
        if(katana_component_relative IN_LIST katana_component_closure)
            continue()
        endif()
        set(katana_component_absolute
            "${KATANA_COMPONENT_IDENTITY_ROOT}/${katana_component_relative}")
        if(NOT EXISTS "${katana_component_absolute}" OR
           IS_DIRECTORY "${katana_component_absolute}")
            message(FATAL_ERROR
                "Component dependency input is missing: "
                "${katana_component_relative}")
        endif()
        list(APPEND katana_component_closure
             "${katana_component_relative}")

        file(READ "${katana_component_absolute}" katana_component_source)
        string(
            REGEX MATCHALL
            "#[ \t]*include[ \t]*\"[^\"]+\""
            katana_component_includes
            "${katana_component_source}"
        )
        get_filename_component(
            katana_component_directory
            "${katana_component_relative}"
            DIRECTORY)
        foreach(katana_component_include IN LISTS katana_component_includes)
            string(
                REGEX REPLACE
                ".*\"([^\"]+)\".*"
                "\\1"
                katana_component_include_path
                "${katana_component_include}"
            )
            if(katana_component_include_path MATCHES "^katana/")
                set(katana_component_candidate
                    "include/${katana_component_include_path}")
            else()
                set(katana_component_candidate
                    "${katana_component_directory}/${katana_component_include_path}")
            endif()
            get_filename_component(
                katana_component_candidate_absolute
                "${KATANA_COMPONENT_IDENTITY_ROOT}/${katana_component_candidate}"
                ABSOLUTE)
            file(
                RELATIVE_PATH
                katana_component_candidate_relative
                "${KATANA_COMPONENT_IDENTITY_ROOT}"
                "${katana_component_candidate_absolute}"
            )
            if(katana_component_candidate_relative STREQUAL
               "include/katana/progress.hpp")
                # Progress is deliberately observational: its implementation
                # cannot change accepted analysis/IR and must not invalidate
                # the expensive persistent analysis layer.
            elseif(NOT katana_component_candidate_relative MATCHES "^\\.\\." AND
               EXISTS "${katana_component_candidate_absolute}" AND
               NOT IS_DIRECTORY "${katana_component_candidate_absolute}" AND
               NOT katana_component_candidate_relative IN_LIST
                   katana_component_closure)
                list(APPEND katana_component_pending
                     "${katana_component_candidate_relative}")
            elseif(katana_component_include_path STREQUAL
                   "katana/build_contract.hpp" AND
                   EXISTS
                   "${KATANA_COMPONENT_IDENTITY_ROOT}/include/katana/build_contract.hpp.in")
                list(APPEND katana_component_pending
                     "include/katana/build_contract.hpp.in")
            endif()
        endforeach()

        if(include_implementation_counterparts AND
           katana_component_relative MATCHES
           "^include/katana/(.+)\\.hpp$")
            set(katana_component_implementation
                "src/${CMAKE_MATCH_1}.cpp")
            if(EXISTS
               "${KATANA_COMPONENT_IDENTITY_ROOT}/${katana_component_implementation}" AND
               NOT katana_component_implementation IN_LIST
                   katana_component_closure)
                list(APPEND katana_component_pending
                     "${katana_component_implementation}")
            endif()
        endif()
    endwhile()
    list(REMOVE_DUPLICATES katana_component_closure)
    list(SORT katana_component_closure)
    set(${output_variable} "${katana_component_closure}" PARENT_SCOPE)
endfunction()

macro(katana_append_component_file material relative_path)
    set(katana_component_absolute
        "${KATANA_COMPONENT_IDENTITY_ROOT}/${relative_path}")
    if(NOT EXISTS "${katana_component_absolute}" OR
       IS_DIRECTORY "${katana_component_absolute}")
        message(FATAL_ERROR
            "Component identity input is missing: ${relative_path}")
    endif()
    file(SHA256 "${katana_component_absolute}" katana_component_digest)
    string(LENGTH "${relative_path}" katana_component_path_length)
    string(APPEND ${material}
        "${katana_component_path_length}:${relative_path}:"
        "${katana_component_digest};")
endmacro()

set(katana_toolchain_material
    "compiler-id=${KATANA_COMPONENT_COMPILER_ID};"
    "compiler-version=${KATANA_COMPONENT_COMPILER_VERSION};"
    "cxx-standard=${KATANA_COMPONENT_CXX_STANDARD};"
    "system-name=${KATANA_COMPONENT_SYSTEM_NAME};"
    "system-processor=${KATANA_COMPONENT_SYSTEM_PROCESSOR};"
    "sizeof-void-p=${KATANA_COMPONENT_SIZEOF_VOID_P};"
    "flags=${KATANA_COMPONENT_CXX_FLAGS};"
    "flags-release=${KATANA_COMPONENT_CXX_FLAGS_RELEASE};"
    "flags-relwithdebinfo=${KATANA_COMPONENT_CXX_FLAGS_RELWITHDEBINFO};")

set(katana_analysis_material
    "katana-analysis-component-v1;${katana_toolchain_material}")
katana_expand_component_dependency_closure(
    KATANA_ANALYSIS_IDENTITY_CLOSURE
    FALSE
    ${KATANA_ANALYSIS_IDENTITY_INPUTS})
foreach(katana_component_file IN LISTS KATANA_ANALYSIS_IDENTITY_CLOSURE)
    katana_append_component_file(
        katana_analysis_material "${katana_component_file}")
endforeach()
string(SHA256 KATANA_ANALYSIS_COMPONENT_IDENTITY
       "${katana_analysis_material}")

set(katana_analysis_cache_material
    "katana-analysis-cache-component-v1;${katana_toolchain_material}")
katana_expand_component_dependency_closure(
    KATANA_ANALYSIS_CACHE_IDENTITY_CLOSURE
    FALSE
    ${KATANA_ANALYSIS_CACHE_IDENTITY_INPUTS})
foreach(katana_component_file
        IN LISTS KATANA_ANALYSIS_CACHE_IDENTITY_CLOSURE)
    katana_append_component_file(
        katana_analysis_cache_material "${katana_component_file}")
endforeach()
string(SHA256 KATANA_ANALYSIS_CACHE_COMPONENT_IDENTITY
       "${katana_analysis_cache_material}")

set(katana_ir_material
    "katana-ir-component-v1;${katana_toolchain_material}")
katana_expand_component_dependency_closure(
    KATANA_IR_IDENTITY_CLOSURE
    FALSE
    ${KATANA_IR_IDENTITY_INPUTS})
foreach(katana_component_file IN LISTS KATANA_IR_IDENTITY_CLOSURE)
    katana_append_component_file(
        katana_ir_material "${katana_component_file}")
endforeach()
string(SHA256 KATANA_IR_COMPONENT_IDENTITY "${katana_ir_material}")

set(katana_codegen_material
    "katana-codegen-component-v1;${katana_toolchain_material}")
katana_expand_component_dependency_closure(
    KATANA_CODEGEN_IDENTITY_CLOSURE
    FALSE
    ${KATANA_CODEGEN_IDENTITY_INPUTS})
# Analysis-cache codecs and latent discovery are owned by the analysis-cache
# component. Codegen includes their public data contracts but must not be
# invalidated when only cache/discovery implementation changes.
list(
    REMOVE_ITEM
    KATANA_CODEGEN_IDENTITY_CLOSURE
    "include/katana/codegen/boot_analysis_cache.hpp"
    "include/katana/codegen/latent_aot_analysis_cache.hpp"
    "include/katana/codegen/latent_aot_registry.hpp"
    "src/codegen/boot_analysis_cache.cpp"
    "src/codegen/latent_aot_analysis_cache.cpp"
    "src/codegen/latent_aot_registry.cpp"
)
foreach(katana_component_file IN LISTS KATANA_CODEGEN_IDENTITY_CLOSURE)
    katana_append_component_file(
        katana_codegen_material "${katana_component_file}")
endforeach()
string(SHA256 KATANA_CODEGEN_COMPONENT_IDENTITY
       "${katana_codegen_material}")

set(katana_orchestration_material
    "katana-orchestration-component-v1;${katana_toolchain_material}")
katana_expand_component_dependency_closure(
    KATANA_ORCHESTRATION_IDENTITY_CLOSURE
    FALSE
    ${KATANA_ORCHESTRATION_IDENTITY_INPUTS})
foreach(katana_component_file
        IN LISTS KATANA_ORCHESTRATION_IDENTITY_CLOSURE)
    katana_append_component_file(
        katana_orchestration_material "${katana_component_file}")
endforeach()
string(SHA256 KATANA_ORCHESTRATION_COMPONENT_IDENTITY
       "${katana_orchestration_material}")

set(katana_component_header
"#pragma once

#include <string_view>

namespace katana::build_contract {

inline constexpr std::string_view analysis_component_identity =
    \"${KATANA_ANALYSIS_COMPONENT_IDENTITY}\";
inline constexpr std::string_view analysis_cache_component_identity =
    \"${KATANA_ANALYSIS_CACHE_COMPONENT_IDENTITY}\";
inline constexpr std::string_view ir_component_identity =
    \"${KATANA_IR_COMPONENT_IDENTITY}\";
inline constexpr std::string_view codegen_component_identity =
    \"${KATANA_CODEGEN_COMPONENT_IDENTITY}\";
inline constexpr std::string_view orchestration_component_identity =
    \"${KATANA_ORCHESTRATION_COMPONENT_IDENTITY}\";

} // namespace katana::build_contract
")

set(katana_write_component_header TRUE)
if(EXISTS "${KATANA_COMPONENT_IDENTITY_OUTPUT}")
    file(READ "${KATANA_COMPONENT_IDENTITY_OUTPUT}"
         katana_existing_component_header)
    if(katana_existing_component_header STREQUAL katana_component_header)
        set(katana_write_component_header FALSE)
    endif()
endif()
if(katana_write_component_header)
    get_filename_component(
        katana_component_output_directory
        "${KATANA_COMPONENT_IDENTITY_OUTPUT}"
        DIRECTORY)
    file(MAKE_DIRECTORY "${katana_component_output_directory}")
    file(WRITE "${KATANA_COMPONENT_IDENTITY_OUTPUT}"
         "${katana_component_header}")
endif()

get_filename_component(
    katana_component_stamp_directory
    "${KATANA_COMPONENT_IDENTITY_STAMP}"
    DIRECTORY)
file(MAKE_DIRECTORY "${katana_component_stamp_directory}")
# The stamp is the freshness output. The generated header remains
# copy-if-different so observational changes do not rebuild the CLI, while
# every successful identity scan makes subsequent no-op builds truly clean.
file(TOUCH "${KATANA_COMPONENT_IDENTITY_STAMP}")
