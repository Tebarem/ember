# idlgen: generates the binding glue (src/bindings/gen/) from the WebIDL files in src/bindings/idl/
# (W5a). Not part of the normal build; the outputs are checked in.
#
#     cmake --build build/<dir> --target idlgen
#
# The generated files are deterministic, so a regeneration without IDL changes leaves no diff.

add_executable(idlgen_tool EXCLUDE_FROM_ALL idlgen.c idl_parse.c idl_gen.c)
ember_set_warnings(idlgen_tool)

file(GLOB _idlgen_idl CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/bindings/idl/*.webidl")
add_custom_target(idlgen
    COMMAND idlgen_tool --props "${CMAKE_SOURCE_DIR}/src/css/props.txt" "${CMAKE_SOURCE_DIR}/src/bindings/gen" ${_idlgen_idl}
    DEPENDS idlgen_tool ${_idlgen_idl} "${CMAKE_SOURCE_DIR}/src/css/props.txt"
    COMMENT "Regenerating src/bindings/gen from src/bindings/idl"
    VERBATIM)
