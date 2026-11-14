/*
 * CSS cursor keywords to SDL system cursors (W5d, platform/cursor.c).
 */
#include "emb_test.h"

#include "platform/platform.h"

#include <SDL3/SDL_mouse.h>

static int32_t cur(const char *kw)
{
    return embi_pf_cursor_from_css((embi_str){kw, (uint32_t)strlen(kw)});
}

EMB_TEST(keywords_map_to_system_cursors)
{
    EMB_CHECK_EQ_INT(cur("pointer"), SDL_SYSTEM_CURSOR_POINTER);
    EMB_CHECK_EQ_INT(cur("text"), SDL_SYSTEM_CURSOR_TEXT);
    EMB_CHECK_EQ_INT(cur("POINTER"), SDL_SYSTEM_CURSOR_POINTER); /* case-insensitive */
    EMB_CHECK_EQ_INT(cur("not-allowed"), SDL_SYSTEM_CURSOR_NOT_ALLOWED);
    EMB_CHECK_EQ_INT(cur("col-resize"), SDL_SYSTEM_CURSOR_EW_RESIZE);
    EMB_CHECK_EQ_INT(cur("nwse-resize"), SDL_SYSTEM_CURSOR_NWSE_RESIZE);
    EMB_CHECK_EQ_INT(cur("grab"), SDL_SYSTEM_CURSOR_MOVE);
    EMB_CHECK_EQ_INT(cur("wait"), SDL_SYSTEM_CURSOR_WAIT);
}

EMB_TEST(default_and_unknown_use_the_arrow)
{
    EMB_CHECK_EQ_INT(cur("auto"), -1);
    EMB_CHECK_EQ_INT(cur("default"), -1);
    EMB_CHECK_EQ_INT(cur("zoom-in"), -1);
    EMB_CHECK_EQ_INT(cur(""), -1);
    EMB_CHECK_EQ_INT(cur("a-keyword-that-is-far-too-long-to-be-one"), -1);
    EMB_CHECK_EQ_INT(cur("none"), -2); /* hide the cursor */
}

EMB_TEST_MAIN()
