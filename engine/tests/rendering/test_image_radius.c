/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 64
#define FB_H 64

/* The node every scene draws: placed off the origin so the box, not the framebuffer, sets the corners. */
#define BOX_X 6
#define BOX_Y 5
#define BOX_W 40
#define BOX_H 30

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test framebuffer composited source-over from every backend call.
 */
typedef struct
{
    uint32_t fb[FB_W * FB_H]; /**< Premultiplied ARGB8888 pixel storage. */
    int fmt_calls;            /**< copy_rect_fmt calls received. */
} TestCtx;

/**
 * @brief One way of drawing the image, each reaching a different path of the image scaler.
 */
typedef struct
{
    const char* what;  /**< Label for failure messages. */
    const char* image; /**< Registered asset name. */
    bool fmt_backend;  /**< Whether the backend offers copy_rect_fmt. */
    uint8_t resize;    /**< ERResizeMode. */
    int16_t padding;   /**< Node padding; the bitmap goes in the content box, the clip stays on the border box. */
} DrawCase;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static TestCtx s_tc;

/* Opaque white sources: 1:1 for the box in both formats, and a small one the box scales up. */
static uint32_t s_argb_box[BOX_W * BOX_H];
static uint16_t s_rgb565_box[BOX_W * BOX_H];
static uint32_t s_argb_small[8 * 8];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Composites one premultiplied pixel source-over into the framebuffer.
 *
 * @param[in] x  Column.
 * @param[in] y  Row.
 * @param[in] s  Premultiplied ARGB8888 source pixel.
 */
static void over(int x, int y, uint32_t s)
{
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H)
        return;
    uint32_t* d = &s_tc.fb[y * FB_W + x];
    const uint32_t inv = 255u - (s >> 24);
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8)
    {
        const uint32_t c = ((s >> sh) & 0xFFu) + (((*d >> sh) & 0xFFu) * inv + 127u) / 255u;
        out |= (c > 255u ? 255u : c) << sh;
    }
    *d = out;
}

/**
 * @brief fill_rect stub — premultiplies the straight-alpha colour and composites it.
 *
 * @param[in] argb  Fill color (straight-alpha ARGB8888).
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Unused.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFu;
    const uint32_t r = ((argb >> 16) & 0xFFu) * a / 255u;
    const uint32_t g = ((argb >> 8) & 0xFFu) * a / 255u;
    const uint32_t b = (argb & 0xFFu) * a / 255u;
    const uint32_t pm = (a << 24) | (r << 16) | (g << 8) | b;
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            over(px, py, pm);
}

/**
 * @brief copy_rect stub — replaces the destination pixels.
 *
 * @param[in] src     Premultiplied ARGB8888 source.
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Unused.
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
            if (x + col >= 0 && x + col < FB_W && y + row >= 0 && y + row < FB_H)
                s_tc.fb[(y + row) * FB_W + x + col] = s[col];
    }
}

/**
 * @brief blend_rect stub — composites premultiplied rows source-over.
 *
 * @param[in] src     Premultiplied ARGB8888 source.
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global opacity (always 255 here).
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Unused.
 */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)alpha;
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
            over(x + col, y + row, s[col]);
    }
}

/**
 * @brief copy_rect_fmt stub — writes ARGB rows verbatim and expands RGB565 ones, as a DMA2D blitter would.
 *
 * @param[in] src     Source rows in @p fmt.
 * @param[in] stride  Source row stride in bytes.
 * @param[in] fmt     Source pixel format.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Unused.
 */
