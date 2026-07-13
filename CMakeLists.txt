cmake_minimum_required(VERSION 3.25)

project(
    KatanaRecomp
    VERSION 0.1.0
    DESCRIPTION "Dreamcast SH-4 static recompilation framework"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(katana_core STATIC
    src/decoder/decoder.cpp
)

target_include_directories(katana_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(
        katana_core
        PRIVATE
            /W4
            /permissive-
            /EHsc
    )
else()
    target_compile_options(
        katana_core
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

add_executable(katana-recomp
    src/cli/main.cpp
)

target_link_libraries(
    katana-recomp
    PRIVATE
        katana_core
)

if(MSVC)
    target_compile_options(
        katana-recomp
        PRIVATE
            /W4
            /permissive-
            /EHsc
    )
endif()

enable_testing()

add_executable(katana-decoder-tests
    tests/decoder/decoder_tests.cpp
)

target_link_libraries(
    katana-decoder-tests
    PRIVATE
        katana_core
)

if(MSVC)
    target_compile_options(
        katana-decoder-tests
        PRIVATE
            /W4
            /permissive-
            /EHsc
    )
endif()

add_test(
    NAME katana-decoder-tests
    COMMAND katana-decoder-tests
)
