if(NOT DEFINED KATANA_CLI OR NOT EXISTS "${KATANA_CLI}")
    message(FATAL_ERROR "KATANA_CLI is missing")
endif()
if(NOT DEFINED KATANA_TEST_ROOT)
    message(FATAL_ERROR "KATANA_TEST_ROOT is missing")
endif()

file(REMOVE_RECURSE "${KATANA_TEST_ROOT}")
file(MAKE_DIRECTORY "${KATANA_TEST_ROOT}")

set(SHA_A "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
set(SHA_B "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
set(SHA_C "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc")
set(SHA_D "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd")
set(SHA_E "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee")
set(SHA_F "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")

set(HEADER "{\"kind\":\"katana-native-bringup-authoring\",\"contract_version\":1,\"project_id\":\"fixture-project\",\"project_version\":\"fixture-v1\",\"analysis_identity\":\"${SHA_A}\",\"aot_pack_identity\":\"${SHA_B}\",\"aot_pack_generation\":7}")
set(PROVEN "{\"kind\":\"target\",\"contract_version\":1,\"stage\":\"Proven\",\"transfer_kind\":\"CallRegister\",\"source_owner\":2348814336,\"source_owner_size\":32,\"source_block\":2348814336,\"source_block_size\":8,\"callsite\":2348814340,\"continuation\":2348814344,\"source_owner_code_identity\":\"${SHA_C}\",\"source_block_code_identity\":\"${SHA_B}\",\"callsite_code_identity\":\"${SHA_D}\",\"target\":2348818432,\"target_block_size\":8,\"target_owner\":2348818432,\"target_owner_size\":32,\"target_block_code_identity\":\"${SHA_E}\",\"target_owner_code_identity\":\"${SHA_F}\",\"source_image_id\":\"primary\",\"target_image_id\":\"primary\",\"source_module_identity\":\"${SHA_A}\",\"target_module_identity\":\"${SHA_A}\",\"source_generation\":7,\"target_generation\":7,\"observation\":\"runtime correlation only\",\"static_correlation\":\"exact immutable transfer proof\",\"missing_proof\":\"\",\"proposed_promotion\":\"StaticCompiledTarget\",\"analyzer_path\":\"prepared-native-port-admission/program-index\",\"runtime_contract_identity\":\"\"}")
set(CANDIDATE "{\"kind\":\"target\",\"contract_version\":1,\"stage\":\"Candidate\",\"transfer_kind\":\"CallRegister\",\"source_owner\":2348814336,\"source_owner_size\":32,\"source_block\":2348814344,\"source_block_size\":8,\"callsite\":2348814348,\"continuation\":2348814352,\"source_owner_code_identity\":\"${SHA_C}\",\"source_block_code_identity\":\"${SHA_B}\",\"callsite_code_identity\":\"${SHA_D}\",\"target\":2348822528,\"target_block_size\":8,\"target_owner\":2348822528,\"target_owner_size\":32,\"target_block_code_identity\":\"${SHA_E}\",\"target_owner_code_identity\":\"${SHA_F}\",\"source_image_id\":\"primary\",\"target_image_id\":\"primary\",\"source_module_identity\":\"${SHA_A}\",\"target_module_identity\":\"${SHA_A}\",\"source_generation\":7,\"target_generation\":7,\"observation\":\"runtime target observed\",\"static_correlation\":\"exact execution safety; CFG completeness remains open\",\"missing_proof\":\"closed-outgoing-transfer-proof\",\"proposed_promotion\":\"AnalyzerReproof\",\"analyzer_path\":\"control-flow/indirect-dispatch\",\"runtime_contract_identity\":\"\"}")

set(INPUT_A "${KATANA_TEST_ROOT}/author-a.jsonl")
set(INPUT_B "${KATANA_TEST_ROOT}/author-b.jsonl")
set(ARTIFACT_A "${KATANA_TEST_ROOT}/allow-a.katana-native-bringup")
set(ARTIFACT_B "${KATANA_TEST_ROOT}/allow-b.katana-native-bringup")
file(WRITE "${INPUT_A}" "${HEADER}\n${CANDIDATE}\n${PROVEN}\n")
file(WRITE "${INPUT_B}" "${HEADER}\n${PROVEN}\n${CANDIDATE}\n")

execute_process(
    COMMAND "${KATANA_CLI}" author-native-bringup "${INPUT_A}" --output "${ARTIFACT_A}"
    RESULT_VARIABLE AUTHOR_A_RESULT
    OUTPUT_VARIABLE AUTHOR_A_OUTPUT
    ERROR_VARIABLE AUTHOR_A_ERROR
)
if(NOT AUTHOR_A_RESULT EQUAL 0)
    message(FATAL_ERROR "first authoring failed: ${AUTHOR_A_ERROR}")
endif()
execute_process(
    COMMAND "${KATANA_CLI}" author-native-bringup "${INPUT_B}" --output "${ARTIFACT_B}"
    RESULT_VARIABLE AUTHOR_B_RESULT
    OUTPUT_VARIABLE AUTHOR_B_OUTPUT
    ERROR_VARIABLE AUTHOR_B_ERROR
)
if(NOT AUTHOR_B_RESULT EQUAL 0 OR NOT AUTHOR_A_OUTPUT STREQUAL AUTHOR_B_OUTPUT)
    message(FATAL_ERROR "authoring is not deterministic")
endif()

file(WRITE "${KATANA_TEST_ROOT}/bad.jsonl" "{\"kind\":\"katana-native-bringup-authoring\",\"kind\":\"duplicate\"}\n")
execute_process(
    COMMAND "${KATANA_CLI}" author-native-bringup "${KATANA_TEST_ROOT}/bad.jsonl" --output "${KATANA_TEST_ROOT}/bad.bin"
    RESULT_VARIABLE BAD_RESULT
)
if(BAD_RESULT EQUAL 0)
    message(FATAL_ERROR "duplicate authoring keys were accepted")
endif()

execute_process(
    COMMAND "${KATANA_CLI}" port "${KATANA_TEST_ROOT}/missing.gdi" --output "${KATANA_TEST_ROOT}/strict" --target-name fixture --native-bringup-allowlist "${ARTIFACT_A}"
    RESULT_VARIABLE STRICT_RESULT
    ERROR_VARIABLE STRICT_ERROR
)
if(STRICT_RESULT EQUAL 0 OR NOT STRICT_ERROR MATCHES "ausschliesslich mit.*native-bringup")
    message(FATAL_ERROR "StrictProduct accepted or mishandled an allowlist: ${STRICT_ERROR}")
endif()

execute_process(
    COMMAND "${KATANA_CLI}" port "${KATANA_TEST_ROOT}/missing.gdi" --output "${KATANA_TEST_ROOT}/bringup" --target-name fixture --game-project "${KATANA_TEST_ROOT}/missing-project" --native-port-definition "${KATANA_TEST_ROOT}/missing-port" --native-execution-profile native-bringup --native-bringup-allowlist "${ARTIFACT_A}"
    RESULT_VARIABLE BRINGUP_RESULT
    ERROR_VARIABLE BRINGUP_ERROR
)
if(BRINGUP_RESULT EQUAL 0 OR NOT BRINGUP_ERROR MATCHES "analysis-generation")
    message(FATAL_ERROR "NativeBringup without committed generation was accepted: ${BRINGUP_ERROR}")
endif()

file(REMOVE_RECURSE "${KATANA_TEST_ROOT}")
