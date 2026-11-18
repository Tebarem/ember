# Font module hook. Runs with _mod, _dir and the target ember_${_mod} in scope.
#
# No fused multiply-adds: the outlines (CFF cubics turned into quadratics, variations) must come out the same on every
# platform, because the hinter in src/text rounds them to pixel rows. Clang on arm64 and GCC fuse by default, which moves
# a point by an ulp and can flip a rounding. MSVC does not fuse without /fp:contract.
if(NOT MSVC)
    target_compile_options(ember_${_mod} PRIVATE -ffp-contract=off)
endif()
