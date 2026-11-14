# W5a: the generated binding glue (src/bindings/gen/, written by tools/idlgen) is part of this
# module. The module's own sources are the top-level *.c files (bind.c, impl_*.c); the generated
# files are added here because the module glob does not descend into gen/.
#
# Runs with _mod, _dir and the target ember_${_mod} in scope.

file(GLOB _bind_gen_srcs CONFIGURE_DEPENDS "${_dir}/gen/*.c")
if(NOT _bind_gen_srcs)
    message(FATAL_ERROR "src/bindings/gen is empty: run `cmake --build <dir> --target idlgen`")
endif()
if(NOT EXISTS "${_dir}/gen/types.h")
    message(FATAL_ERROR "src/bindings/gen/types.h is missing: run `cmake --build <dir> --target idlgen`")
endif()
target_sources(ember_${_mod} PRIVATE ${_bind_gen_srcs})

# The [Testing] interfaces (testing.webidl, their implementation impl_testing.c) exist for the unit
# tests only: a build without EMBER_TESTS leaves them out of the library (the generated tables
# hold nullptr for them, see EMBI_BIND_NO_TESTING).
if(NOT EMBER_TESTS)
    get_target_property(_bind_srcs ember_${_mod} SOURCES)
    list(FILTER _bind_srcs EXCLUDE REGEX "(impl_testing|gen/testing)\.c$")
    set_property(TARGET ember_${_mod} PROPERTY SOURCES "${_bind_srcs}")
    target_compile_definitions(ember_${_mod} PRIVATE EMBI_BIND_NO_TESTING)
endif()

# SZ3b: the canvas and SVG bindings are built only with their feature. A disabled feature has no generated interface table
# (the tables in gen/ifaces.c hold nullptr for it, see [Feature=...] in tools/idlgen) and no implementation. With EMBER_CANVAS
# off, impl_canvas_off.c keeps the HTMLCanvasElement (getContext() returns null). impl_svg_val.c (with impl_svg_list.c, which it calls) holds the geometry
# interfaces (DOMPoint, DOMMatrix, DOMRect) and is always built; the code only the SVG interfaces reach is dropped by the linker.
get_target_property(_bind_srcs ember_${_mod} SOURCES)
if(EMBER_CANVAS)
    list(FILTER _bind_srcs EXCLUDE REGEX "impl_canvas_off[.]c$")
else()
    list(FILTER _bind_srcs EXCLUDE REGEX "(impl_canvas|impl_canvas_path|impl_canvas_data|gen/canvas)[.]c$")
endif()
if(NOT EMBER_SVG)
    list(FILTER _bind_srcs EXCLUDE REGEX "(impl_svg|gen/svg)[.]c$")
endif()
set_property(TARGET ember_${_mod} PROPERTY SOURCES "${_bind_srcs}")
