# Text module hook (R2c). Runs with _mod, _dir and the target ember_${_mod} in scope.
#
# kb_text_shape (segmentation and OpenType shaping) and SheenBidi (UAX #9 bidi) are vendored and
# compiled here as single translation units with warnings off. Their own warnings are not ours.

set(_ember_kb_dir "${CMAKE_SOURCE_DIR}/third_party/kb")
set(_ember_sb_dir "${CMAKE_SOURCE_DIR}/third_party/SheenBidi")

target_include_directories(ember_${_mod} PRIVATE
    "${_ember_kb_dir}"
    "${_ember_sb_dir}/Headers"
    "${_ember_sb_dir}/Source")

set_source_files_properties(
    "${_dir}/kb_impl.c"
    "${_dir}/sheenbidi_impl.c"
    PROPERTIES COMPILE_OPTIONS "-w")

# EMBER_DEFAULT_FONT=OFF (top-level option, SZ3b; OFF by default with EMBER_MIN_SIZE) leaves the bundled default font
# (assets/fonts/ember-default.ttf) out of the library. The text system then starts with no face: it uses the system fonts and
# the fonts the embedder registers.
if(NOT EMBER_DEFAULT_FONT)
    target_compile_definitions(ember_${_mod} PRIVATE EMBER_NO_DEFAULT_FONT=1)
    if(EMBER_TESTS)
        # tests/support/test_default_font.c gives the test executables the face through embi_fontdb_test_default.
        target_compile_definitions(ember_${_mod} PRIVATE EMBI_TEST_DEFAULT_FONT_HOOK=1)
    endif()
endif()
