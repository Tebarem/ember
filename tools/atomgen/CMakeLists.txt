# atomgen regenerates src/dom/atoms.inc from tools/atomgen/atoms.txt:
#     cmake --build build/<dir> --target atomgen
# It also writes src/base/static_atoms.inc (the name list base interns first).
# The generated header is checked in, so a normal build never runs the generator.

add_executable(atomgen_tool EXCLUDE_FROM_ALL atomgen.c)
ember_set_warnings(atomgen_tool)

add_custom_target(atomgen
    COMMAND atomgen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/atoms.txt"
            "${CMAKE_SOURCE_DIR}/src/dom/atoms.inc"
            "${CMAKE_SOURCE_DIR}/src/base/static_atoms.inc"
    DEPENDS atomgen_tool "${CMAKE_CURRENT_SOURCE_DIR}/atoms.txt"
    COMMENT "Regenerating src/dom/atoms.inc and src/base/static_atoms.inc from tools/atomgen/atoms.txt"
    VERBATIM)
