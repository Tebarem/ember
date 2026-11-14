# Examples are built, not run, in CI.
#
# hello_window is a desktop example: it uses a plain main() (SDL_MAIN_HANDLED), which is not an
# entry point on Android or iOS, so it is skipped there.
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

# hello_window uses internal embi_* symbols (the GPU wrapper, the generated shader tables), which a shared libember does
# not export: it is built only against the static library, like the benches.
if(NOT EMBER_SHARED)
    add_executable(hello_window hello_window.c)
    target_include_directories(hello_window PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/gen")
    target_link_libraries(hello_window PRIVATE ember SDL3::SDL3-static)
    ember_set_warnings(hello_window)
    add_dependencies(hello_window ember_shaders)
endif()

# viewer opens a window and shows a page until it is closed; offscreen is a host window that owns the device and blits
# the view's texture (with a spinning quad of its own). Both use only the public API (include/ember/ember.h), plus the
# generated shader header of offscreen.
add_executable(viewer viewer/main.c)
target_include_directories(viewer PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(viewer PRIVATE ember SDL3::SDL3-static)
ember_set_warnings(viewer)
# On Windows the viewer is a windowed (GUI subsystem) program, so starting it opens only its window, no console. It keeps
# its plain main(): MinGW enters main() under -mwindows, and MSVC (clang-cl too) is pointed at mainCRTStartup. The log
# still goes to stderr, which no console shows any more: redirect it to read it (viewer page.html 2> log.txt).
if(WIN32)
    if(MSVC)
        target_link_options(viewer PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        target_link_options(viewer PRIVATE -mwindows)
    endif()
endif()

# offscreen draws its own spinning quad with shaders/host_spin.slang (ember_shaders/host_spin.h), so it needs the generated
# shader headers and the blob type from src/. It also calls embi_gpu_blob_open (the blob may be stored compressed), which a
# shared libember does not export: in a shared build the example links the gpu module objects itself, like the tests. It
# passes its own SDL_GPUDevice to the library, so with a shared libember it is the two-SDL case of include/ember/ember.h
# (EMBER_SDL_EXTERNAL=ON avoids it, but then the host must bring SDL3 as a shared library; this example links it statically).
add_executable(offscreen offscreen/main.c)
target_include_directories(offscreen PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/gen")
target_link_libraries(offscreen PRIVATE ember SDL3::SDL3-static)
if(EMBER_SHARED)
    target_link_libraries(offscreen PRIVATE ember_gpu ember_base)
endif()
ember_set_warnings(offscreen)
add_dependencies(offscreen ember_shaders)

# vue_app ships a Vue 3 + TanStack Query app (vue-app/web, built to vue-app/web/dist with npm) as one executable: the
# build is embedded with #embed (cmake/EmberEmbedAssets.cmake) and served from app://local/, and the host answers the
# app's fetches to app://local/api/ in C. See vue-app/README.md.
include("${CMAKE_SOURCE_DIR}/cmake/EmberEmbedAssets.cmake")
add_executable(vue_app vue-app/main.c)
target_include_directories(vue_app PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vue_app PRIVATE ember SDL3::SDL3-static)
ember_set_warnings(vue_app)
ember_embed_assets(vue_app "${CMAKE_CURRENT_SOURCE_DIR}/vue-app/web/dist")
if(WIN32)
    if(MSVC)
        target_link_options(vue_app PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        target_link_options(vue_app PRIVATE -mwindows)
    endif()
endif()
