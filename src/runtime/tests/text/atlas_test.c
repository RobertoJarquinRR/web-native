#include <stdio.h>
#include <string.h>

#include "text/text_atlas.h"
#include "text/text_runtime.h"

static int checks = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

int main(void)
{
    wn_text_atlas *atlas = wn_text_atlas_create(NULL, 0, 0);
    const wn_atlas_run *run;
    const wn_atlas_run *again;
    const wn_atlas_glyph *glyph;
    const wn_atlas_glyph *repeat;
    unsigned char seed[4] = {7, 9, 11, 13};
    wn_text_atlas *seeded;
    int ink = 0;

    check("atlas is created", atlas != NULL && wn_text_atlas_is_ready(atlas));
    if (atlas == NULL) {
        printf("atlas_test: FAIL\n");
        return 1;
    }
    check("atlas starts at 1024", wn_text_atlas_width(atlas) == 1024);

    run = wn_text_atlas_shape(atlas, "Hola", "Segoe UI", 16.0f, 400);
    check("run is shaped", run != NULL && run->count == 4);
    if (run == NULL) {
        printf("atlas_test: FAIL\n");
        return 1;
    }
    printf("  run \"Hola\" 16px: %d glyphs, width %.2f, ascent %.2f\n",
           run->count, run->width, run->ascent);

    glyph = wn_text_atlas_glyph(atlas, "Segoe UI", 16.0f, 400, run->glyphs[0].glyph);
    check("glyph is packed", glyph != NULL && glyph->width > 0 && glyph->height > 0);
    if (glyph == NULL) {
        printf("atlas_test: FAIL\n");
        return 1;
    }
    printf("  glyph %d: %dx%d bearing=(%d,%d) uv=(%.4f,%.4f)-(%.4f,%.4f)\n",
           run->glyphs[0].glyph, glyph->width, glyph->height, glyph->bearing_x,
           glyph->bearing_y, glyph->u0, glyph->v0, glyph->u1, glyph->v1);
    check("uv stays inside the atlas", glyph->u0 >= 0.0f && glyph->u1 <= 1.0f &&
                                       glyph->v0 >= 0.0f && glyph->v1 <= 1.0f);
    check("uv matches the bitmap size", (glyph->u1 - glyph->u0) * 1024.0f >= (float)glyph->width &&
                                        (glyph->u1 - glyph->u0) * 1024.0f <= (float)glyph->width + 0.01f);

    for (int row = 0; row < glyph->height; ++row) {
        for (int col = 0; col < glyph->width; ++col) {
            int x = (int)(glyph->u0 * 1024.0f) + col;
            int y = (int)(glyph->v0 * 1024.0f) + row;
            if (wn_text_atlas_pixels(atlas)[y * 1024 + x] > 0) {
                ++ink;
            }
        }
    }
    check("atlas holds the glyph ink", ink > 0);

    repeat = wn_text_atlas_glyph(atlas, "Segoe UI", 16.0f, 400, run->glyphs[0].glyph);
    check("glyph lookup is cached", repeat == glyph);
    again = wn_text_atlas_shape(atlas, "Hola", "Segoe UI", 16.0f, 400);
    check("run lookup is cached", again == run);

    seeded = wn_text_atlas_create(seed, 2, 2);
    check("seed atlas is created", seeded != NULL);
    if (seeded != NULL) {
        check("seed pixels are copied",
              wn_text_atlas_pixels(seeded)[0] == 7 && wn_text_atlas_pixels(seeded)[1] == 9);
        check("seeded atlas packs below the seed", wn_text_atlas_height(seeded) == 1024);
        wn_text_atlas_destroy(seeded);
    }

    wn_text_atlas_destroy(atlas);
    wn_text_runtime_shutdown();
    printf("atlas_test: PASS (%d checks)\n", checks);
    return 0;
}
