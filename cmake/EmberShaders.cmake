# Shader pipeline: shaders/*.slang -> slangc -> SPIR-V / DXIL / MSL blobs, embedded
# into generated C headers with #embed.
#
# Programs:
#   shaders/<name>.slang          graphics program, entries vsMain (vertex) and fsMain (fragment)
#   shaders/<name>.comp.slang     compute program, entry csMain
#   shaders/lib/**                imported modules, compiled into every program
#
# Resource counts come from annotation lines in the program source:
#   //! vs: samplers=0 storage_textures=0 storage_buffers=0 uniform_buffers=0
#   //! fs: ...
#   //! cs: samplers=0 ro_storage_textures=0 ro_storage_buffers=0 rw_storage_textures=0
#   //!     rw_storage_buffers=0 uniform_buffers=0 threads=64,1,1
# Missing keys count as 0 (threads defaults to 1,1,1).
#
# Headers are written at configure time. Annotations are configure-time inputs, so every
# shader source is a CMAKE_CONFIGURE_DEPENDS entry: editing it reconfigures and regenerates
# the header if its counts changed. Blobs are build-time custom commands whose DEPENDS are
# only that program's source plus shaders/lib, so editing one program re-runs only its
# slangc steps.

# Parses "//! <stage>: key=value ..." annotations of <src> for a <kind> program
# (graphics or compute). Results are set in the parent scope as
# <outprefix>_<stage>_<key>.
function(_ember_parse_shader_annotations src kind outprefix)
    if(kind STREQUAL "compute")
        set(_allowed_keys samplers ro_storage_textures ro_storage_buffers
            rw_storage_textures rw_storage_buffers uniform_buffers threads)
    else()
        set(_allowed_keys samplers storage_textures storage_buffers uniform_buffers)
    endif()

    file(STRINGS "${src}" _ann_lines REGEX "^//! ")
    foreach(_ln IN LISTS _ann_lines)
        if(NOT "${_ln}" MATCHES "^//! (vs|fs|cs):(.*)$")
            message(FATAL_ERROR "${src}: malformed shader annotation: ${_ln}")
        endif()
        set(_stage "${CMAKE_MATCH_1}")
        set(_body "${CMAKE_MATCH_2}")
        if(kind STREQUAL "compute" AND NOT _stage STREQUAL "cs")
            message(FATAL_ERROR "${src}: compute program only takes a cs: annotation")
        elseif(NOT kind STREQUAL "compute" AND _stage STREQUAL "cs")
            message(FATAL_ERROR "${src}: graphics program takes vs: and fs: annotations, not cs:")
        endif()

        separate_arguments(_toks UNIX_COMMAND "${_body}")
        foreach(_tok IN LISTS _toks)
            if(NOT "${_tok}" MATCHES "^([a-z_]+)=([0-9]+(,[0-9]+)*)$")
                message(FATAL_ERROR "${src}: bad annotation token '${_tok}'")
            endif()
            set(_key "${CMAKE_MATCH_1}")
            set(_val "${CMAKE_MATCH_2}")
            list(FIND _allowed_keys "${_key}" _kidx)
            if(_kidx EQUAL -1)
                message(FATAL_ERROR "${src}: unknown resource key '${_key}' for ${kind} program")
            endif()
            set(${outprefix}_${_stage}_${_key} "${_val}" PARENT_SCOPE)
        endforeach()
    endforeach()
endfunction()

# Reads one annotation value from the parsed variables, default 0.
function(_ember_annot_value outvar outprefix stage key)
    if(DEFINED ${outprefix}_${stage}_${key})
        set(${outvar} "${${outprefix}_${stage}_${key}}" PARENT_SCOPE)
    elseif(key STREQUAL "threads")
        set(${outvar} "1,1,1" PARENT_SCOPE)
    else()
        set(${outvar} "0" PARENT_SCOPE)
    endif()
endfunction()

