# Benches: every tools/bench/bench_<name>.c becomes an executable bench_<name>. They are part of
# the default build so they always compile, but ctest does not run them; run them by hand from
# build/<dir>/tools/bench (use a Release build for numbers). They use internal embi_* symbols,
# which a shared library does not export, so they are built only with the tests and not for a
# shared build.
if(EMBER_TESTS AND NOT EMBER_SHARED)
    file(GLOB _ember_benches CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bench_*.c")
    foreach(_src IN LISTS _ember_benches)
        get_filename_component(_name "${_src}" NAME_WE)
        add_executable(${_name} "${_src}")
        target_include_directories(${_name} PRIVATE
            "${CMAKE_SOURCE_DIR}/include"
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/tests"
            "${CMAKE_BINARY_DIR}/gen")
        ember_set_warnings(${_name})
        ember_set_sanitizers(${_name})
        add_dependencies(${_name} ember_shaders)
        target_link_libraries(${_name} PRIVATE ember)
    endforeach()
endif()
