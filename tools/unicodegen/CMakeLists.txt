# unicodegen regenerates src/jslex/unicode_id.inc from the UCD files in tools/unicodegen/data:
#     cmake --build build/<dir> --target unicodegen
# The generated table is checked in, so a normal build never runs the generator.

add_executable(unicodegen_tool EXCLUDE_FROM_ALL unicodegen.c)
ember_set_warnings(unicodegen_tool)

add_custom_target(unicodegen
    COMMAND unicodegen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_SOURCE_DIR}/src/jslex/unicode_id.inc"
    DEPENDS unicodegen_tool
    COMMENT "Regenerating src/jslex/unicode_id.inc from UCD 17.0.0"
    VERBATIM)

# casegen regenerates src/js/unicode_case.inc (case mapping and Cased/Case_Ignorable tables):
#     cmake --build build/<dir> --target unicodecase
add_executable(casegen_tool EXCLUDE_FROM_ALL casegen.c)
ember_set_warnings(casegen_tool)

add_custom_target(unicodecase
    COMMAND casegen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_SOURCE_DIR}/src/js/unicode_case.inc"
    DEPENDS casegen_tool
    COMMENT "Regenerating src/js/unicode_case.inc from UCD 17.0.0"
    VERBATIM)

# normgen regenerates src/js/unicode_norm.inc (canonical and compatibility decompositions, ccc,
# and the composition pairs behind String.prototype.normalize):
#     cmake --build build/<dir> --target unicodenorm
add_executable(normgen_tool EXCLUDE_FROM_ALL normgen.c)
ember_set_warnings(normgen_tool)

add_custom_target(unicodenorm
    COMMAND normgen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_SOURCE_DIR}/src/js/unicode_norm.inc"
    DEPENDS normgen_tool
    COMMENT "Regenerating src/js/unicode_norm.inc from UCD 17.0.0"
    VERBATIM)

# propgen regenerates src/js/regexp_unicode.inc (General_Category, Script, Script_Extensions and
# binary property sets for RegExp \p{...}, plus the case folding tables for the i flag):
#     cmake --build build/<dir> --target unicodeprops
add_executable(propgen_tool EXCLUDE_FROM_ALL propgen.c)
ember_set_warnings(propgen_tool)

add_custom_target(unicodeprops
    COMMAND propgen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_SOURCE_DIR}/src/js/regexp_unicode.inc"
    DEPENDS propgen_tool
    COMMENT "Regenerating src/js/regexp_unicode.inc from UCD 17.0.0"
    VERBATIM)

# emojigen regenerates src/js/regexp_emoji.inc (the properties of strings behind RegExp
# \p{RGI_Emoji} and friends) from emoji-sequences.txt and emoji-zwj-sequences.txt, which are not
# kept in data/ (fetch them from https://www.unicode.org/Public/emoji/17.0/ to regenerate):
#     cmake --build build/<dir> --target unicodeemoji
add_executable(emojigen_tool EXCLUDE_FROM_ALL emojigen.c)
ember_set_warnings(emojigen_tool)

add_custom_target(unicodeemoji
    COMMAND emojigen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_SOURCE_DIR}/src/js/regexp_emoji.inc"
    DEPENDS emojigen_tool
    COMMENT "Regenerating src/js/regexp_emoji.inc from the Unicode emoji data files"
    VERBATIM)

# idnagen regenerates src/loader/idna_table.inc (UTS #46 mapping table for the URL host parser) and
# src/loader/idna_props.inc (bidi, joining, mark and ccc runs, canonical decomposition and
# composition for NFC):
#     cmake --build build/<dir> --target unicodeidna
add_executable(idnagen_tool EXCLUDE_FROM_ALL idnagen.c)
ember_set_warnings(idnagen_tool)

add_custom_target(unicodeidna
    COMMAND idnagen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_SOURCE_DIR}/src/loader/idna_table.inc"
            "${CMAKE_SOURCE_DIR}/src/loader/idna_props.inc"
    DEPENDS idnagen_tool
    COMMENT "Regenerating src/loader/idna_table.inc and idna_props.inc from UCD 17.0.0"
    VERBATIM)

# encgen regenerates src/loader/encoding_tables.inc (encoding labels and single-byte tables):
#     cmake --build build/<dir> --target unicodeenc
add_executable(encgen_tool EXCLUDE_FROM_ALL encgen.c)
ember_set_warnings(encgen_tool)

add_custom_target(unicodeenc
    COMMAND encgen_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/data/encoding"
            "${CMAKE_SOURCE_DIR}/src/loader/encoding_tables.inc"
    DEPENDS encgen_tool
    COMMENT "Regenerating src/loader/encoding_tables.inc from the WHATWG Encoding Standard"
    VERBATIM)
