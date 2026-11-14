# Extra interpreter variants for tests (J5b).
#
# interp.c uses computed goto under GCC and Clang and a switch elsewhere (MSVC ABI targets,
# clang-cl). Variant 1 builds interp.c with EMBI_JS_NO_COMPUTED_GOTO and runs the numeric fast-path
# and error-location tests on it, so the switch fallback is covered, including its pc_cur store.
# Variant 2 builds interp.c with EMBI_JS_OPSTATS and runs the op-statistics test.
# Every other src/js source is compiled once into ember_js_variant_base and shared by the variant
# executables; only interp.c is built extra. The module's own ember_js target is unchanged.
#
# Runs with _mod, _dir and the target ember_${_mod} in scope.

# CF2: GCC's SLP vectorizer packs the interpreter's dispatch state into vector registers at the one block every handler
# jumps to, which makes that block too large for GCC to copy into the handlers (pass duplicate_computed_gotos): run() was
# left with a single shared `jmp *` for all 250 ops, so the indirect branch predictor saw one branch. Without SLP each
# handler ends in its own indirect jump (and the object is 3% smaller). Clang does not merge them. The flag is an
# optimization option, so under LTO it stays with the functions of this file.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set_property(SOURCE "${_dir}/interp.c" APPEND PROPERTY COMPILE_OPTIONS -fno-tree-slp-vectorize)
endif()

if(EMBER_TESTS)
    file(GLOB _var_srcs CONFIGURE_DEPENDS "${_dir}/*.c")
    list(FILTER _var_srcs EXCLUDE REGEX "/interp\.c$")

    set(_var_inc "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/gen")

    add_library(ember_js_variant_base OBJECT ${_var_srcs})
    target_include_directories(ember_js_variant_base PRIVATE ${_var_inc})
    target_link_libraries(ember_js_variant_base PRIVATE SDL3::SDL3-static)
    ember_set_warnings(ember_js_variant_base)
    ember_set_sanitizers(ember_js_variant_base)

    # name, interp define, test sources...
    function(ember_js_interp_variant _vname _define)
        add_library(ember_js_interp_${_vname} OBJECT "${_dir}/interp.c")
        target_include_directories(ember_js_interp_${_vname} PRIVATE ${_var_inc})
        target_compile_definitions(ember_js_interp_${_vname} PRIVATE ${_define})
        target_link_libraries(ember_js_interp_${_vname} PRIVATE SDL3::SDL3-static)
        ember_set_warnings(ember_js_interp_${_vname})
        ember_set_sanitizers(ember_js_interp_${_vname})
        foreach(_t IN LISTS ARGN)
            get_filename_component(_tn "${_t}" NAME_WE)
            string(REGEX REPLACE "^test_js_" "" _short "${_tn}")
            set(_exe test_js_${_vname}_${_short})
            add_executable(${_exe} "${CMAKE_SOURCE_DIR}/tests/unit/js/${_t}"
                $<TARGET_OBJECTS:ember_js_variant_base>
                $<TARGET_OBJECTS:ember_js_interp_${_vname}>)
            target_include_directories(${_exe} PRIVATE
                "${CMAKE_SOURCE_DIR}/tests/unit/js" "${CMAKE_SOURCE_DIR}/tests" ${_var_inc})
            target_compile_definitions(${_exe} PRIVATE
                EMBER_SOURCE_DIR="${CMAKE_SOURCE_DIR}" ${_define})
            ember_set_warnings(${_exe})
            ember_set_sanitizers(${_exe})
            target_link_libraries(${_exe} PRIVATE ember_base ember_num ember_jslex SDL3::SDL3-static)
            add_test(NAME ${_exe} COMMAND ${_exe})
            set_tests_properties(${_exe} PROPERTIES SKIP_RETURN_CODE 77)
        endforeach()
    endfunction()

    ember_js_interp_variant(nocg EMBI_JS_NO_COMPUTED_GOTO
        test_js_numeric_fast.c test_js_numeric_fast_loc.c test_js_fused.c)
    ember_js_interp_variant(opstats EMBI_JS_OPSTATS test_js_opstats.c)
endif()
