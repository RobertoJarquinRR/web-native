#include <stdio.h>
#include <string.h>

#include "text/wn_text.h"

static int checks = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

int main(void)
{
    const wn_text_ops *ops = wn_text_directwrite();
    wn_text_style regular = {"Segoe UI", 16.0f, 400};
    wn_text_style bold = {"Segoe UI", 16.0f, 700};
    wn_text_metrics first;
    wn_text_metrics second;
    wn_text_metrics other;
    wn_text_metrics empty;
    void *context = NULL;

    if (ops == NULL || ops->init == NULL || ops->measure == NULL) {
        printf("text_measure_test: FAIL (no backend)\n");
        return 1;
    }
    if (ops->init(&context) != 0) {
        printf("text_measure_test: FAIL (init)\n");
        return 1;
    }

    check("measure returns success", ops->measure(context, "Hola", &regular, 0.0f, &first) == 0);
    printf("  measured \"Hola\" 16px Segoe UI: width=%.2f height=%.2f ascent=%.2f descent=%.2f\n",
           first.width, first.height, first.ascent, first.descent);
    check("width is positive", first.width > 0.0f);
    check("height covers the font size", first.height >= 15.0f);
    check("ascent plus descent is the height", first.ascent + first.descent <= first.height + 0.01f);

    check("second measure returns success", ops->measure(context, "Hola", &regular, 0.0f, &second) == 0);
    check("second measure is identical", memcmp(&first, &second, sizeof(first)) == 0);
    check("cache recorded one miss", ops->cache_misses(context) == 1);

    check("bold measures", ops->measure(context, "Hola", &bold, 0.0f, &other) == 0);
    check("bold is a separate cache entry", ops->cache_misses(context) == 2);
    check("bold width differs from regular", other.width != first.width);

    check("empty string measures", ops->measure(context, "", &regular, 0.0f, &empty) == 0);
    check("empty string has no width", empty.width == 0.0f);

    ops->destroy(context);
    printf("text_measure_test: PASS (%d checks)\n", checks);
    return 0;
}
