/*
 * icon_cache.c — PNG → RSX texture loader with LRU cache.
 *
 * libpng on the PPU is plenty fast for 320x176 ICON0.PNG files (under
 * 50 ms each). We decode into a temporary heap buffer, then memcpy into
 * an RSX-mapped texture allocation from tiny3d_AllocTexture. The host
 * buffer is freed; the RSX allocation lives forever (tiny3d's pool is
 * allocate-only, but the cache is bounded so this leaks at most
 * CACHE_MAX * ~256 KB).
 */

#include "icon_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <tiny3d.h>

#define CACHE_MAX 16
#define MAX_IMG_W 512
#define MAX_IMG_H 512

typedef struct {
    char           save_id[64];
    icon_handle_t  handle;
    int            attempted;     /* 1 once load_png has been called */
} entry_t;

static entry_t g_entries[CACHE_MAX];
static int     g_count = 0;

void icon_cache_init(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_count = 0;
}

static entry_t *find_entry(const char *save_id) {
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].save_id, save_id) == 0) return &g_entries[i];
    }
    return NULL;
}

/* Decode a PNG file into a freshly-malloc'd buffer of RGBA8 pixels (packed,
 * no padding). On success, returns the buffer and writes width/height. On
 * failure, returns NULL. The caller frees the buffer. */
static unsigned char *decode_png_rgba(const char *path,
                                       uint32_t *out_w, uint32_t *out_h) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    unsigned char header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8) != 0) {
        fclose(fp);
        return NULL;
    }

    png_structp png_ptr  = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                                   NULL, NULL, NULL);
    if (!png_ptr) { fclose(fp); return NULL; }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp);
        return NULL;
    }

    unsigned char *pixels = NULL;
    png_bytep    *rows   = NULL;
    if (setjmp(png_jmpbuf(png_ptr))) {
        free(pixels);
        free(rows);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 w, h;
    int bit_depth, color_type;
    png_get_IHDR(png_ptr, info_ptr, &w, &h, &bit_depth, &color_type,
                 NULL, NULL, NULL);

    if (w == 0 || h == 0 || w > MAX_IMG_W || h > MAX_IMG_H) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return NULL;
    }

    /* Force 8 bits per channel, RGBA. */
    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);

    /* libpng default emits in R,G,B,A byte order; tiny3d's
     * TINY3D_TEX_FORMAT_A8R8G8B8 also expects ARGB packed in big-endian on
     * PPC — but in memory it's the same byte sequence as RGBA via the
     * packed-pixel convention RSX uses. We swap R and B at decode time
     * via PNG_TRANSFORM_BGR? No — instead use png_set_bgr() to match the
     * expected on-wire byte order. Easier: keep RGBA from libpng, then
     * swap R<->B during the copy. */

    png_read_update_info(png_ptr, info_ptr);

    size_t row_bytes = png_get_rowbytes(png_ptr, info_ptr);
    if (row_bytes != (size_t)w * 4) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return NULL;
    }

    pixels = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    rows   = (png_bytep *)malloc((size_t)h * sizeof(png_bytep));
    if (!pixels || !rows) {
        free(pixels);
        free(rows);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return NULL;
    }
    for (uint32_t i = 0; i < h; i++)
        rows[i] = pixels + (size_t)i * row_bytes;

    png_read_image(png_ptr, rows);
    png_read_end(png_ptr, NULL);

    free(rows);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    return pixels;
}

/* Copy RGBA bytes into RSX memory as A8R8G8B8 (32-bit big-endian word per
 * pixel: 0xAARRGGBB). PPC big-endian write of 0xAARRGGBB stores the bytes
 * AA RR GG BB in that order. libpng's RGBA gives us R G B A in memory, so
 * we shuffle each pixel into the right word. */
static void copy_rgba_to_argb(const unsigned char *src,
                               unsigned char *dst,
                               uint32_t pixels) {
    uint32_t *dp = (uint32_t *)dst;
    for (uint32_t i = 0; i < pixels; i++) {
        unsigned char r = src[i * 4 + 0];
        unsigned char g = src[i * 4 + 1];
        unsigned char b = src[i * 4 + 2];
        unsigned char a = src[i * 4 + 3];
        dp[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                ((uint32_t)g <<  8) |  (uint32_t)b;
    }
}

icon_handle_t icon_cache_get(const char *save_id, const char *icon_path) {
    icon_handle_t empty;
    memset(&empty, 0, sizeof(empty));

    entry_t *e = find_entry(save_id);
    if (e) return e->handle;          /* cached (loaded or tried+failed) */

    if (g_count >= CACHE_MAX) return empty;  /* cache full */

    e = &g_entries[g_count++];
    snprintf(e->save_id, sizeof(e->save_id), "%s", save_id);
    e->attempted = 1;
    e->handle    = empty;

    uint32_t w = 0, h = 0;
    unsigned char *pixels = decode_png_rgba(icon_path, &w, &h);
    if (!pixels) return empty;

    size_t bytes = (size_t)w * (size_t)h * 4;
    unsigned char *rsx = (unsigned char *)tiny3d_AllocTexture(bytes);
    if (!rsx) {
        free(pixels);
        return empty;
    }
    copy_rgba_to_argb(pixels, rsx, w * h);
    free(pixels);

    e->handle.rsx_offset = tiny3d_TextureOffset(rsx);
    e->handle.width      = w;
    e->handle.height     = h;
    e->handle.stride     = w * 4;
    e->handle.ok         = 1;
    return e->handle;
}
