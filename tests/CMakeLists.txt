# Unit tests. Every tests/unit/<mod>/test_<name>.c becomes the executable test_<mod>_<name>.
# It links ember_<mod> when that module exists, together with the modules named in its
# deps.txt (transitively), and SDL3. An optional tests/unit/<mod>/deps.txt lists extra modules
# (and their dependencies) linked only into that directory's test executables.
# tests/unit/harness has no module, so it links SDL only.

function(_ember_module_closure mod depth outvar)
    if(depth GREATER 32)
        message(FATAL_ERROR "module dependency cycle through '${mod}'")
    endif()
    get_property(_direct GLOBAL PROPERTY ember_deps_${mod})
    set(_result "")
    math(EXPR _next_depth "${depth} + 1")
    foreach(_d IN LISTS _direct)
        list(APPEND _result "${_d}")
        _ember_module_closure(${_d} ${_next_depth} _sub)
        list(APPEND _result ${_sub})
    endforeach()
    list(REMOVE_DUPLICATES _result)
    set(${outvar} "${_result}" PARENT_SCOPE)
endfunction()

file(GLOB _ember_test_mod_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/*")
foreach(_mdir IN LISTS _ember_test_mod_dirs)
    if(NOT IS_DIRECTORY "${_mdir}")
        continue()
    endif()
    get_filename_component(_mod "${_mdir}" NAME)
    # SZ3b: the tests of a module that is not built (EMBER_CANVAS / EMBER_SVG off) do not exist.
    if((_mod STREQUAL "canvas" AND NOT EMBER_CANVAS) OR (_mod STREQUAL "svg" AND NOT EMBER_SVG))
        continue()
    endif()
    file(GLOB _ember_test_srcs CONFIGURE_DEPENDS "${_mdir}/test_*.c")
    if(_mod STREQUAL "net" AND NOT EMBER_NET)
        # EMBER_NET=OFF builds no network module: only the test of the (disabled) public API remains.
        set(_ember_test_srcs "${_mdir}/test_public_api.c")
    endif()

    # SZ3b: tests that need an optional feature are built only with it; the tests of the "off" behaviour (*_off.c, *_nofont.c)
    # only without it.
    if(NOT EMBER_CANVAS)
        list(FILTER _ember_test_srcs EXCLUDE REGEX "/test_(bind_canvas|embed_canvas)[.]c$")
    else()
        list(FILTER _ember_test_srcs EXCLUDE REGEX "/test_bind_canvas_off[.]c$")
    endif()
    if(NOT EMBER_SVG)
        list(FILTER _ember_test_srcs EXCLUDE REGEX "/test_(bind_svg|svg_input|svg_render)[.]c$")
    else()
        list(FILTER _ember_test_srcs EXCLUDE REGEX "/test_bind_svg_off[.]c$")
    endif()
    if(EMBER_DEFAULT_FONT)
        list(FILTER _ember_test_srcs EXCLUDE REGEX "/test_(text|embed)_nofont[.]c$")
    endif()

    if(NOT EMBER_COMPRESS_ASSETS)
        # These read the compressed assets (gen/ember_lz), which exist only in a build that compresses them.
        list(FILTER _ember_test_srcs EXCLUDE REGEX "/test_(lz_assets|blob_lz)[.]c$")
    endif()

    # Test-only dependencies: tests/unit/<mod>/deps.txt, whitespace-separated module names.
    set(_test_extra "")
    if(EXISTS "${_mdir}/deps.txt")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mdir}/deps.txt")
        file(READ "${_mdir}/deps.txt" _tdeps_raw)
        string(REGEX REPLACE "[ 	
]+" ";" _tdeps "${_tdeps_raw}")
        foreach(_td IN LISTS _tdeps)
            if(_td STREQUAL "")
                continue()
            endif()
            list(APPEND _test_extra "${_td}")
            _ember_module_closure(${_td} 0 _tclosure)
            list(APPEND _test_extra ${_tclosure})
        endforeach()
        list(REMOVE_DUPLICATES _test_extra)
    endif()

    foreach(_src IN LISTS _ember_test_srcs)
        get_filename_component(_base "${_src}" NAME_WE)
        # test_<mod>_<name>: a file already named test_<mod>_<name>.c keeps that name.
        if(_base MATCHES "^test_${_mod}_")
            set(_exe "${_base}")
        else()
            string(REGEX REPLACE "^test_" "" _tname "${_base}")
            set(_exe "test_${_mod}_${_tname}")
        endif()

        add_executable(${_exe} "${_src}")
        target_include_directories(${_exe} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_SOURCE_DIR}/include"
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_BINARY_DIR}/gen")
        ember_set_warnings(${_exe})
        add_dependencies(${_exe} ember_shaders)
        if(EMBER_COMPRESS_ASSETS)
            add_dependencies(${_exe} ember_lz_assets)
        endif()
        # Tests find fixtures (tests/fonts, tests/data, third_party/*-tests) under this root.
        target_compile_definitions(${_exe} PRIVATE EMBER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        if(EMBER_NET)
            target_compile_definitions(${_exe} PRIVATE EMBER_NET=1)
        endif()

        set(_link_libs "")
        if(TARGET ember_${_mod})
            _ember_module_closure(${_mod} 0 _closure)
            list(APPEND _link_libs "ember_${_mod}")
            foreach(_d IN LISTS _closure)
                list(APPEND _link_libs "ember_${_d}")
            endforeach()
            list(REMOVE_DUPLICATES _link_libs)
        endif()
        foreach(_d IN LISTS _test_extra)
            if(TARGET ember_${_d})
                list(APPEND _link_libs "ember_${_d}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _link_libs)
        # Without the bundled font the tests that link the text module get the face from the source tree
        # (tests/support/test_default_font.c), except the ones that test its absence.
        if(NOT EMBER_DEFAULT_FONT AND "ember_text" IN_LIST _link_libs AND NOT _base MATCHES "nofont|defaultfont|text_fontdb|text_match")
            target_sources(${_exe} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support/test_default_font.c")
        endif()
        target_link_libraries(${_exe} PRIVATE ${_link_libs} SDL3::SDL3-static)

        add_test(NAME ${_exe} COMMAND ${_exe})
        set_tests_properties(${_exe} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
endforeach()
