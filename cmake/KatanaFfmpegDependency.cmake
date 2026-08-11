set(KATANA_FFMPEG_PACKAGE_NAME "ffmpeg-n8.1.2-34-g9b6c8969e0-win64-lgpl-shared-8.1")
set(KATANA_FFMPEG_ARCHIVE_NAME "${KATANA_FFMPEG_PACKAGE_NAME}.zip")
set(
    KATANA_FFMPEG_ARCHIVE_URL
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-31-14-10/${KATANA_FFMPEG_ARCHIVE_NAME}"
    CACHE STRING
    "Pinned FFmpeg LGPL archive URL"
)
set(
    KATANA_FFMPEG_ARCHIVE_SHA256
    "c222a490dde4e7059f45495deef6bfb98dbcacc2b43df5b607546252037aa95c"
)
set(
    KATANA_FFMPEG_ROOT
    ""
    CACHE PATH
    "Override extracted FFmpeg 8.1.2 LGPL shared SDK root"
)
set(
    KATANA_FFMPEG_ARCHIVE
    ""
    CACHE FILEPATH
    "Optional pre-downloaded pinned FFmpeg LGPL archive"
)
set(KATANA_FFMPEG_AUTOMATIC_ROOT FALSE)
set(KATANA_FFMPEG_NOTICE_FILE "${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg/NOTICE.txt")
set(KATANA_FFMPEG_BUILD_CONFIGURATION_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg/BUILD-CONFIGURATION.txt")
set(KATANA_FFMPEG_DEVELOPMENT_ONLY_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg/DEVELOPMENT-ONLY.txt")
if(NOT EXISTS "${KATANA_FFMPEG_NOTICE_FILE}")
    message(FATAL_ERROR "FFmpeg redistribution notice is missing")
endif()
if(NOT EXISTS "${KATANA_FFMPEG_BUILD_CONFIGURATION_FILE}")
    message(FATAL_ERROR "FFmpeg build configuration record is missing")
endif()
if(NOT EXISTS "${KATANA_FFMPEG_DEVELOPMENT_ONLY_FILE}")
    message(FATAL_ERROR "FFmpeg development-only marker is missing")
endif()

set(
    KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE
    ""
    CACHE FILEPATH
    "Verified complete corresponding-source bundle for the pinned FFmpeg DLL closure"
)
set(
    KATANA_FFMPEG_CORRESPONDING_SOURCE_SHA256
    ""
    CACHE STRING
    "SHA-256 of KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE"
)
if(NOT KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE STREQUAL "")
    if(NOT EXISTS "${KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE}" OR
       IS_DIRECTORY "${KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE}" OR
       IS_SYMLINK "${KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE}")
        message(FATAL_ERROR
            "FFmpeg corresponding-source bundle must be a regular non-link file")
    endif()
    get_filename_component(
        KATANA_FFMPEG_CORRESPONDING_SOURCE_EXTENSION
        "${KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE}"
        LAST_EXT
    )
    string(TOLOWER
        "${KATANA_FFMPEG_CORRESPONDING_SOURCE_EXTENSION}"
        KATANA_FFMPEG_CORRESPONDING_SOURCE_EXTENSION)
    if(NOT KATANA_FFMPEG_CORRESPONDING_SOURCE_EXTENSION STREQUAL ".zip")
        message(FATAL_ERROR "FFmpeg corresponding-source bundle must be a ZIP archive")
    endif()
    string(TOLOWER
        "${KATANA_FFMPEG_CORRESPONDING_SOURCE_SHA256}"
        KATANA_FFMPEG_CORRESPONDING_SOURCE_SHA256_NORMALIZED)
    if(NOT KATANA_FFMPEG_CORRESPONDING_SOURCE_SHA256_NORMALIZED MATCHES
       "^[0-9a-f]{64}$")
        message(FATAL_ERROR
            "FFmpeg corresponding-source bundle requires an exact SHA-256")
    endif()
    file(SHA256
        "${KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE}"
        KATANA_FFMPEG_CORRESPONDING_SOURCE_ACTUAL_SHA256)
    if(NOT KATANA_FFMPEG_CORRESPONDING_SOURCE_ACTUAL_SHA256 STREQUAL
       KATANA_FFMPEG_CORRESPONDING_SOURCE_SHA256_NORMALIZED)
        message(FATAL_ERROR "FFmpeg corresponding-source bundle hash mismatch")
    endif()
    file(REAL_PATH
        "${KATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE}"
        KATANA_FFMPEG_REDISTRIBUTION_FILE)
    set(KATANA_FFMPEG_REDISTRIBUTION_FILENAME
        "FFmpeg-Corresponding-Source.zip")
    set(KATANA_FFMPEG_REDISTRIBUTION_READY TRUE)
else()
    set(KATANA_FFMPEG_REDISTRIBUTION_FILE
        "${KATANA_FFMPEG_DEVELOPMENT_ONLY_FILE}")
    set(KATANA_FFMPEG_REDISTRIBUTION_FILENAME
        "FFmpeg-DEVELOPMENT-ONLY.txt")
    set(KATANA_FFMPEG_REDISTRIBUTION_READY FALSE)
endif()

if(KATANA_FFMPEG_ROOT STREQUAL "" AND NOT "$ENV{KATANA_FFMPEG_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{KATANA_FFMPEG_ROOT}" KATANA_FFMPEG_ROOT)
endif()
if(KATANA_FFMPEG_ROOT STREQUAL "")
    set(KATANA_FFMPEG_AUTOMATIC_ROOT TRUE)
    foreach(
        KATANA_FFMPEG_CANDIDATE
        IN ITEMS
            "${CMAKE_CURRENT_SOURCE_DIR}/deps-cache/ffmpeg-8.1.2-lgpl-shared"
            "${CMAKE_CURRENT_SOURCE_DIR}/../deps-cache/ffmpeg-8.1.2-lgpl-shared"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../deps-cache/ffmpeg-8.1.2-lgpl-shared"
    )
        if(EXISTS "${KATANA_FFMPEG_CANDIDATE}/include/libavcodec/avcodec.h")
            set(KATANA_FFMPEG_ROOT "${KATANA_FFMPEG_CANDIDATE}")
            break()
        endif()
    endforeach()
endif()
if(KATANA_FFMPEG_ROOT STREQUAL "")
    set(KATANA_FFMPEG_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/katana-ffmpeg")
    file(MAKE_DIRECTORY "${KATANA_FFMPEG_DOWNLOAD_DIR}")
    if(KATANA_FFMPEG_ARCHIVE STREQUAL "")
        set(KATANA_FFMPEG_ARCHIVE "${KATANA_FFMPEG_DOWNLOAD_DIR}/${KATANA_FFMPEG_ARCHIVE_NAME}")
    endif()
    if(EXISTS "${KATANA_FFMPEG_ARCHIVE}")
        file(SHA256 "${KATANA_FFMPEG_ARCHIVE}" KATANA_FFMPEG_ARCHIVE_ACTUAL_HASH)
        if(NOT KATANA_FFMPEG_ARCHIVE_ACTUAL_HASH STREQUAL KATANA_FFMPEG_ARCHIVE_SHA256)
            message(FATAL_ERROR "Pinned FFmpeg archive hash mismatch: ${KATANA_FFMPEG_ARCHIVE}")
        endif()
    else()
        file(
            DOWNLOAD
            "${KATANA_FFMPEG_ARCHIVE_URL}"
            "${KATANA_FFMPEG_ARCHIVE}"
            EXPECTED_HASH "SHA256=${KATANA_FFMPEG_ARCHIVE_SHA256}"
            TLS_VERIFY ON
            STATUS KATANA_FFMPEG_DOWNLOAD_STATUS
            SHOW_PROGRESS
        )
        list(GET KATANA_FFMPEG_DOWNLOAD_STATUS 0 KATANA_FFMPEG_DOWNLOAD_CODE)
        list(GET KATANA_FFMPEG_DOWNLOAD_STATUS 1 KATANA_FFMPEG_DOWNLOAD_MESSAGE)
        if(NOT KATANA_FFMPEG_DOWNLOAD_CODE EQUAL 0)
            message(FATAL_ERROR "Pinned FFmpeg download failed: ${KATANA_FFMPEG_DOWNLOAD_MESSAGE}")
        endif()
    endif()
    set(KATANA_FFMPEG_EXTRACT_DIR "${KATANA_FFMPEG_DOWNLOAD_DIR}/extracted")
    set(KATANA_FFMPEG_ROOT "${KATANA_FFMPEG_EXTRACT_DIR}/${KATANA_FFMPEG_PACKAGE_NAME}")
    if(NOT EXISTS "${KATANA_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
        file(MAKE_DIRECTORY "${KATANA_FFMPEG_EXTRACT_DIR}")
        file(ARCHIVE_EXTRACT INPUT "${KATANA_FFMPEG_ARCHIVE}" DESTINATION "${KATANA_FFMPEG_EXTRACT_DIR}")
    endif()
endif()
if(KATANA_FFMPEG_AUTOMATIC_ROOT)
    # The upstream archive also contains command-line programs, documentation,
    # presets and libraries outside Katana's five-DLL media closure. Keep the
    # verified SDK cache small and make it impossible to package those tools by
    # accident. User-supplied override roots are never modified.
    file(REMOVE_RECURSE "${KATANA_FFMPEG_ROOT}/doc" "${KATANA_FFMPEG_ROOT}/presets")
    file(GLOB KATANA_FFMPEG_UNUSED_BINARIES
        "${KATANA_FFMPEG_ROOT}/bin/*.exe"
        "${KATANA_FFMPEG_ROOT}/bin/avdevice-*.dll"
        "${KATANA_FFMPEG_ROOT}/bin/avfilter-*.dll"
    )
    if(KATANA_FFMPEG_UNUSED_BINARIES)
        file(REMOVE ${KATANA_FFMPEG_UNUSED_BINARIES})
    endif()
    file(GLOB KATANA_FFMPEG_LIBRARY_CANDIDATES "${KATANA_FFMPEG_ROOT}/lib/*")
    foreach(KATANA_FFMPEG_LIBRARY_CANDIDATE IN LISTS KATANA_FFMPEG_LIBRARY_CANDIDATES)
        get_filename_component(KATANA_FFMPEG_LIBRARY_NAME
                               "${KATANA_FFMPEG_LIBRARY_CANDIDATE}" NAME)
        if(NOT KATANA_FFMPEG_LIBRARY_NAME MATCHES
           "^(avformat|avcodec|avutil|swresample|swscale)\\.lib$")
            file(REMOVE_RECURSE "${KATANA_FFMPEG_LIBRARY_CANDIDATE}")
        endif()
    endforeach()
endif()
if(NOT EXISTS "${KATANA_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "Pinned FFmpeg SDK root is invalid: ${KATANA_FFMPEG_ROOT}")
endif()
file(REAL_PATH "${KATANA_FFMPEG_ROOT}" KATANA_FFMPEG_ROOT)

# The native product may add KatanaRecomp as a subdirectory and invoke the
# deployment helper from its parent directory. CMake functions resolve normal
# variables in the caller scope, so capture the verified redistribution
# closure in directory-independent global properties instead of relying on
# the subdirectory's transient variables.
set_property(GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_ROOT
    "${KATANA_FFMPEG_ROOT}")
set_property(GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_NOTICE_FILE
    "${KATANA_FFMPEG_NOTICE_FILE}")
set_property(GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_BUILD_CONFIGURATION_FILE
    "${KATANA_FFMPEG_BUILD_CONFIGURATION_FILE}")
set_property(GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_REDISTRIBUTION_FILE
    "${KATANA_FFMPEG_REDISTRIBUTION_FILE}")
set_property(GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_REDISTRIBUTION_FILENAME
    "${KATANA_FFMPEG_REDISTRIBUTION_FILENAME}")

set(
    KATANA_FFMPEG_PINNED_FILES
    "bin/avformat-62.dll|148cfc25efafb0b63998740685e7789636309bb8a8304cbde3241bda66dbc77b"
    "bin/avcodec-62.dll|4b9f8f2dcf6ff62f8a3136f31afdd31f90c24625ce8ea618951e356a7a5a8ce2"
    "bin/avutil-60.dll|93ba0919a68532a718edc8de25c5f4c48a68e5ce3609fd128602c129783fab73"
    "bin/swresample-6.dll|a7a66dffe5a5611ef7f7879ab4def1d9f18eaed5dafcdb0e9ac8191461f4dda5"
    "bin/swscale-9.dll|65656f162298df89fda00ddedb5669f4233a6a1e6bd159e093168732be62d91d"
    "lib/avformat.lib|c3fe893975b777e45a42672c65b29b2bc6528571ec1665e6ebbd2c1c92d4d983"
    "lib/avcodec.lib|aae17c97ec976acfab98a5b865273274dffcc3bcc59c7ab401a6b33db04b9ee5"
    "lib/avutil.lib|de461263818475d87a395e5a7330a32d6fd507a4f3f5ee03c067da8a8e368178"
    "lib/swresample.lib|e81abb13e51972ceecb4caf9a7af4d12cffd6e91a2eb82f30cb31dcc00a95a0a"
    "lib/swscale.lib|c720364a08b72cfa6d3ffb3d3360f2c3c82f1a521c87ab5cd45f4cad35c3a043"
    "include/libavformat/avformat.h|a92ad4e9ae2866fb5bb409a30dda8162e253f77e9250fd3057c09f60e3059cbe"
    "include/libavformat/version_major.h|3dde828335345cf56d654fcae728c0fdeb66da23774556d5fed8b09a1fb1a9fc"
    "include/libavcodec/avcodec.h|909f5c2fe7caf26601c39cd96acbc6f58f49ec5a78914d2e5b524a3fc2571afc"
    "include/libavcodec/version_major.h|b70daf8ad1fbf8c6d1658b79a223119b7a788c9637e4fb6446e98ba8114bdd58"
    "include/libavutil/version.h|1de4a0ee20c75f04603ec1799faf9f4e650f790674e2f5ed2fdb1ab735cca75b"
    "include/libswresample/swresample.h|b23e625ab295d57a3d1ae0142c937b485dc386a36e4ed63f4b99ed886dc905e1"
    "include/libswresample/version_major.h|5ded851cffe3a75e556e6914551ee8331c9b6eaa515307c88ceff8c9ab8b2dd1"
    "include/libswscale/swscale.h|8811a21d373f71a00e38fabc7d82dc9c7ccd91b151d30d4e1ac37b9acf63ca66"
    "include/libswscale/version_major.h|00be1f7f4faefb2a98269e901abeccaa28794f347e585bf1ad3cf975d1a3c616"
    "LICENSE.txt|da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768"
)
foreach(KATANA_FFMPEG_PINNED_FILE IN LISTS KATANA_FFMPEG_PINNED_FILES)
    string(REPLACE "|" ";" KATANA_FFMPEG_PINNED_PARTS "${KATANA_FFMPEG_PINNED_FILE}")
    list(GET KATANA_FFMPEG_PINNED_PARTS 0 KATANA_FFMPEG_RELATIVE_FILE)
    list(GET KATANA_FFMPEG_PINNED_PARTS 1 KATANA_FFMPEG_EXPECTED_HASH)
    set(KATANA_FFMPEG_FILE "${KATANA_FFMPEG_ROOT}/${KATANA_FFMPEG_RELATIVE_FILE}")
    if(NOT EXISTS "${KATANA_FFMPEG_FILE}")
        message(FATAL_ERROR "Pinned FFmpeg file is missing: ${KATANA_FFMPEG_RELATIVE_FILE}")
    endif()
    file(SHA256 "${KATANA_FFMPEG_FILE}" KATANA_FFMPEG_ACTUAL_HASH)
    if(NOT KATANA_FFMPEG_ACTUAL_HASH STREQUAL KATANA_FFMPEG_EXPECTED_HASH)
        message(FATAL_ERROR "Pinned FFmpeg file hash mismatch: ${KATANA_FFMPEG_RELATIVE_FILE}")
    endif()
endforeach()

set(KATANA_FFMPEG_MAJOR_HEADERS
    "libavformat/version_major.h|LIBAVFORMAT_VERSION_MAJOR|62"
    "libavcodec/version_major.h|LIBAVCODEC_VERSION_MAJOR|62"
    "libavutil/version.h|LIBAVUTIL_VERSION_MAJOR|60"
    "libswresample/version_major.h|LIBSWRESAMPLE_VERSION_MAJOR|6"
    "libswscale/version_major.h|LIBSWSCALE_VERSION_MAJOR|9"
)
foreach(KATANA_FFMPEG_MAJOR_HEADER IN LISTS KATANA_FFMPEG_MAJOR_HEADERS)
    string(REPLACE "|" ";" KATANA_FFMPEG_MAJOR_PARTS "${KATANA_FFMPEG_MAJOR_HEADER}")
    list(GET KATANA_FFMPEG_MAJOR_PARTS 0 KATANA_FFMPEG_MAJOR_FILE)
    list(GET KATANA_FFMPEG_MAJOR_PARTS 1 KATANA_FFMPEG_MAJOR_DEFINE)
    list(GET KATANA_FFMPEG_MAJOR_PARTS 2 KATANA_FFMPEG_MAJOR_EXPECTED)
    file(STRINGS
        "${KATANA_FFMPEG_ROOT}/include/${KATANA_FFMPEG_MAJOR_FILE}"
        KATANA_FFMPEG_MAJOR_LINE
        REGEX "^#define[ \t]+${KATANA_FFMPEG_MAJOR_DEFINE}[ \t]+${KATANA_FFMPEG_MAJOR_EXPECTED}$"
    )
    if(KATANA_FFMPEG_MAJOR_LINE STREQUAL "")
        message(FATAL_ERROR "Pinned FFmpeg major ABI mismatch: ${KATANA_FFMPEG_MAJOR_DEFINE}")
    endif()
endforeach()

set(KATANA_FFMPEG_INCLUDE_DIR "${KATANA_FFMPEG_ROOT}/include")
set(KATANA_FFMPEG_LIBRARY_DIR "${KATANA_FFMPEG_ROOT}/lib")
set(KATANA_FFMPEG_RUNTIME_FILES
    "${KATANA_FFMPEG_ROOT}/bin/avformat-62.dll"
    "${KATANA_FFMPEG_ROOT}/bin/avcodec-62.dll"
    "${KATANA_FFMPEG_ROOT}/bin/avutil-60.dll"
    "${KATANA_FFMPEG_ROOT}/bin/swresample-6.dll"
    "${KATANA_FFMPEG_ROOT}/bin/swscale-9.dll"
)
set(KATANA_FFMPEG_IMPORT_LIBRARIES
    "${KATANA_FFMPEG_LIBRARY_DIR}/avformat.lib"
    "${KATANA_FFMPEG_LIBRARY_DIR}/avcodec.lib"
    "${KATANA_FFMPEG_LIBRARY_DIR}/avutil.lib"
    "${KATANA_FFMPEG_LIBRARY_DIR}/swresample.lib"
    "${KATANA_FFMPEG_LIBRARY_DIR}/swscale.lib"
)

function(katana_ffmpeg_import component dll_name library_name)
    add_library(KatanaFfmpeg::${component} SHARED IMPORTED GLOBAL)
    set_target_properties(
        KatanaFfmpeg::${component}
        PROPERTIES
            IMPORTED_LOCATION "${KATANA_FFMPEG_ROOT}/bin/${dll_name}"
            IMPORTED_IMPLIB "${KATANA_FFMPEG_LIBRARY_DIR}/${library_name}"
    )
endfunction()
katana_ffmpeg_import(avutil avutil-60.dll avutil.lib)
katana_ffmpeg_import(swresample swresample-6.dll swresample.lib)
katana_ffmpeg_import(swscale swscale-9.dll swscale.lib)
katana_ffmpeg_import(avcodec avcodec-62.dll avcodec.lib)
katana_ffmpeg_import(avformat avformat-62.dll avformat.lib)
set_property(TARGET KatanaFfmpeg::swresample PROPERTY INTERFACE_LINK_LIBRARIES KatanaFfmpeg::avutil)
set_property(TARGET KatanaFfmpeg::swscale PROPERTY INTERFACE_LINK_LIBRARIES KatanaFfmpeg::avutil)
set_property(TARGET KatanaFfmpeg::avcodec PROPERTY INTERFACE_LINK_LIBRARIES "KatanaFfmpeg::avutil;KatanaFfmpeg::swresample")
set_property(TARGET KatanaFfmpeg::avformat PROPERTY INTERFACE_LINK_LIBRARIES "KatanaFfmpeg::avcodec;KatanaFfmpeg::avutil")

add_library(KatanaFfmpegBuild INTERFACE)
target_include_directories(KatanaFfmpegBuild INTERFACE "${KATANA_FFMPEG_INCLUDE_DIR}")
target_link_libraries(
    KatanaFfmpegBuild
    INTERFACE
        "$<BUILD_INTERFACE:KatanaFfmpeg::avformat>"
        "$<BUILD_INTERFACE:KatanaFfmpeg::avcodec>"
        "$<BUILD_INTERFACE:KatanaFfmpeg::avutil>"
        "$<BUILD_INTERFACE:KatanaFfmpeg::swresample>"
        "$<BUILD_INTERFACE:KatanaFfmpeg::swscale>"
)

function(katana_deploy_ffmpeg_runtime target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "katana_deploy_ffmpeg_runtime target does not exist: ${target}")
    endif()
    get_property(_katana_ffmpeg_root
        GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_ROOT)
    get_property(_katana_ffmpeg_notice_file
        GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_NOTICE_FILE)
    get_property(_katana_ffmpeg_build_configuration_file
        GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_BUILD_CONFIGURATION_FILE)
    get_property(_katana_ffmpeg_redistribution_file
        GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_REDISTRIBUTION_FILE)
    get_property(_katana_ffmpeg_redistribution_filename
        GLOBAL PROPERTY KATANA_FFMPEG_DEPLOY_REDISTRIBUTION_FILENAME)
    foreach(
        _katana_ffmpeg_deploy_file
        IN ITEMS
            "${_katana_ffmpeg_root}/LICENSE.txt"
            "${_katana_ffmpeg_notice_file}"
            "${_katana_ffmpeg_build_configuration_file}"
            "${_katana_ffmpeg_redistribution_file}"
    )
        if(_katana_ffmpeg_deploy_file STREQUAL "" OR
           NOT EXISTS "${_katana_ffmpeg_deploy_file}" OR
           IS_DIRECTORY "${_katana_ffmpeg_deploy_file}")
            message(FATAL_ERROR
                "Pinned FFmpeg deployment closure is incomplete: ${_katana_ffmpeg_deploy_file}")
        endif()
    endforeach()
    if(_katana_ffmpeg_redistribution_filename STREQUAL "" OR
       _katana_ffmpeg_redistribution_filename MATCHES "[/\\\\]")
        message(FATAL_ERROR
            "Pinned FFmpeg redistribution filename is invalid")
    endif()
    add_custom_command(
        TARGET "${target}"
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target}>
            $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_katana_ffmpeg_root}/LICENSE.txt"
            "$<TARGET_FILE_DIR:${target}>/FFmpeg-LGPL.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_katana_ffmpeg_notice_file}"
            "$<TARGET_FILE_DIR:${target}>/FFmpeg-NOTICE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_katana_ffmpeg_build_configuration_file}"
            "$<TARGET_FILE_DIR:${target}>/FFmpeg-BUILD-CONFIGURATION.txt"
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:${target}>/FFmpeg-Corresponding-Source.zip"
            "$<TARGET_FILE_DIR:${target}>/FFmpeg-DEVELOPMENT-ONLY.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_katana_ffmpeg_redistribution_file}"
            "$<TARGET_FILE_DIR:${target}>/${_katana_ffmpeg_redistribution_filename}"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endfunction()

add_custom_target(
    katana_ffmpeg_runtime_files
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KATANA_FFMPEG_RUNTIME_FILES} "${CMAKE_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KATANA_FFMPEG_RUNTIME_FILES} "${CMAKE_BINARY_DIR}/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATANA_FFMPEG_ROOT}/LICENSE.txt" "${CMAKE_BINARY_DIR}/FFmpeg-LGPL.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATANA_FFMPEG_ROOT}/LICENSE.txt" "${CMAKE_BINARY_DIR}/$<CONFIG>/FFmpeg-LGPL.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATANA_FFMPEG_NOTICE_FILE}" "${CMAKE_BINARY_DIR}/FFmpeg-NOTICE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATANA_FFMPEG_NOTICE_FILE}" "${CMAKE_BINARY_DIR}/$<CONFIG>/FFmpeg-NOTICE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATANA_FFMPEG_BUILD_CONFIGURATION_FILE}" "${CMAKE_BINARY_DIR}/FFmpeg-BUILD-CONFIGURATION.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATANA_FFMPEG_BUILD_CONFIGURATION_FILE}" "${CMAKE_BINARY_DIR}/$<CONFIG>/FFmpeg-BUILD-CONFIGURATION.txt"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_BINARY_DIR}/FFmpeg-Corresponding-Source.zip" "${CMAKE_BINARY_DIR}/FFmpeg-DEVELOPMENT-ONLY.txt"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_BINARY_DIR}/$<CONFIG>/FFmpeg-Corresponding-Source.zip" "${CMAKE_BINARY_DIR}/$<CONFIG>/FFmpeg-DEVELOPMENT-ONLY.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATANA_FFMPEG_REDISTRIBUTION_FILE}" "${CMAKE_BINARY_DIR}/${KATANA_FFMPEG_REDISTRIBUTION_FILENAME}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATANA_FFMPEG_REDISTRIBUTION_FILE}" "${CMAKE_BINARY_DIR}/$<CONFIG>/${KATANA_FFMPEG_REDISTRIBUTION_FILENAME}"
    COMMENT "Deploy pinned FFmpeg LGPL runtime closure"
    VERBATIM
)
add_dependencies(KatanaFfmpegBuild katana_ffmpeg_runtime_files)
