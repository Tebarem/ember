#include "emb_render_test.h"

#include "render_check.h"

#include "base/base.h"
#include "gpu/atlas.h"

/* Atlas allocator: shelf placement with the one-texel extrusion border, page overflow into a new
 * page, solo pages for big images, page reuse after frees, and the size limits. These need a
 * device (pages are GPU textures), and they skip without one. */

static embi_gpu *open_gpu(void)
{
    return embi_gpu_create(nullptr, false);
}

EMB_TEST(atlas_places_slots_with_extrusion_border)
{
    embi_gpu *g = open_gpu();
    if (g == nullptr) {
        EMB_SKIP("no GPU device");
    }
    embi_atlas *a = embi_atlas_create(embi_gpu_device(g));
    EMB_REQUIRE(a != nullptr);

    /* Each 100 x 100 image takes a 102 x 102 slot: a 1 px border on every side. */
    embi_atlas_region r[3];
    for (int i = 0; i < 3; i++) {
        EMB_REQUIRE(embi_atlas_alloc(a, 100, 100, &r[i]));
    }
    EMB_CHECK(r[0].page == r[1].page && r[1].page == r[2].page);
    EMB_CHECK(r[0].page_w == EMBI_ATLAS_PAGE && r[0].page_h == EMBI_ATLAS_PAGE);
    EMB_CHECK_EQ_INT(r[0].x, 1);
    EMB_CHECK_EQ_INT(r[0].y, 1);
    EMB_CHECK_EQ_INT(r[1].x, 103);
    EMB_CHECK_EQ_INT(r[1].y, 1);
    EMB_CHECK_EQ_INT(r[2].x, 205);
    EMB_CHECK_EQ_INT(r[2].y, 1);
    EMB_CHECK_EQ_INT(r[0].w, 100);
    EMB_CHECK_EQ_INT(r[0].h, 100);

    embi_atlas_destroy(a);
    embi_gpu_destroy(g);
}

EMB_TEST(atlas_opens_shelves_then_a_new_page)
{
    embi_gpu *g = open_gpu();
    if (g == nullptr) {
        EMB_SKIP("no GPU device");
    }
    embi_atlas *a = embi_atlas_create(embi_gpu_device(g));
    EMB_REQUIRE(a != nullptr);

    /* A page holds 20 slots a row (20 * 102 = 2040) and 20 rows, so 400 images. */
    embi_atlas_region last = {0};
    embi_atlas_region first = {0};
    for (uint32_t i = 0; i < 400u; i++) {
        embi_atlas_region r;
        EMB_REQUIRE(embi_atlas_alloc(a, 100, 100, &r));
        if (i == 0) {
            first = r;
        }
        last = r;
    }
    EMB_CHECK(first.page == last.page);
    /* The 400th image is the last slot of the last shelf: slot (19, 19), so (1939, 1939). */
    EMB_CHECK_EQ_INT(last.x, 1939);
    EMB_CHECK_EQ_INT(last.y, 1939);

    embi_atlas_region overflow;
    EMB_REQUIRE(embi_atlas_alloc(a, 100, 100, &overflow));
    EMB_CHECK(overflow.page != first.page);
    EMB_CHECK_EQ_INT(overflow.x, 1);
    EMB_CHECK_EQ_INT(overflow.y, 1);

    embi_atlas_destroy(a);
    embi_gpu_destroy(g);
}

EMB_TEST(atlas_solo_page_for_images_over_the_limit)
{
    embi_gpu *g = open_gpu();
    if (g == nullptr) {
        EMB_SKIP("no GPU device");
    }
    embi_atlas *a = embi_atlas_create(embi_gpu_device(g));
    EMB_REQUIRE(a != nullptr);

    embi_atlas_region shared;
    EMB_REQUIRE(embi_atlas_alloc(a, 64, 64, &shared));

    /* 1500 px wide is over the solo side, so the page is the image plus its border. */
    embi_atlas_region solo;
    EMB_REQUIRE(embi_atlas_alloc(a, 1500, 20, &solo));
    EMB_CHECK(solo.page != shared.page);
    EMB_CHECK(solo.tex != shared.tex);
    EMB_CHECK_EQ_INT(solo.page_w, 1502);
    EMB_CHECK_EQ_INT(solo.page_h, 22);
    EMB_CHECK_EQ_INT(solo.x, 1);
    EMB_CHECK_EQ_INT(solo.y, 1);

    /* Freed, its page slot goes to the next solo image. */
    const uint32_t solo_page = solo.page;
    embi_atlas_free(a, &solo);
    embi_atlas_region solo2;
    EMB_REQUIRE(embi_atlas_alloc(a, 1100, 30, &solo2));
    EMB_CHECK_EQ_INT(solo2.page, solo_page);
    EMB_CHECK_EQ_INT(solo2.page_w, 1102);

    embi_atlas_destroy(a);
    embi_gpu_destroy(g);
}

