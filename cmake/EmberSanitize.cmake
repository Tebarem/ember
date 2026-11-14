# Sanitizers for Ember targets (EMBER_SANITIZE=ON). Flags go on each Ember target, never
# globally, so SDL3 and its try-compiles keep their own flags.
#
#   ember_set_sanitizers(<target>)        modules, the aggregate library, one test or example
#   ember_set_sanitizers_in_dir(<dir>)    every target created in <dir> and in its subdirectories
#                                         (used for tests/ and examples/). Call it after the
#                                         add_subdirectory() that creates the targets.
#
# Compile options go on every target. Link options go on every target that links, which is
# every target except OBJECT libraries (their objects are linked by the consumers).

set(_ember_sanitize_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
# EMBER_FUZZ (clang): coverage instrumentation for libFuzzer on every Ember target, and a UBSan finding
# aborts instead of printing and carrying on, so the fuzzer sees it as a crash.
if(EMBER_FUZZ)
    set(_ember_sanitize_flags -fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined
                              -fno-omit-frame-pointer)
endif()

# Windows clang (MSVC target) links the ASan runtime as a DLL. Without it next to the executable,
# running a test outside ctest fails with a "DLL not found" system dialog that blocks until it is
# dismissed. Each sanitized executable's output directory gets a copy at configure time.
set(_ember_asan_dll "")
if(EMBER_SANITIZE AND WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT MINGW)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
                    OUTPUT_VARIABLE _ember_clang_res OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(GLOB _ember_asan_dll "${_ember_clang_res}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT _ember_asan_dll)
        message(WARNING "EMBER_SANITIZE: clang_rt.asan_dynamic-x86_64.dll not found under "
                        "${_ember_clang_res}; sanitized executables need it on PATH")
    endif()
endif()

function(_ember_copy_asan_runtime target)
    if(NOT _ember_asan_dll)
        return()
    endif()
    get_target_property(_ember_out ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT _ember_out)
        get_target_property(_ember_out ${target} BINARY_DIR)
    endif()
    file(COPY "${_ember_asan_dll}" DESTINATION "${_ember_out}")
endfunction()

function(ember_set_sanitizers target)
    if(NOT EMBER_SANITIZE)
        return()
    endif()
    target_compile_options(${target} PRIVATE ${_ember_sanitize_flags})
    get_target_property(_ember_type ${target} TYPE)
    if(NOT _ember_type STREQUAL "OBJECT_LIBRARY")
        target_link_options(${target} PRIVATE ${_ember_sanitize_flags})
    endif()
    if(_ember_type STREQUAL "EXECUTABLE" OR _ember_type STREQUAL "SHARED_LIBRARY")
        _ember_copy_asan_runtime(${target})
    endif()
endfunction()

# BUILDSYSTEM_TARGETS only lists the targets of one directory, so the walk recurses through
# SUBDIRECTORIES. Each directory must already be processed by add_subdirectory().
function(ember_set_sanitizers_in_dir dir)
    if(NOT EMBER_SANITIZE)
        return()
    endif()
    get_property(_ember_dir_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_ember_t IN LISTS _ember_dir_targets)
        get_target_property(_ember_t_type ${_ember_t} TYPE)
        if(_ember_t_type STREQUAL "EXECUTABLE" OR _ember_t_type STREQUAL "SHARED_LIBRARY"
           OR _ember_t_type STREQUAL "STATIC_LIBRARY" OR _ember_t_type STREQUAL "OBJECT_LIBRARY")
            ember_set_sanitizers(${_ember_t})
        endif()
    endforeach()
    get_property(_ember_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_ember_sub IN LISTS _ember_subdirs)
        ember_set_sanitizers_in_dir("${_ember_sub}")
    endforeach()
endfunction()
