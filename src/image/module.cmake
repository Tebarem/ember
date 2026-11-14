# SDL_image for module image (JPEG and GIF only).
#
# SDL_image is built static, with no install, samples or tests, every format off except JPG and
# GIF, stb_image as the JPEG backend and no OS codecs (the same decoder on every platform). Its
# sources get no Ember warning flags, so they build the way SDL_image builds them. Sanitizers
# (EMBER_SANITIZE) do apply, so the fuzz tests exercise the decoders under ASan and UBSan.
#
# Runs with _mod, _dir and the target ember_${_mod} in scope.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/SDL_image/CMakeLists.txt")
    message(FATAL_ERROR "SDL_image not found at third_party/SDL_image "
                        "(run: git submodule update --init third_party/SDL_image)")
endif()

if(NOT TARGET SDL3_image-static)
    # Options are cache variables set before add_subdirectory, as the root does for SDL3.
    set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_INSTALL_CPACK OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TESTS_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_VENDORED OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_STRICT OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_WERROR OFF CACHE BOOL "" FORCE)

    set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
    set(SDLIMAGE_BACKEND_WIC OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_BACKEND_IMAGEIO OFF CACHE BOOL "" FORCE)

    foreach(_fmt ANI AVIF BMP JXL LBM PCX PNG PNM QOI SVG TGA TIF WEBP XCF XPM XV)
        set(SDLIMAGE_${_fmt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SDLIMAGE_JPG ON CACHE BOOL "" FORCE)
    set(SDLIMAGE_GIF ON CACHE BOOL "" FORCE)
    set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "" FORCE)

    foreach(_fmt ANI_SAVE AVIF_SAVE BMP_SAVE GIF_SAVE JPG_SAVE PNG_SAVE TGA_SAVE WEBP_SAVE)
        set(SDLIMAGE_${_fmt} OFF CACHE BOOL "" FORCE)
    endforeach()

    # A normal variable shadows the cache entry while SDL_image is configured, so the rest of
    # the project keeps its own BUILD_SHARED_LIBS. unset() below reveals the cache value again.
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/SDL_image"
                     "${CMAKE_BINARY_DIR}/third_party/SDL_image" EXCLUDE_FROM_ALL)
    unset(BUILD_SHARED_LIBS)

    set_target_properties(SDL3_image-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    ember_set_sanitizers(SDL3_image-static)
endif()

target_link_libraries(ember_${_mod} PRIVATE SDL3_image::SDL3_image-static)

# The aggregate library `ember` is built from the module objects, so the PRIVATE link above does
# not reach it. embi_sdl_decode calls IMG_LoadTyped_IO, so `ember` (static consumers and the
# shared library) needs SDL3_image too. The root creates `ember` after the module loop, so this
# link is deferred to the end of the root directory. PUBLIC lets static consumers of libember.a
# get the library on their link line as well.
function(_ember_image_link_aggregate)
    if(TARGET ember AND NOT (EMBER_SHARED AND EMBER_SDL_EXTERNAL))
        target_link_libraries(ember PUBLIC SDL3_image::SDL3_image-static)
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _ember_image_link_aggregate)
