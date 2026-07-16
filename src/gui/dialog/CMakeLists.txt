get_directory_property(katana_dialog_compile_options COMPILE_OPTIONS)
list(REMOVE_ITEM katana_dialog_compile_options /fsanitize=address /Oy-)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${katana_dialog_compile_options}")
get_directory_property(katana_dialog_link_options LINK_OPTIONS)
list(REMOVE_ITEM katana_dialog_link_options /INCREMENTAL:NO)
set_directory_properties(PROPERTIES LINK_OPTIONS "${katana_dialog_link_options}")

add_executable(katana-file-dialog file_dialog_win32.cpp)
target_link_libraries(katana-file-dialog PRIVATE comdlg32 ole32 shell32 user32)
target_compile_features(katana-file-dialog PRIVATE cxx_std_20)
set_target_properties(
    katana-file-dialog
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
