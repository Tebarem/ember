# Host-side tools: generators and benches.
#
# Every tools/<name>/ that has its own CMakeLists.txt is added automatically, so tasks can add a
# tool without editing this file. Generators are EXCLUDE_FROM_ALL and run through a named custom
# target (for example `cmake --build build/<dir> --target unicodegen`); their outputs are checked in.

file(GLOB _ember_tool_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*")
list(SORT _ember_tool_dirs)
foreach(_d IN LISTS _ember_tool_dirs)
    if(IS_DIRECTORY "${_d}" AND EXISTS "${_d}/CMakeLists.txt")
        add_subdirectory("${_d}")
    endif()
endforeach()
