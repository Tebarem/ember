# entitiesgen regenerates src/html/entities.inc from the WHATWG named character reference table in
# tools/entitiesgen/data/entities.json:
#     cmake --build build/<dir> --target entitiesgen
# The generated table is checked in, so a normal build never runs the generator.

add_executable(entitiesgen_tool EXCLUDE_FROM_ALL entitiesgen.c)
target_include_directories(entitiesgen_tool PRIVATE "${CMAKE_SOURCE_DIR}/tests")
ember_set_warnings(entitiesgen_tool)

add_custom_target(entitiesgen
    COMMAND entitiesgen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/data/entities.json"
            "${CMAKE_SOURCE_DIR}/src/html/entities.inc"
    DEPENDS entitiesgen_tool
    COMMENT "Regenerating src/html/entities.inc from entities.json"
    VERBATIM)