EMB_TEST(atlas_free_reuses_a_page_once_it_is_empty)
{
    embi_gpu *g = open_gpu();
    if (g == nullptr) {
        EMB_SKIP("no GPU device");
    }
    embi_atlas *a = embi_atlas_create(embi_gpu_device(g));
    EMB_REQUIRE(a != nullptr);

    embi_atlas_region p;
    embi_atlas_region q;
    EMB_REQUIRE(embi_atlas_alloc(a, 100, 100, &p));
    EMB_REQUIRE(embi_atlas_alloc(a, 100, 100, &q));
    EMB_CHECK_EQ_INT(q.x, 103);

    /* One region still lives on the page, so its space is not reused yet. */
    embi_atlas_free(a, &p);
    embi_atlas_region r;
    EMB_REQUIRE(embi_atlas_alloc(a, 100, 100, &r));
    EMB_CHECK_EQ_INT(r.x, 205);

    /* Once every region is freed, the page starts again from the top-left. */
    embi_atlas_free(a, &q);
    embi_atlas_free(a, &r);
    embi_atlas_region s;
    EMB_REQUIRE(embi_atlas_alloc(a, 50, 50, &s));
    EMB_CHECK_EQ_INT(s.page, p.page);
    EMB_CHECK_EQ_INT(s.x, 1);
    EMB_CHECK_EQ_INT(s.y, 1);

    embi_atlas_destroy(a);
    embi_gpu_destroy(g);
}

EMB_TEST(atlas_rejects_empty_and_oversized_images)
{
    embi_gpu *g = open_gpu();
    if (g == nullptr) {
        EMB_SKIP("no GPU device");
    }
    embi_atlas *a = embi_atlas_create(embi_gpu_device(g));
    EMB_REQUIRE(a != nullptr);

    embi_atlas_region r;
    EMB_CHECK(!embi_atlas_alloc(a, 0, 10, &r));
    EMB_CHECK(!embi_atlas_alloc(a, 10, 0, &r));
    EMB_CHECK(!embi_atlas_alloc(a, EMBI_ATLAS_MAX_SIDE + 1u, 1, &r));
    EMB_CHECK(!embi_atlas_alloc(a, 1, EMBI_ATLAS_MAX_SIDE + 1u, &r));
    EMB_CHECK(!embi_atlas_alloc(nullptr, 1, 1, &r));

    embi_atlas_destroy(a);
    embi_gpu_destroy(g);
}

EMB_TEST(atlas_upload_rejects_a_region_outside_its_page)
{
    embi_gpu *g = open_gpu();
    if (g == nullptr) {
        EMB_SKIP("no GPU device");
    }
    embi_atlas *a = embi_atlas_create(embi_gpu_device(g));
    EMB_REQUIRE(a != nullptr);

    embi_atlas_region r;
    EMB_REQUIRE(embi_atlas_alloc(a, 100, 100, &r));

    /* The validation returns before the command buffer or the pixels are used, so a non-null
     * stand-in for the command buffer and a zeroed pixel block are enough to reach it. */
    static uint8_t pixels[100 * 100 * 4];
    SDL_GPUCommandBuffer *const fake_cmd = (SDL_GPUCommandBuffer *)(uintptr_t)1;

    /* The slot is the image plus a one texel border, so the border's far edge must stay inside
     * the page: x + w + 1 <= page_w. Moving the region one texel further right or down, or
     * pointing it at the zero border, must all fail. */
    embi_atlas_region bad = r;
    bad.x = (uint16_t)(r.page_w - r.w);      /* far border edge lands one past the page */
    EMB_CHECK(!embi_atlas_upload(a, &bad, pixels, 100 * 4, fake_cmd));
    bad = r;
    bad.y = (uint16_t)(r.page_h - r.h);
    EMB_CHECK(!embi_atlas_upload(a, &bad, pixels, 100 * 4, fake_cmd));
    bad = r;
    bad.x = 0;                               /* no room for the left border texel */
    EMB_CHECK(!embi_atlas_upload(a, &bad, pixels, 100 * 4, fake_cmd));
    bad = r;
    bad.y = 0;
    EMB_CHECK(!embi_atlas_upload(a, &bad, pixels, 100 * 4, fake_cmd));

    embi_atlas_free(a, &r);
    embi_atlas_destroy(a);
    embi_gpu_destroy(g);
}

EMB_RENDER_TEST_MAIN()
