# test262 runner. The runs are:
#   cmake --build build/<dir> --target test262
#   build/<dir>/tools/test262/test262 [--smoke | --list FILE | --filter PATH] [--update]
# ctest runs the fixed smoke sample (tests/test262/smoke-list.txt) and reports skipped (77) when
# the third_party/test262 checkout is missing.

add_executable(test262 main.c frontmatter.c runner.c)
ember_set_warnings(test262)
ember_set_sanitizers(test262)
add_dependencies(test262 ember_shaders)
target_include_directories(test262 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/gen")
target_compile_definitions(test262 PRIVATE EMBER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# ember_js and the modules it depends on (src/js/deps.txt), listed in dependency order.
target_link_libraries(test262 PRIVATE ember_js ember_num ember_jslex ember_base SDL3::SDL3-static)

add_test(NAME test262_smoke COMMAND test262 --smoke)
set_tests_properties(test262_smoke PROPERTIES SKIP_RETURN_CODE 77)
