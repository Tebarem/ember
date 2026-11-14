/*
 * kb_text_shape implementation TU (R2c). Compiled with warnings off (module.cmake).
 *
 * kb's allocator macros route through the Ember allocator. kb frees without a size, so every block
 * carries a 16-byte header with its size (keeps 16-byte alignment). kb's memory helpers use SDL.
 *
 * kb is not hardened against hostile fonts. src/font validates tables before kb sees a font
 * (see src/font).
 */
#define KB_TEXT_SHAPE_IMPLEMENTATION
#define KB_TEXT_SHAPE_NO_CRT

#include "base/base.h"

#include <SDL3/SDL_stdinc.h>

#define KBTS_EMB_HDR 16u

static void *kbts_emb_malloc(size_t size)
{
    uint8_t *p = embi_alloc(size + KBTS_EMB_HDR);
    *(size_t *)(void *)p = size;
    return p + KBTS_EMB_HDR;
}

static void kbts_emb_free(void *ptr)
{
    if (ptr == nullptr) {
        return;
    }
    uint8_t *base = (uint8_t *)ptr - KBTS_EMB_HDR;
    size_t size = *(size_t *)(void *)base;
    embi_free(base, size + KBTS_EMB_HDR);
}

#define KBTS_MALLOC(Data, Size) kbts_emb_malloc((Size))
#define KBTS_FREE(Data, Pointer) kbts_emb_free((Pointer))
#define KBTS_MEMSET(Dst, Value, Size) SDL_memset((Dst), (Value), (Size))
#define KBTS_MEMCPY(Dst, Src, Size) SDL_memcpy((Dst), (Src), (Size))

#include "kb_text_shape.h"
