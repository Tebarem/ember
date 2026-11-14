/*
 * The font metrics behind the font-relative units ex, cap, ch and ic (CSS Values 4 section 6.1.1), measured from the
 * first available face of an element's font-family list. Kept apart from compute.c because css/props.h and font/font.h
 * both define embi_font_metrics.
 */
#include "base/base.h"
#include <math.h>
#include "font/font.h"
#include "style/fontmetric.h"
#include "text/text.h"

bool embi_style_unit_metrics(embi_fontdb *db, const embi_str *families, uint32_t nfam, float weight, uint8_t style,
                             float stretch, double font_px, double out[5])
{
    out[0] = out[1] = out[2] = out[3] = out[4] = 0.0;
    if (db == nullptr || nfam == 0) {
        return false;
    }
    embi_style_match m = {families, nfam, weight, style, stretch};
    embi_face *faces[8];
    uint32_t nf = embi_fontdb_candidates(db, &m, faces, 8);
    /* The first available font is the first face that can show a space (a face whose unicode-range leaves it out is
     * skipped, CSS Fonts 4 section 5.1). */
    embi_face *face = nf > 0 ? faces[0] : nullptr;
    for (uint32_t i = 0; i < nf; i++) {
        if (embi_face_covers(faces[i], ' ')) {
            face = faces[i];
            break;
        }
    }
    if (face == nullptr) {
        return false;
    }
    const embi_font *font = embi_face_font(face);
    embi_font_metrics fm;
    SDL_memset(&fm, 0, sizeof fm);
    if (font == nullptr || !embi_font_get_metrics(font, &fm) || fm.units_per_em == 0) {
        return false;
    }
    double k = font_px / (double)fm.units_per_em;
    out[0] = fm.x_height > 0 ? (double)fm.x_height * k : 0.0;
    out[1] = fm.cap_height > 0 ? (double)fm.cap_height * k : 0.0;
    /* ch and ic are the advance of the glyph in the first face of the list that has it (a face whose unicode-range
     * leaves the character out is skipped). A glyph advance of zero is a real zero (the callers treat 0 as absent,
     * so it is stored as a tiny positive value). */
    static const uint32_t cps[2] = {'0', 0x6C34};
    for (int w = 0; w < 2; w++) {
        for (uint32_t i = 0; i < nf; i++) {
            if (!embi_face_covers(faces[i], cps[w])) {
                continue;
            }
            const embi_font *f2 = embi_face_font(faces[i]);
            embi_font_metrics fm2;
            SDL_memset(&fm2, 0, sizeof fm2);
            if (f2 == nullptr || !embi_font_get_metrics(f2, &fm2) || fm2.units_per_em == 0) {
                continue;
            }
            uint32_t gid = embi_font_glyph_index(f2, cps[w]);
            float adv = 0.0f;
            if (gid != 0 && embi_font_glyph_advance(f2, gid, nullptr, &adv)) {
                out[2 + w] = adv > 0.0f ? (double)adv * font_px / (double)fm2.units_per_em : 1e-9;
                break;
            }
        }
    }
    /* The normal line height: Blink rounds ascent, descent and line gap to whole px and adds them (the same rule as
     * embi_lay_line_metrics). */
    double asc = fm.use_typo_metrics ? fm.typo_ascender : fm.hhea_ascender;
    double desc = fm.use_typo_metrics ? -fm.typo_descender : -fm.hhea_descender;
    double gap = fm.use_typo_metrics ? fm.typo_line_gap : fm.hhea_line_gap;
    out[4] = round(asc * k) + round(desc * k) + round(gap * k);
    return true;
}
