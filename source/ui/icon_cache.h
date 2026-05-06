/*
 * icon_cache — load PS3 save ICON0.PNG files into RSX textures.
 *
 * The cache is a tiny LRU keyed by save dir_name. Each hit gives back a
 * tiny3d-ready texture handle (RSX offset + dimensions) the renderer can
 * bind via tiny3d_SetTexture. PNG decode happens on the PPU via libpng;
 * the decoded pixels are copied directly into RSX-mapped memory pulled
 * from tiny3d_AllocTexture.
 *
 * Design: textures are never freed — tiny3d's pool is allocate-only.
 * That's fine because we cap the number of cached icons (CACHE_MAX) and
 * skip caching once full. Saves whose icon fails to load are recorded as
 * "tried, failed" so we don't retry every frame.
 */

#ifndef SAVESYNC_ICON_CACHE_H
#define SAVESYNC_ICON_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rsx_offset;
    uint32_t width;
    uint32_t height;
    uint32_t stride;          /* in bytes, == width * 4 */
    int      ok;              /* 1 if loaded; 0 if not yet attempted or failed */
} icon_handle_t;

void icon_cache_init(void);

/* Look up by save_id (dir_name). If not yet cached, attempt to load
 * <icon_path> from disk on this call. Returns an icon_handle_t — caller
 * checks .ok before binding. The path is only consulted on the first
 * call for a given save_id; subsequent calls return the cached entry. */
icon_handle_t icon_cache_get(const char *save_id, const char *icon_path);

#ifdef __cplusplus
}
#endif

#endif
