# cssgen regenerates src/css/props.inc from src/css/props.txt:
#     cmake --build build/<dir> --target cssgen
# The generated header is checked in, so a normal build never runs the generator.

add_executable(cssgen_tool EXCLUDE_FROM_ALL cssgen.c)
ember_set_warnings(cssgen_tool)

add_custom_target(cssgen
    COMMAND cssgen_tool
            "${CMAKE_SOURCE_DIR}/src/css/props.txt"
            "${CMAKE_SOURCE_DIR}/src/css/props.inc"
    DEPENDS cssgen_tool "${CMAKE_SOURCE_DIR}/src/css/props.txt"
    COMMENT "Regenerating src/css/props.inc from src/css/props.txt"
    VERBATIM)
