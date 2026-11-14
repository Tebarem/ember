# libFuzzer harnesses (EMBER_FUZZ=ON, clang only). Included from the root CMakeLists.txt, which has
# already put the sanitizer and fuzzer-no-link flags on every Ember module.
#
# Every tests/fuzz/fuzz_<name>.c becomes the executable fuzz_<name>. A harness names the modules it
# needs in a first-lines comment of the form
#
#     // fuzz-modules: html dom loader
#
# and links those modules plus their deps.txt closure. Seed corpora live in tests/fuzz/corpus/<name>/
# and dictionaries in tests/fuzz/dict/<name>.dict (see README.md and tools/fuzz/run.sh).

function(_ember_fuzz_closure mod depth outvar)
    if(depth GREATER 32)
        message(FATAL_ERROR "module dependency cycle through '${mod}'")
    endif()
    get_property(_direct GLOBAL PROPERTY ember_deps_${mod})
    set(_result "")
    math(EXPR _next_depth "${depth} + 1")
    foreach(_d IN LISTS _direct)
        list(APPEND _result "${_d}")
        _ember_fuzz_closure(${_d} ${_next_depth} _sub)
        list(APPEND _result ${_sub})
    endforeach()
    list(REMOVE_DUPLICATES _result)
    set(${outvar} "${_result}" PARENT_SCOPE)
endfunction()

file(GLOB _ember_fuzz_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_*.c")
set(EMBER_FUZZ_TARGETS "")
foreach(_src IN LISTS _ember_fuzz_srcs)
    get_filename_component(_exe "${_src}" NAME_WE)
    file(STRINGS "${_src}" _mods_line REGEX "^// fuzz-modules:" LIMIT_COUNT 1)
    if(NOT _mods_line)
        message(FATAL_ERROR "${_src}: missing '// fuzz-modules: ...' line")
    endif()
    string(REGEX REPLACE "^// fuzz-modules:[ \t]*" "" _mods "${_mods_line}")
    string(REGEX REPLACE "[ \t\r\n]+" ";" _mods "${_mods}")
    list(REMOVE_ITEM _mods "")

    add_executable(${_exe} "${_src}")
    target_include_directories(${_exe} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/gen")
    target_compile_definitions(${_exe} PRIVATE EMBER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    ember_set_warnings(${_exe})
    add_dependencies(${_exe} ember_shaders)

    set(_link_libs "")
    foreach(_m IN LISTS _mods)
        if(NOT TARGET ember_${_m})
            message(FATAL_ERROR "${_src}: unknown module '${_m}'")
        endif()
        list(APPEND _link_libs "ember_${_m}")
        _ember_fuzz_closure(${_m} 0 _closure)
        foreach(_d IN LISTS _closure)
            list(APPEND _link_libs "ember_${_d}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _link_libs)
    target_link_libraries(${_exe} PRIVATE ${_link_libs} SDL3::SDL3-static)

    # fuzzer-no-link comes from ember_set_sanitizers (EMBER_FUZZ); the driver and main() come from
    # -fsanitize=fuzzer at link time.
    ember_set_sanitizers(${_exe})
    target_link_options(${_exe} PRIVATE -fsanitize=fuzzer)
    list(APPEND EMBER_FUZZ_TARGETS ${_exe})
endforeach()

add_custom_target(fuzz_all DEPENDS ${EMBER_FUZZ_TARGETS})
