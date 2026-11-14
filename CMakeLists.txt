cmake_minimum_required(VERSION 3.28)
project(ember C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- options
option(EMBER_SHARED "Build libember as a shared library" OFF)
option(EMBER_TESTS "Build the unit tests" ON)
option(EMBER_EXAMPLES "Build the examples" ON)
option(EMBER_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(EMBER_FUZZ "Build the libFuzzer harnesses in tests/fuzz (clang only; implies the sanitizers)" OFF)
if(EMBER_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "EMBER_FUZZ needs clang (libFuzzer); the compiler is ${CMAKE_C_COMPILER_ID}")
    endif()
    set(EMBER_SANITIZE ON)
endif()
option(EMBER_NET "Build the network stack (HTTP/HTTPS over SDL_net and Mbed TLS, the Mozilla roots). OFF builds none of it and the emb_runtime_net_* functions return false" ON)
option(EMBER_MIN_SIZE "Optimize the library and SDL for binary size: -Oz (EMBER_SIZE_OPT), unused-section removal, LTO, and the SDL parts Ember does not use left out (Windows: Direct3D 12 only)" OFF)
# -Oz (clang, and gcc 12 or newer) is -Os with the inliner and loop transforms held back further: 9% smaller than -Os on
# the Ember-only library (clang 19, tools/libsize.sh), and slower still. MSVC has /O1 only.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR (CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 12))
    set(_ember_size_opt_default "-Oz")
else()
    set(_ember_size_opt_default "-Os")
endif()
set(EMBER_SIZE_OPT "${_ember_size_opt_default}" CACHE STRING "With EMBER_MIN_SIZE: the optimization flag for size (-Oz or -Os)")
option(EMBER_LTO "With EMBER_MIN_SIZE: link with LTO. OFF is for tools/libsize.sh, which needs a linker map that names the module of every byte" ON)
# The JS front end (lexer, parser, scope analysis, bytecode emitter) compiles the 223 KB React bundle in the first paint.
# At -Oz it takes 10.0 ms against 8.1 ms for the default build, so these sources keep EMBER_SPEED_OPT: first paint
# 19.5 -> 17.3 ms for about 40 KB (gcc 15, Windows). Source paths are relative to src/. Empty: everything at EMBER_SIZE_OPT.
# MSVC is left out (/O1 only).
# Clang 19 with LTO is different (box, tools/libsize.sh Ember only, bench_js_compile on the React bundle): the four sources
# without parser.c cost 33 KB for 10.6 -> 10.0 ms, while adding parser.c costs 135 KB in all (parser.c alone 299 KB: the
# LTO inliner pulls the -Oz code it calls into it) for 9.5 ms. So clang keeps parser.c at -Oz; gcc gets all five.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(_ember_speed_default "js/scope.c;js/compiler.c;jslex/jslex.c;js/js_atom.c")
else()
    set(_ember_speed_default "js/parser.c;js/scope.c;js/compiler.c;jslex/jslex.c;js/js_atom.c")
endif()
set(EMBER_SPEED_SOURCES "${_ember_speed_default}" CACHE STRING "With EMBER_MIN_SIZE: sources compiled with EMBER_SPEED_OPT instead of EMBER_SIZE_OPT")
set(EMBER_SPEED_OPT "-O2" CACHE STRING "With EMBER_MIN_SIZE: the optimization flag of EMBER_SPEED_SOURCES")
if(WIN32 AND EMBER_MIN_SIZE)
    set(_ember_default_formats "dxil") # Vulkan is left out of SDL below, so SPIR-V would never be used
elseif(WIN32)
    set(_ember_default_formats "spirv;dxil")
elseif(APPLE)
    set(_ember_default_formats "msl")
else()
    set(_ember_default_formats "spirv")
endif()
set(EMBER_SHADER_FORMATS "${_ember_default_formats}" CACHE STRING
    "Shader formats to build: any of spirv;dxil;msl")

# RELR needs glibc 2.36 or newer (musl 1.2.4) at run time, so the default is on only for MIN_SIZE on Linux; Android (its own
# loader, its own minimum API level) and the BSDs can ask for it by hand.
if(EMBER_MIN_SIZE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ember_relr_default ON)
else()
    set(_ember_relr_default OFF)
endif()
option(EMBER_RELR "Shared library on ELF: pack the relative relocations (DT_RELR, -z pack-relative-relocs). Needs glibc 2.36 or newer (musl 1.2.4) at run time. Default: on with EMBER_MIN_SIZE on Linux" ${_ember_relr_default})
option(EMBER_UNWIND_TABLES "With EMBER_MIN_SIZE: keep the unwind tables (.eh_frame, 0.79 MB). Off drops them from Release builds on ELF: host crash reporters, profilers and debuggers then lose the frames inside Ember, and a C++ exception thrown from a host callback terminates the process instead of propagating through Ember" OFF)
option(EMBER_SDL_EXTERNAL "Shared library only: do not link SDL3 and SDL_image into libember; the host supplies them at load time (the size of this library is Ember's own size, see tools/libsize.sh)" OFF)

# LLVM's machine outliner (shared sequences of machine code turned into calls) is off for x86 even at -Oz. On with clang
# and EMBER_MIN_SIZE it takes 33 KB (0.9%) off the Ember-only library. It runs in the LTO link (the compile flag is for builds
# without LTO). Costs a call and a return per outlined sequence, which is why it is not on in the default build.
# CMake builds clang's LTO as ThinLTO. Full LTO (one module, one inlining and merging pass over everything) makes libember
# 45 KB (1.2%) smaller, but on the reference machine it slows the JS compile of the React bundle from 7.0 to 8.1 ms (+16%, bench_js_compile,
# clang 19), which is first-paint time, so it is off by default. It also links single-threaded (about a minute), and with the
# tests every test executable would be linked that way. Needs clang.
option(EMBER_LTO_FULL "With EMBER_MIN_SIZE and clang LTO: full LTO instead of ThinLTO (smaller, slower JS compile)" OFF)
option(EMBER_OUTLINE "With EMBER_MIN_SIZE and clang: enable the machine outliner (-mllvm -enable-machine-outliner=always)" ${EMBER_MIN_SIZE})
option(EMBER_COMPRESS_ASSETS "Store the shader blobs, the prelude scripts, the UA style sheet, the Brotli dictionary and the RegExp property data LZ-compressed and expand them where they are used (tools/lz, src/base/lz.c), and with EMBER_NET the root certificates as DER instead of PEM. In the Ember-only library (clang 19, MIN_SIZE, NET off) the shaders, scripts and style sheet save 184 KB; the dictionary, SPIR-V at -O0 and the property data about 110 KB more; the DER roots 73 KB with NET. Default: on with EMBER_MIN_SIZE" ${EMBER_MIN_SIZE})
# slangc optimisation level of the SPIR-V blobs. -O2 inlines and unrolls: the box fragment shader is 125 KB at -O2 and
# 65 KB at -O0 (the driver optimises it again anyway). Default: -O0 with EMBER_MIN_SIZE, -O2 otherwise.
if(EMBER_MIN_SIZE)
    set(_ember_spirv_opt_default -O0)
else()
    set(_ember_spirv_opt_default -O2)
endif()
set(EMBER_SPIRV_OPT "${_ember_spirv_opt_default}" CACHE STRING "slangc optimisation level for the SPIR-V blobs (-O0, -O1, -O2)")

# optional features. Each is ON by default; with EMBER_MIN_SIZE the default is OFF (the size build leaves them out), and an
# explicit -DEMBER_X=ON keeps the feature in a size build.
if(EMBER_MIN_SIZE)
    set(_ember_feat_default OFF)
else()
    set(_ember_feat_default ON)
endif()
option(EMBER_CANVAS "The canvas 2D context (src/canvas and the CanvasRenderingContext2D, Path2D, ImageData, ... bindings, about 130 KB). OFF: canvas.getContext() returns null, <canvas> stays an empty replaced box (300x150 by default). Default: on, off with EMBER_MIN_SIZE" ${_ember_feat_default})
option(EMBER_SVG "SVG rendering (src/svg, the SVG DOM bindings, SVG images in img and CSS url()). OFF: <svg> and the other SVG elements get no box (nothing renders), an SVG image does not decode. Default: on, off with EMBER_MIN_SIZE" ${_ember_feat_default})
option(EMBER_DEFAULT_FONT "Bundle the default font (assets/fonts/ember-default.ttf, 216 KB) as the last resort of the font fallback. OFF: text uses the system fonts and the fonts the embedder registers (emb_runtime_add_font); with neither, nothing is drawn and the runtime logs a warning once. Default: on, off with EMBER_MIN_SIZE" ${_ember_feat_default})
option(EMBER_NO_DEFAULT_FONT "Deprecated: same as EMBER_DEFAULT_FONT=OFF" OFF)
if(EMBER_NO_DEFAULT_FONT)
    set(EMBER_DEFAULT_FONT OFF)
endif()

set(EMBER_SLANGC "" CACHE FILEPATH "Path to the slangc compiler")
set(EMBER_DXC_DIR "" CACHE PATH
    "Directory holding dxcompiler.dll and dxil.dll (auto-detected from the Windows SDK when empty)")
set(EMBER_SDL_DIR "${CMAKE_SOURCE_DIR}/third_party/SDL" CACHE PATH
    "SDL3 source directory (default: the vendored submodule; point it at another checkout for worktrees)")

# ---------------------------------------------------------------- helpers
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(EmberWarnings)
include(EmberSanitize)
include(EmberShaders)
include(EmberCompress)
if(EMBER_COMPRESS_ASSETS)
    # The compressor runs at build time (the host is the target here; cross builds would need a host build of it).
    add_executable(ember_lz tools/lz/lz.c)
    ember_set_warnings(ember_lz)
    add_executable(ember_incbin tools/lz/incbin.c)
    ember_set_warnings(ember_incbin)
    if(EMBER_NET)
        add_executable(ember_pem2der tools/lz/pem2der.c)
        ember_set_warnings(ember_pem2der)
    endif()
endif()

# ---------------------------------------------------------------- SDL3 (static, vendored)
# SDL is configured without any Ember flags: sanitizers and warnings are per Ember target only.
set(SDL_SHARED OFF CACHE BOOL "Build SDL3 shared (Ember uses static)" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL3 static" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)

# ---------------------------------------------------------------- EMBER_MIN_SIZE
# For shipping: examples/vue-app goes from 10.9 to 6.0 MB on Windows (stripped, MinGW GCC 16; 10.1 to 5.5 MB with
# EMBER_NET=OFF, measured 2026-11-12). The cost, measured on
# 2026-11-12: style, layout and display list builds 15-45% slower, TodoMVC first paint 33 -> 38 ms and its click frame
# 23 -> 30 ms; simple JS loops get faster. Keeping the per-frame modules at -O2 bought little (6.8 MB, TodoMVC no faster).
# The flags apply to every target from here on, SDL included, so this block must stay before add_subdirectory(SDL).
if(EMBER_MIN_SIZE)
    if(MSVC)
        add_compile_options(/O1 /Gy /Gw)
        add_link_options(/OPT:REF /OPT:ICF)
    else()
        # The size flag comes after the build type's -O2/-O3 on the command line, so it wins.
        add_compile_options(${EMBER_SIZE_OPT} -ffunction-sections -fdata-sections)
        if(APPLE)
            add_link_options(-Wl,-dead_strip)
        else()
            add_link_options(-Wl,--gc-sections)
        endif()
    endif()
    # No unwind tables on ELF Release builds (measured on Linux only; Mach-O is not measured and keeps them). Ember is C
    # and its own code never unwinds (no C++ exceptions, no pthread_cancel). The cost, which a host must know: a crash
    # reporter, a sampling profiler or a debugger that walks the stack with .eh_frame loses the frames at Ember (frame
    # pointers still work with -fno-omit-frame-pointer), and a C++ exception thrown from a host callback that Ember called
    # terminates the process instead of propagating through Ember. Debug, RelWithDebInfo and sanitizer builds keep the
    # tables; Win64 needs them for SEH (.pdata). EMBER_UNWIND_TABLES=ON keeps them everywhere.
    if(NOT WIN32 AND NOT MSVC AND NOT APPLE AND NOT EMBER_SANITIZE AND NOT EMBER_UNWIND_TABLES)
        add_compile_options("$<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>>:-fno-asynchronous-unwind-tables;-fno-unwind-tables>")
    endif()
    # Not in Debug, RelWithDebInfo or sanitizer builds: outlined code makes backtraces and profiles harder to read.
    if(EMBER_OUTLINE AND CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT MSVC AND NOT EMBER_SANITIZE
       AND NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
        set(_ember_outline_on ON)
        add_compile_options("-mllvm" "-enable-machine-outliner=always")
    endif()
    include(CheckIPOSupported)
    if(EMBER_LTO)
        check_ipo_supported(RESULT _ember_ipo OUTPUT _ember_ipo_msg LANGUAGES C)
    else()
        set(_ember_ipo OFF)
        set(_ember_ipo_msg "EMBER_LTO=OFF")
    endif()
    if(_ember_ipo)
        if(EMBER_LTO_FULL AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
            set(CMAKE_C_COMPILE_OPTIONS_IPO "-flto=full")
        endif()
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    elseif(EMBER_LTO)
        message(WARNING "EMBER_MIN_SIZE: no LTO with this toolchain (${_ember_ipo_msg})")
    endif()
    # SDL subsystems Ember never calls. Joysticks stay in without LTO: SDL_joystick.c is then still linked and refers to
    # a function the disabled subsystem does not build (an undefined SDL_OnUpdateJoysticksChanged).
    foreach(_opt SDL_SENSOR SDL_POWER SDL_GPU_OPENXR SDL_TRAY SDL_DIALOG)
        set(${_opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    if(_ember_ipo)
        set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
        set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
    endif()
    if(WIN32)
        # Ember draws through SDL_gpu's Direct3D 12 backend on Windows; OpenGL and Vulkan are not used.
        foreach(_opt SDL_OPENGL SDL_OPENGLES SDL_VULKAN)
            set(${_opt} OFF CACHE BOOL "" FORCE)
        endforeach()
    endif()
endif()

if(NOT EXISTS "${EMBER_SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SDL3 not found at '${EMBER_SDL_DIR}' (run: git submodule update --init third_party/SDL, or set EMBER_SDL_DIR)")
endif()
# Explicit binary dir: EMBER_SDL_DIR may live outside the source tree.
add_subdirectory("${EMBER_SDL_DIR}" "${CMAKE_BINARY_DIR}/third_party/SDL" EXCLUDE_FROM_ALL)
if(EMBER_SHARED)
    set_target_properties(SDL3-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
# The trick below depends on one line of SDL's header; an SDL update that drops it would turn the dynamic API back on without
# any message (160 KB and a relocation per function), so the configure step checks the line.
file(STRINGS "${EMBER_SDL_DIR}/src/dynapi/SDL_dynapi.h" _ember_dynapi_off REGEX "^#elif.*defined\\(__RESHARPER__\\)")
if(NOT _ember_dynapi_off)
    message(FATAL_ERROR "${EMBER_SDL_DIR}/src/dynapi/SDL_dynapi.h no longer switches the dynamic API off for __RESHARPER__. "
        "Ember relies on that (target_compile_definitions(SDL3-static ... __RESHARPER__=1) in CMakeLists.txt) to keep SDL_dynapi.c, "
        "160 KB of jump table, out of the library. Find the new way to set SDL_DYNAMIC_API to 0 without editing SDL (another "
        "name in that #elif, or a build option) and update CMakeLists.txt and the check above it.")
endif()
# SDL's dynamic API (SDL_dynapi.c: a table of every SDL function, so that a program can swap SDL3 at run time through an
# environment variable) is useless in a static link: it costs 160 KB, a relocation per entry and an indirect jump per call.
# SDL_dynapi.h refuses SDL_DYNAMIC_API=0 on the command line ("edit this file"); it does switch the API off for static
# analysers, and __RESHARPER__ is a name that nothing else in SDL looks at. SDL's sources stay untouched.
target_compile_definitions(SDL3-static PRIVATE __RESHARPER__=1)

# the feature switches are seen by every source of the library, the tests and the tools.
if(EMBER_CANVAS)
    add_compile_definitions(EMBER_CANVAS=1)
else()
    add_compile_definitions(EMBER_CANVAS=0)
endif()
if(EMBER_SVG)
    add_compile_definitions(EMBER_SVG=1)
else()
    add_compile_definitions(EMBER_SVG=0)
endif()

# ---------------------------------------------------------------- shaders
ember_add_shaders()

# The other compressed assets: the prelude scripts of src/bindings and the UA style sheet (EMBER_LZ_ASSETS in their sources).
if(EMBER_COMPRESS_ASSETS)
    file(GLOB _ember_js_preludes CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/bindings/js/*.js")
    foreach(_f IN LISTS _ember_js_preludes)
        get_filename_component(_n "${_f}" NAME)
        ember_lz_pack(_unused "${_f}" "js/${_n}")
    endforeach()
    ember_lz_pack(_unused "${CMAKE_SOURCE_DIR}/src/style/ua/html.css" "ua/html.css")
    ember_lz_pack(_unused "${CMAKE_SOURCE_DIR}/src/codec/data/brotli_dict.bin" "codec/brotli_dict.bin")
    ember_lz_pack_inc(tables/re_set_data "${CMAKE_SOURCE_DIR}/src/js/regexp_unicode.inc" embi_re_set_data 1)
    if(EMBER_NET)
        # Mozilla's roots as DER records (tools/lz/pem2der.c) instead of the PEM text: 200 KB to 127 KB.
        set(_roots_der "${CMAKE_BINARY_DIR}/gen/ember_lz/net/roots.der")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/gen/ember_lz/net")
        add_custom_command(
            OUTPUT "${_roots_der}"
            COMMAND ember_pem2der "${CMAKE_SOURCE_DIR}/third_party/cacert/cacert.pem" "${_roots_der}"
            DEPENDS ember_pem2der "${CMAKE_SOURCE_DIR}/third_party/cacert/cacert.pem"
            COMMENT "pem2der cacert.pem"
            VERBATIM)
        file(WRITE "${_roots_der}.h.tmp"
            "/* Generated by CMake. Do not edit. */\n"
            "static const unsigned char embi_net_roots_der[] = {\n"
            "#embed \"roots.der\"\n"
            "};\n")
        configure_file("${_roots_der}.h.tmp" "${_roots_der}.h" COPYONLY)
        set_property(GLOBAL APPEND PROPERTY EMBER_LZ_OUTPUTS "${_roots_der}")
    endif()
    ember_lz_finish()
endif()

# ---------------------------------------------------------------- modules
set(EMBER_LIBRARY_OBJECTS "")
set(EMBER_MODULES "")
file(GLOB _ember_mod_entries LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*")
foreach(_dir IN LISTS _ember_mod_entries)
    if(NOT IS_DIRECTORY "${_dir}")
        continue()
    endif()
    get_filename_component(_mod "${_dir}" NAME)
    if(_mod STREQUAL "net" AND NOT EMBER_NET)
        continue()
    endif()
    if((_mod STREQUAL "canvas" AND NOT EMBER_CANVAS) OR (_mod STREQUAL "svg" AND NOT EMBER_SVG))
        continue()
    endif()
    file(GLOB _srcs CONFIGURE_DEPENDS "${_dir}/*.c")
    if(NOT _srcs)
        continue()
    endif()
    list(APPEND EMBER_MODULES "${_mod}")

    add_library(ember_${_mod} OBJECT ${_srcs})
    target_include_directories(ember_${_mod} PUBLIC
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/gen")
    target_compile_definitions(ember_${_mod} PRIVATE EMBER_BUILDING)
    if(EMBER_NET)
        target_compile_definitions(ember_${_mod} PRIVATE EMBER_NET=1)
    endif()
    if(EMBER_SHARED)
        target_compile_definitions(ember_${_mod} PUBLIC EMBER_SHARED)
        set_target_properties(ember_${_mod} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    target_link_libraries(ember_${_mod} PUBLIC SDL3::SDL3-static)
    # Only emb_* (EMB_API) is public. Hidden by default lets the compiler bind calls locally and keeps embi_* out of
    # the dynamic symbol table of the shared library. Tests link these objects statically, so they are not affected.
    if(NOT WIN32)
        set_target_properties(ember_${_mod} PROPERTIES C_VISIBILITY_PRESET hidden)
    endif()
    ember_set_warnings(ember_${_mod})
    ember_set_sanitizers(ember_${_mod})
    add_dependencies(ember_${_mod} ember_shaders)
    if(EMBER_COMPRESS_ASSETS)
        target_compile_definitions(ember_${_mod} PRIVATE EMBER_LZ_ASSETS=1)
        add_dependencies(ember_${_mod} ember_lz_assets)
    endif()
    # Optional per-module hook (per-source flags, extra include dirs, vendored code). It runs with
    # _mod, _dir and the target ember_${_mod} in scope.
    if(EXISTS "${_dir}/module.cmake")
        include("${_dir}/module.cmake")
    endif()
    list(APPEND EMBER_LIBRARY_OBJECTS "$<TARGET_OBJECTS:ember_${_mod}>")
endforeach()
# per-source flag (COMPILE_OPTIONS comes after the directory's -Oz, and under LTO it is kept per function).
if(EMBER_MIN_SIZE AND EMBER_SPEED_SOURCES AND NOT MSVC)
    foreach(_f IN LISTS EMBER_SPEED_SOURCES)
        set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/${_f}" APPEND PROPERTY COMPILE_OPTIONS "${EMBER_SPEED_OPT}")
    endforeach()
endif()

# Module dependencies from src/<mod>/deps.txt (whitespace separated module names).
foreach(_mod IN LISTS EMBER_MODULES)
    set(_deps_file "${CMAKE_SOURCE_DIR}/src/${_mod}/deps.txt")
    set(_deps "")
    if(EXISTS "${_deps_file}")
        file(READ "${_deps_file}" _deps_text)
        string(REGEX REPLACE "[ \t\r\n]+" ";" _deps "${_deps_text}")
        list(REMOVE_ITEM _deps "")
    endif()
    # a module of a feature that is off is not built (and not a dependency).
    if(NOT EMBER_CANVAS)
        list(REMOVE_ITEM _deps canvas)
    endif()
    if(NOT EMBER_SVG)
        list(REMOVE_ITEM _deps svg)
    endif()
    set_property(GLOBAL PROPERTY ember_deps_${_mod} "${_deps}")
    foreach(_dep IN LISTS _deps)
        if(NOT TARGET ember_${_dep})
            message(FATAL_ERROR "src/${_mod}/deps.txt: unknown module '${_dep}'")
        endif()
        target_link_libraries(ember_${_mod} PUBLIC ember_${_dep})
    endforeach()
endforeach()

# The embedding layer uses the network service when it is built: the runtime creates and destroys it.
if(EMBER_NET AND TARGET ember_embed AND TARGET ember_net)
    get_property(_embed_deps GLOBAL PROPERTY ember_deps_embed)
    set_property(GLOBAL PROPERTY ember_deps_embed "${_embed_deps};net")
    target_link_libraries(ember_embed PUBLIC ember_net)
endif()

# Aggregate library with every module object. It is generated from an empty
# translation unit while there are no modules yet.
if(EMBER_MODULES)
    set(_ember_lib_srcs ${EMBER_LIBRARY_OBJECTS})
else()
    set(_empty_src "${CMAKE_BINARY_DIR}/gen/ember_empty.c")
    file(WRITE "${_empty_src}.tmp" "typedef int embi_empty_unit;\n")
    configure_file("${_empty_src}.tmp" "${_empty_src}" COPYONLY)
    set(_ember_lib_srcs "${_empty_src}")
endif()

if(EMBER_SHARED)
    add_library(ember SHARED ${_ember_lib_srcs})
    target_compile_definitions(ember PUBLIC EMBER_SHARED)
else()
    add_library(ember STATIC ${_ember_lib_srcs})
endif()
target_include_directories(ember PUBLIC "${CMAKE_SOURCE_DIR}/include")
if(EMBER_SHARED AND EMBER_SDL_EXTERNAL)
    # The module objects name SDL functions; the dynamic loader resolves them from the host's SDL3 (and SDL_image).
    target_link_libraries(ember PUBLIC SDL3::Headers)
else()
    target_link_libraries(ember PUBLIC SDL3::SDL3-static)
endif()
if(EMBER_SHARED AND NOT WIN32 AND NOT EMBER_SDL_EXTERNAL)
    message(STATUS "EMBER_SHARED without EMBER_SDL_EXTERNAL: libember carries a private SDL3 (only emb_* is exported). A host that "
        "links its own SDL3 and passes its SDL_GPUDevice* / SDL_GPUCommandBuffer* (EMB_VIEW_OFFSCREEN) would then use two SDL "
        "copies; build with -DEMBER_SDL_EXTERNAL=ON for that, or keep the two SDL versions identical (see include/ember/ember.h).")
endif()
set(_ember_outline_n 0)
if(EMBER_SHARED AND UNIX AND NOT APPLE)
    # Export emb_* only: not embi_* and not the SDL3 and SDL_image symbols linked in statically. This keeps ~9000
    # names out of .dynsym/.dynstr and lets the linker bind everything else locally.
    target_link_options(ember PRIVATE "-Wl,--version-script=${CMAKE_SOURCE_DIR}/cmake/ember.map")
    set_property(TARGET ember APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/ember.map")
    if(_ember_outline_on AND _ember_ipo)
        # The LTO code generation runs inside the linker: GNU ld and gold take the option through the LLVM plugin, lld through -mllvm.
        include(CheckCSourceCompiles)
        set(CMAKE_REQUIRED_FLAGS "-flto")
        foreach(_form "-Wl,-plugin-opt=-enable-machine-outliner=always" "-Wl,-mllvm,-enable-machine-outliner=always")
            set(CMAKE_REQUIRED_LINK_OPTIONS "${_form}")
            check_c_source_compiles("int main(void) { return 0; }" _ember_outline_ok_${_ember_outline_n})
            if(_ember_outline_ok_${_ember_outline_n})
                target_link_options(ember PRIVATE "${_form}")
                break()
            endif()
            math(EXPR _ember_outline_n "${_ember_outline_n} + 1")
        endforeach()
        if(NOT _ember_outline_ok_0 AND NOT _ember_outline_ok_1)
            message(STATUS "EMBER_OUTLINE: the linker accepts neither -plugin-opt nor -mllvm; the outliner is off in the LTO link")
        endif()
        unset(CMAKE_REQUIRED_FLAGS)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)
    endif()
    if(EMBER_RELR)
        include(CheckLinkerFlag)
        check_linker_flag(C "-Wl,-z,pack-relative-relocs" _ember_have_relr)
        if(_ember_have_relr)
            target_link_options(ember PRIVATE "-Wl,-z,pack-relative-relocs")
        else()
            message(WARNING "EMBER_RELR: the linker does not support -z pack-relative-relocs")
        endif()
    endif()
endif()
ember_set_warnings(ember)
ember_set_sanitizers(ember)
add_dependencies(ember ember_shaders)

if(EXISTS "${CMAKE_SOURCE_DIR}/include/ember")
    install(DIRECTORY include/ember DESTINATION include)
endif()
install(TARGETS ember ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)

# ---------------------------------------------------------------- tests and examples
if(EMBER_TESTS)
    enable_testing()
    add_subdirectory(tests)
    # tests/CMakeLists.txt creates the test executables; sanitize them here, per target.
    ember_set_sanitizers_in_dir("${CMAKE_SOURCE_DIR}/tests")
endif()

if(EMBER_EXAMPLES AND EXISTS "${CMAKE_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
    ember_set_sanitizers_in_dir("${CMAKE_SOURCE_DIR}/examples")
endif()

# ---------------------------------------------------------------- fuzz harnesses (EMBER_FUZZ)
if(EMBER_FUZZ AND EXISTS "${CMAKE_SOURCE_DIR}/tests/fuzz/CMakeLists.txt")
    add_subdirectory(tests/fuzz)
endif()

# ---------------------------------------------------------------- host tools (generators)
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()
