/*
 * Test support for a build without the bundled default font (EMBER_DEFAULT_FONT=OFF, SZ3b).
 *
 * Most tests draw or measure text and were written against the bundled face. In such a build tests/CMakeLists.txt links this
 * file into the test executables (not into the ones that test the absence of the font): before main it reads
 * assets/fonts/ember-default.ttf from the source tree and hands it to the font registry as the face every new registry starts
 * with, which is what an embedder that bundles its own default face does through emb_runtime_add_font. The tests of the
 * absence itself (tests/unit/text/test_text_nofont.c, ...) do not get it.
 */
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <stddef.h>

void embi_fontdb_test_default(const void *data, size_t size);

__attribute__((constructor)) static void embi_test_default_font_load(void)
{
    size_t n = 0;
    void *bytes = SDL_LoadFile(EMBER_SOURCE_DIR "/assets/fonts/ember-default.ttf", &n);
    if (bytes != nullptr) {
        embi_fontdb_test_default(bytes, n); /* kept for the life of the process */
    }
}
