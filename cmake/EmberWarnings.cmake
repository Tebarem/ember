# ember_set_warnings(<target>)
#
# Applies the Ember warning policy to one target only. It is never applied
# globally, so third-party code (SDL3) keeps its own warning settings.
function(ember_set_warnings target)
    set(_ember_warning_flags
        -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes
        -Wvla -Wimplicit-fallthrough)
    target_compile_options(${target} PRIVATE
        "$<$<C_COMPILER_ID:GNU,Clang>:${_ember_warning_flags}>")
endfunction()
