/*
 * tv_ui.h — TV UI module (tiny3d + libfont). Renders the save list, job
 * strip, and header on a connected display. Currently DISABLED at the
 * Makefile level because tiny3d's RSX init wedges the PS3 in this firmware
 * + Cobra state. Source kept here so the UI can be revived once the
 * tiny3d issue is resolved.
 *
 * To re-enable: in Makefile, add 'source/ui' to SOURCES, add -ltiny3d
 * -lfont3d -lrsx -lgcm_sys to LIBS, and #define SAVESYNC_TV_UI in main.cpp.
 */

#ifndef SAVESYNC_TV_UI_H
#define SAVESYNC_TV_UI_H

#include "savesync.h"
#include "savedata.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AppState;

/* Initializes tiny3d, viewport, texture pool, fonts. Returns 0 on success. */
int  tv_ui_init(void);

/* One frame: clear + draw background gradient, header, save list, job
 * strip, footer. Caller should call this once per main-loop iteration
 * after polling pad. */
void tv_ui_render(const struct AppState *st, int net_step);

/* Tears down tiny3d. */
void tv_ui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SAVESYNC_TV_UI_H */