static void copy_fmt_cb(const void* src, int stride, ERImageFormat fmt, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    s_tc.fmt_calls++;
    for (int row = 0; row < h; row++)
    {
        const uint8_t* s = (const uint8_t*)src + (size_t)row * (size_t)stride;
        for (int col = 0; col < w; col++)
        {
            uint32_t p;
            if (fmt == ER_IMG_RGB565)
            {
                const uint16_t v = ((const uint16_t*)s)[col];
                const uint32_t r5 = (v >> 11) & 0x1Fu, g6 = (v >> 5) & 0x3Fu, b5 = v & 0x1Fu;
                p = 0xFF000000u | (((r5 << 3) | (r5 >> 2)) << 16) | (((g6 << 2) | (g6 >> 4)) << 8)
                    | ((b5 << 3) | (b5 >> 2));
            }
            else
                p = ((const uint32_t*)s)[col];
            if (x + col >= 0 && x + col < FB_W && y + row >= 0 && y + row < FB_H)
                s_tc.fb[(y + row) * FB_W + x + col] = p;
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] msg  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Installs the test backend, with or without copy_rect_fmt, and registers the test images.
 *
 * Installing a backend resets the image registry, so the images are registered after it.
 *
 * @param[in] with_fmt  Whether to offer copy_rect_fmt.
 */
static void set_backend(bool with_fmt)
{
    static EmbeddedRenderBackend be;
    memset(&be, 0, sizeof(be));
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    be.copy_rect_fmt = with_fmt ? copy_fmt_cb : NULL;
    be.ctx = &s_tc;
    embedded_renderer_set_backend(&be);
    er_image_load("argb_box", s_argb_box, BOX_W, BOX_H);
    er_image_load_rgb565("rgb565_box", s_rgb565_box, BOX_W, BOX_H);
    er_image_load("argb_small", s_argb_small, 8, 8);
}

/**
 * @brief Returns an ERProps with all int16_t layout fields set to ER_LAYOUT_AUTO.
 *
 * @return ERProps with all layout sentinels initialised and opacity set to 255.
 */
static ERProps props_default(void)
{
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

/**
 * @brief Renders one node at the test box over a transparent root and returns the framebuffer's coverage.
 *
 * @param[in]  type   ER_NODE_VIEW (the reference) or ER_NODE_IMAGE.
 * @param[in]  shape  Props carrying the radii (and, for an image, its source); position and size are set here.
 * @param[out] cov    Receives each pixel's premultiplied alpha.
 */
static void render_coverage(ERNodeType type, const ERProps* shape, uint8_t cov[FB_W * FB_H])
{
    er_reset();
    memset(s_tc.fb, 0, sizeof(s_tc.fb));
    s_tc.fmt_calls = 0;

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = FB_W;
    rp.height = FB_H;
    er_node_set_props(root, &rp);

    ERNode* n = er_node_create(type);
    ERProps p = *shape;
    p.position = ER_POS_ABSOLUTE;
    p.left = BOX_X;
    p.top = BOX_Y;
    p.width = BOX_W;
    p.height = BOX_H;
    er_node_set_props(n, &p);

    er_tree_append_child(root, n);
    er_tree_set_root(root);
    er_commit();

    for (int i = 0; i < FB_W * FB_H; i++)
        cov[i] = (uint8_t)(s_tc.fb[i] >> 24);

    er_tree_remove_child(root, n);
    er_node_destroy(n);
    er_node_destroy(root);
}

/**
 * @brief Compares two coverage masks over a rect, reporting the first difference.
 *
 * @param[in] want  Reference coverage.
 * @param[in] got   Coverage under test.
 * @param[in] x0    Left edge of the compared rect.
 * @param[in] y0    Top edge.
 * @param[in] x1    One past the right edge.
 * @param[in] y1    One past the bottom edge.
 * @param[in] what  Label for the failure message.
 *
 * @return true when every pixel matches.
 */
static bool same_coverage(const uint8_t* want, const uint8_t* got, int x0, int y0, int x1, int y1, const char* what)
{
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            if (want[y * FB_W + x] != got[y * FB_W + x])
            {
                fprintf(stderr,
                        "  %s at (%d,%d): background alpha %u, image alpha %u\n",
                        what,
                        x,
                        y,
                        want[y * FB_W + x],
                        got[y * FB_W + x]);
                return false;
            }
        }
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests that an Image is clipped to its corner radii.
 *
 * A rounded image has to stop at exactly the edge a background with the same radii fills, anti-aliased
 * fringe included. Each case renders an opaque white image and a white rounded View at the same box and
 * requires their coverage to match pixel for pixel, through every path of the image scaler: the ARGB and
 * RGB565 1:1 transfers (with and without copy_rect_fmt), the scaled row path, per-corner radii, and an
 * image inset by padding, which is clipped to the border box rather than to its content box.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    for (int i = 0; i < BOX_W * BOX_H; i++)
    {
        s_argb_box[i] = 0xFFFFFFFFu;
        s_rgb565_box[i] = 0xFFFFu;
    }
    for (int i = 0; i < 8 * 8; i++)
        s_argb_small[i] = 0xFFFFFFFFu;

    static uint8_t want[FB_W * FB_H];
    static uint8_t got[FB_W * FB_H];

    /* -----------------------------------------------------------------------
     * Uniform radius: every path of the scaler matches the background fill.
     * ---------------------------------------------------------------------- */
    static const DrawCase k_cases[] = {
        {"ARGB 1:1", "argb_box", true, ER_RESIZE_STRETCH, 0},
        {"ARGB 1:1 without copy_rect_fmt", "argb_box", false, ER_RESIZE_STRETCH, 0},
        {"RGB565 1:1", "rgb565_box", true, ER_RESIZE_STRETCH, 0},
        {"RGB565 1:1 without copy_rect_fmt", "rgb565_box", false, ER_RESIZE_STRETCH, 0},
        {"scaled", "argb_small", true, ER_RESIZE_STRETCH, 0},
        {"cover", "argb_small", true, ER_RESIZE_COVER, 0},
    };
    for (size_t k = 0; k < sizeof(k_cases) / sizeof(k_cases[0]); k++)
    {
        set_backend(k_cases[k].fmt_backend);

        ERProps shape = props_default();
        shape.border_radius = 12;
        shape.background_color = 0xFFFFFFFFu;
        render_coverage(ER_NODE_VIEW, &shape, want);

        if (want[BOX_Y * FB_W + BOX_X] != 0)
            return fail("the background reference did not round its own corner");

        shape = props_default();
        shape.border_radius = 12;
        strncpy(shape.image_name, k_cases[k].image, ER_IMAGE_NAME_MAX);
        shape.resize_mode = k_cases[k].resize;
        render_coverage(ER_NODE_IMAGE, &shape, got);

        if (!same_coverage(want, got, 0, 0, FB_W, FB_H, k_cases[k].what))
            return fail("a rounded image's coverage differs from a rounded background's");

        /* At 1:1 the rows between the arcs still reach the backend as one whole-rect transfer. */
        if (k_cases[k].fmt_backend && strcmp(k_cases[k].image, "argb_small") != 0 && s_tc.fmt_calls != 1)
            return fail("a rounded 1:1 image no longer sends its straight rows as one copy_rect_fmt call");
    }

    /* -----------------------------------------------------------------------
     * Per-corner radii: each corner is cut by its own radius, and a 0 corner stays square.
     * ---------------------------------------------------------------------- */
    set_backend(true);
    {
        ERProps shape = props_default();
        shape.border_top_left_radius = 14;
        shape.border_bottom_right_radius = 6;
        shape.background_color = 0xFFFFFFFFu;
        render_coverage(ER_NODE_VIEW, &shape, want);

        shape = props_default();
        shape.border_top_left_radius = 14;
        shape.border_bottom_right_radius = 6;
        strncpy(shape.image_name, "argb_box", ER_IMAGE_NAME_MAX);
        render_coverage(ER_NODE_IMAGE, &shape, got);

        if (got[BOX_Y * FB_W + BOX_X] != 0)
            return fail("per-corner radius: the rounded top-left corner still painted");
        if (got[BOX_Y * FB_W + BOX_X + BOX_W - 1] != 0xFF)
            return fail("per-corner radius: the square top-right corner was clipped");
        if (!same_coverage(want, got, 0, 0, FB_W, FB_H, "per-corner radii"))
            return fail("per-corner radius: the image's coverage differs from the background's");
    }

    /* -----------------------------------------------------------------------
     * Padding: the bitmap sits in the content box, but the corners are the border box's.
     * ---------------------------------------------------------------------- */
    {
        const int pad = 2;
        ERProps shape = props_default();
        shape.border_radius = 12;
        shape.background_color = 0xFFFFFFFFu;
        render_coverage(ER_NODE_VIEW, &shape, want);

        shape = props_default();
        shape.border_radius = 12;
        shape.padding = (int16_t)pad;
        strncpy(shape.image_name, "argb_small", ER_IMAGE_NAME_MAX);
        render_coverage(ER_NODE_IMAGE, &shape, got);

        if (got[(BOX_Y + pad) * FB_W + BOX_X + pad] != 0)
            return fail("padding: the content box's corner pixel is outside the border box's arc yet painted");
        if (!same_coverage(
                want, got, BOX_X + pad, BOX_Y + pad, BOX_X + BOX_W - pad, BOX_Y + BOX_H - pad, "padded image"))
            return fail("padding: the image is not clipped to the border box's rounded silhouette");
    }

    /* -----------------------------------------------------------------------
     * No radius: the bitmap still covers its whole box, square corners included.
     * ---------------------------------------------------------------------- */
    {
        ERProps shape = props_default();
        strncpy(shape.image_name, "argb_box", ER_IMAGE_NAME_MAX);
        render_coverage(ER_NODE_IMAGE, &shape, got);
        for (int y = 0; y < FB_H; y++)
            for (int x = 0; x < FB_W; x++)
            {
                const bool inside = x >= BOX_X && x < BOX_X + BOX_W && y >= BOX_Y && y < BOX_Y + BOX_H;
                if (got[y * FB_W + x] != (inside ? 0xFF : 0x00))
                    return fail("no radius: a square image does not cover exactly its box");
            }
    }

    return EXIT_SUCCESS;
}
