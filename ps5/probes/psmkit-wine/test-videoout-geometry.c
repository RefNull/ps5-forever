/* RADV VideoOut output geometry (ps5/radv/patches/0095), compiled from the
 * header that patch adds: where each output size is drawn, and that the
 * cursor plane is drawn where Wine's click for the same pointer lands. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "wsi_common_videoout_geometry.h"

/* Title pointers and the cursor plane's space (runtime-input.c). */
#define DESKTOP_WIDTH 1920
#define DESKTOP_HEIGHT 1080

static const struct size_case
{
    const char *setting;
    unsigned width, height, scanout_width, scanout_height;
    unsigned region_width, region_height, offset_x;
    unsigned scale_milli; /* screen pixels per output pixel at 3840x2160 */
    int blit, known;
} cases[] = {
    {NULL, 3840, 2160, 3840, 2160, 3840, 2160, 0, 1000, 0, 1},
    {"", 3840, 2160, 3840, 2160, 3840, 2160, 0, 1000, 0, 1},
    {"3840x2160", 3840, 2160, 3840, 2160, 3840, 2160, 0, 1000, 0, 1},
    /* 16:9: the scanout is the output; VideoOut's scaler fills the screen. */
    {"2560x1440", 2560, 1440, 2560, 1440, 2560, 1440, 0, 1500, 0, 1},
    {"1920x1080", 1920, 1080, 1920, 1080, 1920, 1080, 0, 2000, 0, 1},
    /* 16:10: scaled into a centred 3456x2160 region of the 4K scanout. */
    {"2560x1600", 2560, 1600, 3840, 2160, 3456, 2160, 192, 1350, 1, 1},
    {"1920x1200", 1920, 1200, 3840, 2160, 3456, 2160, 192, 1800, 1, 1},
    {"1280x800", 1280, 800, 3840, 2160, 3456, 2160, 192, 2700, 1, 1},
    /* Anything else is refused: the 4K default. */
    {"640x480", 3840, 2160, 3840, 2160, 3840, 2160, 0, 1000, 0, 0},
    {"2560x1600 ", 3840, 2160, 3840, 2160, 3840, 2160, 0, 1000, 0, 0},
};

static int cursor_x(const struct videoout_geometry *g, int x, int y, int *out_y)
{
    int out_x;
    videoout_geometry_cursor(g, x, y, DESKTOP_WIDTH, DESKTOP_HEIGHT, &out_x, out_y);
    return out_x;
}

static void test_geometry(const struct size_case *c, const struct videoout_geometry *g)
{
    assert(g->extent.width == c->width && g->extent.height == c->height);
    assert(g->scanout.width == c->scanout_width && g->scanout.height == c->scanout_height);
    assert(g->region.width == c->region_width && g->region.height == c->region_height);
    assert(g->offset.x == (int)c->offset_x && g->offset.y == 0);
    assert(videoout_geometry_needs_blit(g) == c->blit);
    /* Centred, inside the scanout, and one uniform scale: no stretch. */
    assert(2 * g->offset.x + g->region.width == g->scanout.width);
    assert(g->region.height == g->scanout.height);
    assert((uint64_t)g->region.width * g->extent.height == (uint64_t)g->region.height * g->extent.width);
    /* The scale to the screen: the region's own, then VideoOut's to 4K. */
    assert((uint64_t)g->region.height * VIDEOOUT_HEIGHT * 1000 ==
           (uint64_t)c->scale_milli * g->extent.height * g->scanout.height);
}

static void test_cursor(const struct videoout_geometry *g)
{
    const int left = g->offset.x, right = g->offset.x + (int)g->region.width - 1;
    const int bottom = (int)g->region.height - 1;
    const int cell = (int)((g->region.width + DESKTOP_WIDTH - 1) / DESKTOP_WIDTH);
    int x, y;

    /* Corners and centre of the pointer's space: the image's. */
    x = cursor_x(g, 0, 0, &y);
    assert(x == left && y == 0);
    x = cursor_x(g, DESKTOP_WIDTH - 1, DESKTOP_HEIGHT - 1, &y);
    assert(x <= right && x > right - cell && y <= bottom && y > bottom - cell);
    x = cursor_x(g, DESKTOP_WIDTH / 2, DESKTOP_HEIGHT / 2, &y);
    assert(x == left + (int)g->region.width / 2 && y == (int)g->region.height / 2);
    if (g->scanout.width == VIDEOOUT_WIDTH && g->scanout.height == VIDEOOUT_HEIGHT)
        assert(x == VIDEOOUT_WIDTH / 2 && y == VIDEOOUT_HEIGHT / 2);
    /* Never in the bars beside the image, whatever the position. */
    x = cursor_x(g, -50, -50, &y);
    assert(x == left && y == 0);
    x = cursor_x(g, 5000, 5000, &y);
    assert(x == right && y == bottom);
    x = cursor_x(g, -1, DESKTOP_HEIGHT, &y);
    assert(x == left && y == bottom);

    /* Wine's title-GPU driver turns the pointer into output pixels
     * (win32u driver.c nulldrv_ProcessEvents: x * SM_CXSCREEN / 1920, with
     * SM_CXSCREEN the display mode, the output size), and the present draws
     * output pixel X at the region's X * region / output. The cursor plane
     * for the same pointer is within one output pixel of that click. */
    const int span_x = (int)((g->region.width + g->extent.width - 1) / g->extent.width);
    const int span_y = (int)((g->region.height + g->extent.height - 1) / g->extent.height);
    for (int py = 0; py < DESKTOP_HEIGHT; py += 7)
        for (int px = 0; px < DESKTOP_WIDTH; px += 13)
        {
            const int click_x = (int)((int64_t)px * g->extent.width / DESKTOP_WIDTH);
            const int click_y = (int)((int64_t)py * g->extent.height / DESKTOP_HEIGHT);
            int drawn_x, drawn_y, plane_y, plane_x = cursor_x(g, px, py, &plane_y);
            videoout_geometry_cursor(g, click_x, click_y, (int)g->extent.width,
                                     (int)g->extent.height, &drawn_x, &drawn_y);
            assert(abs(plane_x - drawn_x) <= span_x && abs(plane_y - drawn_y) <= span_y);
            assert(plane_x >= left && plane_x <= right && plane_y >= 0 && plane_y <= bottom);
        }
}

int main(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        struct videoout_geometry g;
        assert(videoout_geometry_parse(cases[i].setting, &g) == cases[i].known);
        test_geometry(&cases[i], &g);
        test_cursor(&g);
    }
    puts("test-videoout-geometry: ok");
    return 0;
}