# Chooses the DXC directory (dxcompiler.dll + dxil.dll) from the newest Windows SDK.
function(_ember_find_dxc_dir outvar)
    # The Windows SDK installs under ProgramFiles(x86); the default location is the fallback.
    set(_pf86 "C:/Program Files (x86)")
    if(DEFINED ENV{ProgramFiles\(x86\)} AND NOT "$ENV{ProgramFiles\(x86\)}" STREQUAL "")
        set(_pf86 "$ENV{ProgramFiles\(x86\)}")
    endif()
    set(_kit_roots "${_pf86}/Windows Kits/10/bin")
    set(_found "")
    foreach(_root IN LISTS _kit_roots)
        file(GLOB _ver_dirs LIST_DIRECTORIES true "${_root}/*")
        foreach(_d IN LISTS _ver_dirs)
            if(EXISTS "${_d}/x64/dxcompiler.dll")
                list(APPEND _found "${_d}/x64")
            endif()
        endforeach()
    endforeach()
    if(_found)
        list(SORT _found COMPARE NATURAL ORDER DESCENDING)
        list(GET _found 0 _dir)
        set(${outvar} "${_dir}" PARENT_SCOPE)
    else()
        set(${outvar} "" PARENT_SCOPE)
    endif()
endfunction()

function(ember_add_shaders)
    file(GLOB _progs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/shaders/*.slang")
    file(GLOB_RECURSE _libs CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/shaders/lib/*.slang"
        "${CMAKE_SOURCE_DIR}/shaders/lib/*.slangh")

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_progs} ${_libs})

    # The clip-depth constant lives in both C and Slang. Fail the configure if they differ.
    set(_types_h "${CMAKE_SOURCE_DIR}/src/gpu/render_types.h")
    set(_types_s "${CMAKE_SOURCE_DIR}/shaders/lib/types.slang")
    if(EXISTS "${_types_h}" AND EXISTS "${_types_s}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_types_h}" "${_types_s}")
        file(STRINGS "${_types_h}" _depth_c REGEX "^#define EMBI_MAX_CLIP_DEPTH [0-9]+")
        file(STRINGS "${_types_s}" _depth_s REGEX "^static const uint MAX_CLIP_DEPTH = [0-9]+;")
        string(REGEX REPLACE "[^0-9]" "" _depth_c "${_depth_c}")
        string(REGEX REPLACE "[^0-9]" "" _depth_s "${_depth_s}")
        if(NOT _depth_c MATCHES "^[0-9]+$" OR NOT _depth_s MATCHES "^[0-9]+$"
           OR NOT _depth_c EQUAL _depth_s)
            message(FATAL_ERROR
                "EMBI_MAX_CLIP_DEPTH (render_types.h: '${_depth_c}') must equal MAX_CLIP_DEPTH "
                "(shaders/lib/types.slang: '${_depth_s}')")
        endif()
    endif()

    # Formats.
    set(_formats "")
    string(TOLOWER "${EMBER_SHADER_FORMATS}" _req)
    foreach(_f IN LISTS _req)
        if(NOT "${_f}" MATCHES "^(spirv|dxil|msl)$")
            message(FATAL_ERROR "EMBER_SHADER_FORMATS: unknown format '${_f}' (spirv, dxil, msl)")
        endif()
        list(APPEND _formats "${_f}")
    endforeach()

    set(_dxc_dir "")
    list(FIND _formats dxil _dxil_idx)
    if(NOT _dxil_idx EQUAL -1)
        if(NOT WIN32)
            message(WARNING "dxil shader format skipped: DXIL is only built on a Windows host")
            list(REMOVE_ITEM _formats dxil)
        else()
            if(EMBER_DXC_DIR)
                set(_dxc_dir "${EMBER_DXC_DIR}")
            else()
                _ember_find_dxc_dir(_dxc_dir)
            endif()
            if(NOT _dxc_dir)
                message(WARNING "dxil shader format skipped: no DXC directory found "
                    "(set EMBER_DXC_DIR to a folder that contains dxcompiler.dll)")
                list(REMOVE_ITEM _formats dxil)
            endif()
        endif()
    endif()

    set(_built_fmts "${_formats}")

    # Global format flags (one file for the whole build). Program headers do not repeat them.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/gen/ember_shaders")
    set(_fmt_hdr "/* Generated by CMake (cmake/EmberShaders.cmake). Do not edit. */\n")
    string(APPEND _fmt_hdr "#pragma once\n")
    string(APPEND _fmt_hdr "#include <SDL3/SDL_gpu.h>\n\n")
    set(_fmt_mask "0u")
    foreach(_fmt IN ITEMS spirv dxil msl)
        list(FIND _built_fmts "${_fmt}" _fi)
        if(_fi EQUAL -1)
            set(_v 0)
        else()
            set(_v 1)
        endif()
        string(TOUPPER "${_fmt}" _FMT)
        string(APPEND _fmt_hdr "#define EMBI_SHADER_FMT_${_FMT} ${_v}\n")
        string(APPEND _fmt_mask " | (EMBI_SHADER_FMT_${_FMT} ? SDL_GPU_SHADERFORMAT_${_FMT} : 0u)")
    endforeach()
    string(APPEND _fmt_hdr "#define EMBI_BUILT_SHADER_FORMATS (${_fmt_mask})\n")
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/gen/ember_shaders/formats.h" CONTENT "${_fmt_hdr}")

    if(NOT _progs)
        add_custom_target(ember_shaders)
        return()
    endif()

    # slangc.
    set(_slangc "${EMBER_SLANGC}")
    if(NOT _slangc)
        find_program(_slangc_found slangc)
        set(_slangc "${_slangc_found}")
    endif()
    if(NOT _slangc)
        message(FATAL_ERROR "slangc not found: set -DEMBER_SLANGC=<path to slangc>")
    endif()

    set(_debug_flags "")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_debug_flags -g)
    endif()

    set(_out_dir "${CMAKE_BINARY_DIR}/shaders")
    set(_hdr_dir "${CMAKE_BINARY_DIR}/gen/ember_shaders")
    file(MAKE_DIRECTORY "${_out_dir}")
    file(MAKE_DIRECTORY "${_hdr_dir}")

    set(_all_outputs "")
    set(_seen_names "")

    foreach(_src IN LISTS _progs)
        get_filename_component(_fname "${_src}" NAME)
        if("${_fname}" MATCHES "^(.+)\\.comp\\.slang$")
            set(_kind compute)
            set(_name "${CMAKE_MATCH_1}")
        elseif("${_fname}" MATCHES "^(.+)\\.slang$")
            set(_kind graphics)
            set(_name "${CMAKE_MATCH_1}")
        else()
            message(FATAL_ERROR "bad shader file name: ${_fname}")
        endif()

        list(FIND _seen_names "${_name}" _dup)
        if(NOT _dup EQUAL -1)
            message(FATAL_ERROR "two shader programs named '${_name}' in shaders/")
        endif()
        list(APPEND _seen_names "${_name}")

        # Annotations.
        set(_pfx "_ember_sh_${_name}")
        _ember_parse_shader_annotations("${_src}" "${_kind}" "${_pfx}")

        if(_kind STREQUAL "compute")
            set(_stages cs)
        else()
            set(_stages vs fs)
        endif()

        # Stage table: stage -> slang stage name and entry point.
        set(_hdr "/* Generated by CMake from shaders/${_fname}. Do not edit. */\n")
        string(APPEND _hdr "#pragma once\n")
        # src/ is an include directory of every Ember target, so this path is relative.
        string(APPEND _hdr "#include \"emb_shader_blob.h\"\n")
        # The global format flags (EMBI_SHADER_FMT_*) live in formats.h, next to this header.
        # Including it here keeps a program header self-sufficient for code that includes only
        # the program (for example the harness self-test).
        string(APPEND _hdr "#include \"formats.h\"\n")

        # Blobs: one slangc invocation per stage and format.
        foreach(_st IN LISTS _stages)
            if(_st STREQUAL "vs")
                set(_slang_stage vertex)
                set(_entry vsMain)
            elseif(_st STREQUAL "fs")
                set(_slang_stage fragment)
                set(_entry fsMain)
            else()
                set(_slang_stage compute)
                set(_entry csMain)
            endif()

            foreach(_fmt IN ITEMS spirv dxil msl)
                list(FIND _built_fmts "${_fmt}" _fi)
                if(_fi EQUAL -1)
                    continue()
                endif()

                if(_fmt STREQUAL "spirv")
                    set(_ext spv)
                    set(_fmt_args -target spirv -fvk-use-entrypoint-name)
                elseif(_fmt STREQUAL "dxil")
                    set(_ext dxil)
                    set(_fmt_args -target dxil -profile sm_6_0)
                else()
                    set(_ext metal)
                    set(_fmt_args -target metal)
                endif()

                set(_blob "${_out_dir}/${_name}.${_st}.${_ext}")
                list(APPEND _all_outputs "${_blob}")

                if(_fmt STREQUAL "dxil")
                    # slangc finds dxcompiler.dll through PATH. DXIL is only built on WIN32 hosts,
                    # where the PATH separator is ';'.
                    add_custom_command(
                        OUTPUT "${_blob}"
                        COMMAND "${CMAKE_COMMAND}" -E env "PATH=${_dxc_dir};$ENV{PATH}"
                                "${_slangc}" "${_src}" ${_fmt_args}
                                -entry ${_entry} -stage ${_slang_stage}
                                -o "${_blob}" -I "${CMAKE_SOURCE_DIR}/shaders/lib" -O2 ${_debug_flags}
                        DEPENDS "${_src}" ${_libs}
                        COMMENT "slangc ${_name}.${_st} (${_fmt})"
                        VERBATIM)
                else()
                    set(_opt -O2)
                    if(_fmt STREQUAL "spirv")
                        set(_opt ${EMBER_SPIRV_OPT})
                    endif()
                    add_custom_command(
                        OUTPUT "${_blob}"
                        COMMAND "${_slangc}" "${_src}" ${_fmt_args}
                                -entry ${_entry} -stage ${_slang_stage}
                                -o "${_blob}" -I "${CMAKE_SOURCE_DIR}/shaders/lib" ${_opt} ${_debug_flags}
                        DEPENDS "${_src}" ${_libs}
                        COMMENT "slangc ${_name}.${_st} (${_fmt})"
                        VERBATIM)
                endif()
                if(EMBER_COMPRESS_ASSETS)
                    set(_lz_kind "")
                    # The strip drops OpString and the names that -g's NonSemantic debug instructions refer to: only without -g.
                    if(_fmt STREQUAL "spirv" AND NOT _debug_flags)
                        set(_lz_kind SPIRV)
                    endif()
                    ember_lz_pack(_lz "${_blob}" "shaders/${_name}.${_st}.${_ext}" ${_lz_kind} DEPENDS "${_blob}")
                    list(APPEND _all_outputs "${_lz}")
                endif()
            endforeach()
        endforeach()

        # Header body: one #embed array per built blob, then the program object.
        foreach(_st IN LISTS _stages)
            foreach(_fmt IN ITEMS spirv dxil msl)
                list(FIND _built_fmts "${_fmt}" _fi)
                if(_fi EQUAL -1)
                    continue()
                endif()
                if(_fmt STREQUAL "spirv")
                    set(_ext spv)
                elseif(_fmt STREQUAL "dxil")
                    set(_ext dxil)
                else()
                    set(_ext metal)
                endif()
                set(_blob "${_out_dir}/${_name}.${_st}.${_ext}")
                if(EMBER_COMPRESS_ASSETS)
                    set(_blob "${CMAKE_BINARY_DIR}/gen/ember_lz/shaders/${_name}.${_st}.${_ext}.lz")
                endif()
                # #embed resolves quoted paths against the header's directory.
                file(RELATIVE_PATH _blob_rel "${_hdr_dir}" "${_blob}")
                string(APPEND _hdr
                    "static const unsigned char embi_blob_${_name}_${_st}_${_fmt}[] = {\n"
                    "#embed \"${_blob_rel}\"\n"
                    "};\n")
            endforeach()
        endforeach()

        # Blob initializer for one stage: built formats point at their array, the rest are empty.
        foreach(_st IN LISTS _stages)
            foreach(_fmt IN ITEMS spirv dxil msl)
                set(_bi_${_st}_${_fmt} "{nullptr, 0, nullptr, false}")
                list(FIND _built_fmts "${_fmt}" _fi)
                if(NOT _fi EQUAL -1)
                    if(_st STREQUAL "vs")
                        set(_entry vsMain)
                    elseif(_st STREQUAL "fs")
                        set(_entry fsMain)
                    else()
                        set(_entry csMain)
                    endif()
                    if(EMBER_COMPRESS_ASSETS)
                        set(_packed ", true")
                    else()
                        set(_packed ", false")
                    endif()
                    set(_bi_${_st}_${_fmt}
                        "{embi_blob_${_name}_${_st}_${_fmt}, sizeof(embi_blob_${_name}_${_st}_${_fmt}), \"${_entry}\"${_packed}}")
                endif()
            endforeach()
        endforeach()

        if(_kind STREQUAL "compute")
            _ember_annot_value(_thr "${_pfx}" cs threads)
            string(REPLACE "," ";" _thr_l "${_thr}")
            list(GET _thr_l 0 _tx)
            list(GET _thr_l 1 _ty)
            list(GET _thr_l 2 _tz)
            set(_counts "")
            foreach(_k samplers ro_storage_textures ro_storage_buffers
                    rw_storage_textures rw_storage_buffers uniform_buffers)
                _ember_annot_value(_v "${_pfx}" cs ${_k})
                string(APPEND _counts "    .num_${_k} = ${_v},\n")
            endforeach()
            string(APPEND _hdr
                "static const embi_compute_program embi_shader_${_name} = {\n"
                "    .spirv = ${_bi_cs_spirv},\n"
                "    .dxil = ${_bi_cs_dxil},\n"
                "    .msl = ${_bi_cs_msl},\n"
                "${_counts}"
                "    .threads_x = ${_tx}, .threads_y = ${_ty}, .threads_z = ${_tz},\n"
                "};\n")
        else()
            set(_stage_inits "")
            foreach(_st IN ITEMS vs fs)
                set(_counts "")
                foreach(_k samplers storage_textures storage_buffers uniform_buffers)
                    _ember_annot_value(_v "${_pfx}" ${_st} ${_k})
                    string(APPEND _counts "        .num_${_k} = ${_v},\n")
                endforeach()
                string(APPEND _stage_inits
                    "    .${_st} = {\n"
                    "        .spirv = ${_bi_${_st}_spirv},\n"
                    "        .dxil = ${_bi_${_st}_dxil},\n"
                    "        .msl = ${_bi_${_st}_msl},\n"
                    "${_counts}"
                    "    },\n")
            endforeach()
            string(APPEND _hdr
                "static const embi_shader_program embi_shader_${_name} = {\n"
                "${_stage_inits}"
                "};\n")
        endif()

        set(_hdr_out "${_hdr_dir}/${_name}.h")
        file(GENERATE OUTPUT "${_hdr_out}" CONTENT "${_hdr}")
    endforeach()

    add_custom_target(ember_shaders DEPENDS ${_all_outputs})
endfunction()
