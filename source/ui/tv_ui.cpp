/*
 * tv_ui.cpp — TV UI for savesync, built on tiny3d.
 *
 * Layout (1920x1080 virtual):
 *   0-72     breadcrumb     ("SaveSync · 4 games · 6 saves" or "‹ Games · …")
 *   80-752   list           (8 rows × 84 px)
 *   760-790  list footer    (page indicator when overflowing)
 *   812-1000 jobs strip
 *   1000-1080 footer button hints
 *
 * Two screens — GAMES (one row per title_id) and SLOTS (slots inside the
 * active game). State lives in AppState (app_state.h); this module only
 * reads it.
 */

#include "tv_ui.h"
#include "icon_cache.h"
#include "app_state.h"

#include <stdio.h>
#include <string.h>

#include <sys/systime.h>
#include <sysutil/video.h>

#include <tiny3d.h>
#include <libfont.h>

#include <ps3ui_text.h>
#include <ps3ui_image.h>
#include "inter.h"

extern "C" {
extern const unsigned char cross_png[];    extern const unsigned int cross_png_size;
extern const unsigned char square_png[];   extern const unsigned int square_png_size;
extern const unsigned char triangle_png[]; extern const unsigned int triangle_png_size;
extern const unsigned char circle_png[];   extern const unsigned int circle_png_size;
extern const unsigned char L1_png[];       extern const unsigned int L1_png_size;
extern const unsigned char R1_png[];       extern const unsigned int R1_png_size;

extern const unsigned char ps3_png[];      extern const unsigned int ps3_png_size;
extern const unsigned char rpcs3_png[];    extern const unsigned int rpcs3_png_size;
}

static ps3ui_image_t g_btn_cross    = {};
static ps3ui_image_t g_btn_square   = {};
static ps3ui_image_t g_btn_triangle = {};
static ps3ui_image_t g_btn_circle   = {};
static ps3ui_image_t g_btn_l1       = {};
static ps3ui_image_t g_btn_r1       = {};
static ps3ui_image_t g_logo_ps3     = {};
static ps3ui_image_t g_logo_rpcs3   = {};

#include "savesync.h"
#include "savedata.h"
#include "convert.h"
#include "jobs.h"

extern "C" {
extern unsigned char font[];
extern unsigned char msx[];
}

static int g_ttf_ready = 0;

namespace {

/* ----- Theme ------------------------------------------------------------ */

constexpr uint32_t COL_BG_TOP    = 0x0B0F18FFu;
constexpr uint32_t COL_BG_BOTTOM = 0x161B26FFu;
constexpr uint32_t COL_PANEL     = 0x1B2230AAu;
constexpr uint32_t COL_PANEL_HI  = 0x2A3346FFu;
constexpr uint32_t COL_ACCENT    = 0x4FC3F7FFu;
constexpr uint32_t COL_ACCENT_DIM= 0x4FC3F744u;
constexpr uint32_t COL_TEXT      = 0xECEFF4FFu;
constexpr uint32_t COL_TEXT_DIM  = 0xB6BFD0FFu;
constexpr uint32_t COL_DIM       = 0x7B8499FFu;
constexpr uint32_t COL_OK        = 0x6FCF97FFu;
constexpr uint32_t COL_ERR       = 0xEB5757FFu;
constexpr uint32_t COL_DIVIDER   = 0xFFFFFF18u;
constexpr uint32_t COL_PROGBAR_BG= 0x2A3346FFu;

/* Layout */
constexpr float LIST_X       = 64.0f;
constexpr float LIST_Y       = 80.0f;
constexpr float LIST_W       = 1792.0f;
constexpr float ROW_H        = 84.0f;
constexpr float ICON_W       = 128.0f;
constexpr float ICON_H       = 72.0f;
/* List ends at LIST_Y + 8*ROW_H = 80 + 672 = 752. Page indicator y=760.
 * Jobs strip divider at y=812 — clear ~52 px gap. */
constexpr int   VISIBLE_ROWS = 8;

constexpr int FT_BREADCRUMB  = 30;
constexpr int FT_ROW_TITLE   = 32;
constexpr int FT_ROW_META    = 22;
constexpr int FT_SECTION     = 28;
constexpr int FT_JOB         = 24;
constexpr int FT_FOOTER      = 22;
constexpr int FT_PLACEHOLDER = 20;
constexpr int FT_EMPTY_TITLE = 36;
constexpr int FT_EMPTY_HINT  = 24;

/* ----- Drawing primitives ----------------------------------------------- */

void draw_quad(float x, float y, float w, float h, uint32_t rgba) {
	tiny3d_SetPolygon(TINY3D_QUADS);
	tiny3d_VertexPos(x,     y,     65535); tiny3d_VertexColor(rgba);
	tiny3d_VertexPos(x + w, y,     65535);
	tiny3d_VertexPos(x + w, y + h, 65535);
	tiny3d_VertexPos(x,     y + h, 65535);
	tiny3d_End();
}

void draw_quad_v(float x, float y, float w, float h,
                 uint32_t top, uint32_t bottom) {
	tiny3d_SetPolygon(TINY3D_QUADS);
	tiny3d_VertexPos(x,     y,     65535); tiny3d_VertexColor(top);
	tiny3d_VertexPos(x + w, y,     65535);
	tiny3d_VertexPos(x + w, y + h, 65535); tiny3d_VertexColor(bottom);
	tiny3d_VertexPos(x,     y + h, 65535);
	tiny3d_End();
}

void draw_textured_quad(float x, float y, float w, float h,
                         const icon_handle_t &h_icon) {
	tiny3d_SetTextureWrap(0, h_icon.rsx_offset, h_icon.width, h_icon.height,
	                     h_icon.stride, TINY3D_TEX_FORMAT_A8R8G8B8,
	                     TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
	tiny3d_SetPolygon(TINY3D_QUADS);
	tiny3d_VertexPos(x,     y,     65535); tiny3d_VertexColor(0xFFFFFFFFu);
	tiny3d_VertexTexture(0.0f, 0.0f);
	tiny3d_VertexPos(x + w, y,     65535);
	tiny3d_VertexTexture(1.0f, 0.0f);
	tiny3d_VertexPos(x + w, y + h, 65535);
	tiny3d_VertexTexture(1.0f, 1.0f);
	tiny3d_VertexPos(x,     y + h, 65535);
	tiny3d_VertexTexture(0.0f, 1.0f);
	tiny3d_End();
}

/* ----- Helpers ---------------------------------------------------------- */

const char *flavor_str(savesync_flavor_t f)     { return savesync_flavor_str(f); }
const char *location_str(savesync_location_t l) { return savesync_location_str(l); }

const char *job_phase_str(savesync_phase_t p) {
	switch (p) {
		case SAVESYNC_PHASE_PREPARE:        return "prep";
		case SAVESYNC_PHASE_COPY:           return "copy";
		case SAVESYNC_PHASE_TRANSFORM_SFO:  return "sfo";
		case SAVESYNC_PHASE_SIGN_PFD:       return "sign";
		case SAVESYNC_PHASE_ARCHIVE:        return "zip";
		case SAVESYNC_PHASE_DONE:           return "done";
		case SAVESYNC_PHASE_FAILED:         return "fail";
		default:                            return "...";
	}
}

const char *job_state_str(savesync_job_state_t s) {
	switch (s) {
		case SAVESYNC_JOB_PENDING:   return "pending";
		case SAVESYNC_JOB_RUNNING:   return "running";
		case SAVESYNC_JOB_DONE:      return "done";
		case SAVESYNC_JOB_FAILED:    return "failed";
		case SAVESYNC_JOB_CANCELLED: return "cancel";
		default:                     return "?";
	}
}

uint32_t state_color(savesync_job_state_t s) {
	switch (s) {
		case SAVESYNC_JOB_DONE:    return COL_OK;
		case SAVESYNC_JOB_FAILED:  return COL_ERR;
		case SAVESYNC_JOB_RUNNING: return COL_ACCENT;
		default:                   return COL_DIM;
	}
}

void format_size(uint64_t bytes, char *out, size_t out_len) {
	const char *units[] = { "B", "KB", "MB", "GB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
	if (u == 0) snprintf(out, out_len, "%llu %s", (unsigned long long)bytes, units[u]);
	else        snprintf(out, out_len, "%.1f %s", v, units[u]);
}

/* ----- Sections --------------------------------------------------------- */

void draw_background_gradient() {
	draw_quad_v(0, 0, 1920, 1080, COL_BG_TOP, COL_BG_BOTTOM);
}

void draw_breadcrumb(const AppState *st) {
	const int y = 24;

	if (st->screen == TV_SCREEN_GAMES) {
		int x = ps3ui_text_draw(64, y, COL_ACCENT, FT_BREADCRUMB, "SaveSync");
		x = ps3ui_text_draw(x, y, COL_DIM,  FT_BREADCRUMB,
			"   \xE2\x80\xA2   ");
		x = ps3ui_text_drawf(x, y, COL_TEXT_DIM, FT_BREADCRUMB,
			"%d %s", st->games_count,
			st->games_count == 1 ? "game" : "games");
		x = ps3ui_text_draw(x, y, COL_DIM,  FT_BREADCRUMB,
			"   \xE2\x80\xA2   ");
		ps3ui_text_drawf(x, y, COL_TEXT_DIM, FT_BREADCRUMB,
			"%d %s", (int)st->saves.count,
			st->saves.count == 1 ? "save" : "saves");
	} else {
		/* Slots view: [○] Games  ›  <Title> */
		const int icon_size = FT_BREADCRUMB + 4;
		ps3ui_image_draw_inline(&g_btn_circle, 64, y, FT_BREADCRUMB,
		                        icon_size, icon_size, PS3UI_VALIGN_TEXT_CENTER);
		int x = 64 + icon_size + 10;
		x = ps3ui_text_draw(x, y, COL_TEXT_DIM, FT_BREADCRUMB, "Games");
		x = ps3ui_text_draw(x, y, COL_DIM, FT_BREADCRUMB, "   \xE2\x80\xBA   ");

		/* Game title: pull from active game's first slot. */
		const char *title = st->active_title_id;
		int g = tv_state_find_game(st, st->active_title_id);
		if (g >= 0) {
			int idx = st->games[g].first_save_idx;
			if (idx >= 0 && idx < (int)st->saves.count) {
				const savesync_save_t *s = &st->saves.items[idx];
				if (s->title[0]) title = s->title;
			}
		}
		ps3ui_text_drawf(x, y, COL_TEXT, FT_BREADCRUMB, "%.60s", title);
	}

	draw_quad(64, 72, 1792, 1, COL_DIVIDER);
}

void draw_icon_thumb(float icon_x, float icon_y, float icon_w, float icon_h,
                     const savesync_save_t *s) {
	char icon_path[SAVESYNC_PATH_LEN + 16];
	snprintf(icon_path, sizeof(icon_path), "%s/ICON0.PNG", s->path);

	icon_handle_t h = { 0, 0, 0, 0, 0 };
	if (s->has_icon0) h = icon_cache_get(s->dir_name, icon_path);

	if (h.ok) {
		draw_textured_quad(icon_x, icon_y, icon_w, icon_h, h);
	} else {
		draw_quad(icon_x, icon_y, icon_w, icon_h, COL_PANEL);
		draw_quad(icon_x, icon_y + icon_h - 2, icon_w, 2, COL_ACCENT_DIM);
		ps3ui_text_drawf((int)icon_x + 12, (int)icon_y + 22, COL_TEXT_DIM,
			FT_PLACEHOLDER, "%.10s", s->title_id);
	}
}

/* Row backdrop + accent stripe for the highlighted row. */
void draw_row_highlight(float row_y) {
	draw_quad(LIST_X - 8, row_y, LIST_W + 16, ROW_H, COL_PANEL_HI);
	draw_quad(LIST_X - 8, row_y, 4,           ROW_H, COL_ACCENT);
}

void draw_game_row(const tv_game_entry_t *g, const savesync_save_t *anchor,
                   float row_y, bool selected) {
	if (selected) draw_row_highlight(row_y);

	float icon_x = LIST_X + 4;
	float icon_y = row_y + (ROW_H - ICON_H) / 2.0f;
	if (anchor) {
		draw_icon_thumb(icon_x, icon_y, ICON_W, ICON_H, anchor);
	} else {
		draw_quad(icon_x, icon_y, ICON_W, ICON_H, COL_PANEL);
	}

	int text_x = (int)(icon_x + ICON_W + 18);
	uint32_t meta_color = selected ? COL_TEXT_DIM : COL_DIM;

	int after_id = ps3ui_text_drawf(text_x, (int)row_y + 8, COL_ACCENT,
		FT_ROW_TITLE, "%-9s", g->title_id);
	const char *t = (anchor && anchor->title[0]) ? anchor->title : g->title_id;
	ps3ui_text_drawf(after_id + 16, (int)row_y + 8, COL_TEXT, FT_ROW_TITLE,
		"%.50s", t);

	ps3ui_text_drawf(text_x, (int)row_y + 50, meta_color, FT_ROW_META,
		"%d %s", g->slot_count, g->slot_count == 1 ? "slot" : "slots");
}

void draw_slot_row(const savesync_save_t *s, float row_y, bool selected) {
	if (selected) draw_row_highlight(row_y);

	float icon_x = LIST_X + 4;
	float icon_y = row_y + (ROW_H - ICON_H) / 2.0f;
	draw_icon_thumb(icon_x, icon_y, ICON_W, ICON_H, s);

	int text_x = (int)(icon_x + ICON_W + 18);
	uint32_t meta_color = selected ? COL_TEXT_DIM : COL_DIM;

	const char *primary = (s->subtitle[0]) ? s->subtitle : s->dir_name;
	ps3ui_text_drawf(text_x, (int)row_y + 8, COL_TEXT, FT_ROW_TITLE,
		"%.60s", primary);

	char size_buf[32];
	format_size(s->total_size_bytes, size_buf, sizeof(size_buf));

	ps3ui_text_drawf(text_x, (int)row_y + 50, meta_color, FT_ROW_META,
		"%s  \xE2\x80\xA2  %s  \xE2\x80\xA2  %s  \xE2\x80\xA2  %u files  \xE2\x80\xA2  %s",
		s->dir_name, flavor_str(s->flavor), location_str(s->location),
		(unsigned)s->file_count, size_buf);
}

/* Page indicator under the list — "1-8 of 23" + arrows for overflow. */
void draw_page_indicator(int scroll, int visible, int total) {
	if (total <= visible) return;
	int start = scroll + 1;
	int end   = scroll + visible;
	if (end > total) end = total;

	const char *up   = (scroll > 0)              ? "\xE2\x96\xB2 more above   " : "";
	const char *down = (end < total)             ? "   \xE2\x96\xBC more below" : "";
	int y = (int)(LIST_Y + VISIBLE_ROWS * ROW_H + 8);
	ps3ui_text_drawf((int)LIST_X, y, COL_DIM, FT_FOOTER,
		"%s%d-%d of %d%s", up, start, end, total, down);
}

/* Centered two-line empty state inside the list area. */
void draw_empty_state(const char *title, const char *hint) {
	const float center_y = LIST_Y + (VISIBLE_ROWS * ROW_H) / 2.0f - 30.0f;
	const int title_y = (int)center_y;
	const int hint_y  = title_y + FT_EMPTY_TITLE + 12;

	int title_w = ps3ui_text_width(title, FT_EMPTY_TITLE);
	int hint_w  = ps3ui_text_width(hint,  FT_EMPTY_HINT);

	ps3ui_text_draw((1920 - title_w) / 2, title_y, COL_TEXT_DIM,
	                FT_EMPTY_TITLE, title);
	ps3ui_text_draw((1920 - hint_w)  / 2, hint_y,  COL_DIM,
	                FT_EMPTY_HINT, hint);
}

void draw_games_view(const AppState *st) {
	if (st->games_count == 0) {
		draw_empty_state("No saves found", "Press \xE2\x96\xA1 to rescan");
		return;
	}

	int end = st->games_scroll + VISIBLE_ROWS;
	if (end > st->games_count) end = st->games_count;

	for (int i = st->games_scroll; i < end; i++) {
		const tv_game_entry_t *g = &st->games[i];
		const savesync_save_t *anchor = nullptr;
		if (g->first_save_idx >= 0 && g->first_save_idx < (int)st->saves.count) {
			anchor = &st->saves.items[g->first_save_idx];
		}
		float y = LIST_Y + (i - st->games_scroll) * ROW_H;
		draw_game_row(g, anchor, y, i == st->games_selected);
	}

	draw_page_indicator(st->games_scroll, VISIBLE_ROWS, st->games_count);
}

void draw_slots_view(const AppState *st) {
	if (st->slot_count == 0) {
		draw_empty_state("This game has no slots anymore",
		                 "Press \xE2\x97\xAF to go back");
		return;
	}

	int end = st->slots_scroll + VISIBLE_ROWS;
	if (end > st->slot_count) end = st->slot_count;

	for (int i = st->slots_scroll; i < end; i++) {
		int idx = st->slot_save_idx[i];
		if (idx < 0 || idx >= (int)st->saves.count) continue;
		float y = LIST_Y + (i - st->slots_scroll) * ROW_H;
		draw_slot_row(&st->saves.items[idx], y, i == st->slots_selected);
	}

	draw_page_indicator(st->slots_scroll, VISIBLE_ROWS, st->slot_count);
}

void draw_jobs_strip() {
	/* Static — sizeof(savesync_job_t)*16 is ~40 KB; on the stack it blew
	 * the PPU main thread's stack mid-frame. See CLAUDE.md stack-budget. */
	static savesync_job_t snap[SAVESYNC_MAX_JOBS];
	size_t n = savesync_jobs_snapshot(snap, SAVESYNC_MAX_JOBS);

	int y = 820;
	draw_quad(64, (float)(y - 8), 1792, 1, COL_DIVIDER);

	ps3ui_text_drawf(64, y, COL_TEXT, FT_SECTION,
		"ACTIVE JOBS  (%u)", (unsigned)n);

	if (n == 0) {
		ps3ui_text_draw(64, y + 36, COL_DIM, FT_FOOTER, "no jobs running");
		return;
	}

	size_t shown = n > 4 ? 4 : n;
	for (size_t i = 0; i < shown; i++) {
		const savesync_job_t *j = &snap[i];
		int jy = y + 36 + (int)i * 36;

		uint32_t pct = (j->state == SAVESYNC_JOB_DONE)
			? 100u
			: ((j->progress.bytes_total > 0)
				? (uint32_t)((j->progress.bytes_done * 100) /
				             j->progress.bytes_total)
				: 0);

		uint32_t sc = state_color(j->state);
		ps3ui_text_drawf(64, jy, sc, FT_JOB,
			"[%-7s]", job_state_str(j->state));
		ps3ui_text_drawf(220, jy, COL_TEXT, FT_JOB,
			"%.50s  \xE2\x80\xA2  %s",
			j->label, job_phase_str(j->progress.phase));
		ps3ui_text_drawf(1220, jy, COL_TEXT_DIM, FT_JOB, "%3u%%", pct);

		float bar_x = 1340.0f, bar_y = (float)jy + 12.0f;
		float bar_w = 480.0f,  bar_h = 8.0f;
		draw_quad(bar_x, bar_y, bar_w, bar_h, COL_PROGBAR_BG);
		float fill = bar_w * (float)pct / 100.0f;
		if (fill > 0.0f) draw_quad(bar_x, bar_y, fill, bar_h, sc);
	}
}

int draw_button_hint(int x, int y_text_top, const ps3ui_image_t *btn,
                      const char *label) {
	const int icon_h = FT_FOOTER + 6;
	const int icon_w = icon_h;
	const int gap_after_icon  = 10;
	const int gap_after_label = 36;

	ps3ui_image_draw_inline(btn, x, y_text_top, FT_FOOTER, icon_w, icon_h,
	                        PS3UI_VALIGN_TEXT_CENTER);
	int after_icon  = x + icon_w + gap_after_icon;
	int after_label = ps3ui_text_draw(after_icon, y_text_top,
	                                   COL_TEXT_DIM, FT_FOOTER, label);
	return after_label + gap_after_label;
}

/* Per-screen footer hints — only the actions that mean something here. */
void draw_footer(const AppState *st) {
	int y = 1080 - 48;
	draw_quad(64, (float)(y - 12), 1792, 1, COL_DIVIDER);

	int x = 64;
	if (st->screen == TV_SCREEN_GAMES) {
		x = draw_button_hint(x, y, &g_btn_cross,  "open game");
		x = draw_button_hint(x, y, &g_btn_square, "rescan");
		x = draw_button_hint(x, y, &g_btn_circle, "exit");
	} else {
		x = draw_button_hint(x, y, &g_btn_cross,    "export");
		x = draw_button_hint(x, y, &g_btn_triangle, "sign for PS3");
		x = draw_button_hint(x, y, &g_btn_square,   "rescan");
		x = draw_button_hint(x, y, &g_btn_circle,   "back");
	}
	(void)x;
	ps3ui_text_drawf(1600, y, COL_TEXT_DIM, FT_FOOTER, "SELECT  help");
}

constexpr uint64_t TOAST_FADE_US = 400ull * 1000ull;

void draw_toast(const AppState *st) {
	if (!st->toast_until_us || !st->toast_msg[0]) return;
	uint64_t now = sysGetSystemTime();
	if (now >= st->toast_until_us) return;

	uint64_t remaining = st->toast_until_us - now;
	uint32_t alpha = 0xF0;
	if (remaining < TOAST_FADE_US) {
		alpha = (uint32_t)((uint64_t)0xF0 * remaining / TOAST_FADE_US);
	}

	const float pw = 720.0f, ph = 56.0f;
	const float px = (1920.0f - pw) / 2.0f;
	const float py = 760.0f;

	uint32_t bg     = 0x1F273500u | (alpha & 0xFF);
	uint32_t stripe = (COL_ACCENT & 0xFFFFFF00u) | (alpha & 0xFF);
	uint32_t text   = (COL_TEXT   & 0xFFFFFF00u) | (alpha & 0xFF);

	draw_quad(px, py, pw, ph, bg);
	draw_quad(px, py, 4,  ph, stripe);

	int ty = (int)py + (int)((ph - FT_JOB) / 2.0f);
	ps3ui_text_drawf((int)px + 24, ty, text, FT_JOB, "%s", st->toast_msg);
}

constexpr int FT_HELP_TITLE = 40;
constexpr int FT_HELP_HEAD  = 28;
constexpr int FT_HELP_LINE  = 24;

void draw_help_overlay(const AppState *st) {
	draw_quad(0, 0, 1920, 1080, 0x000000C0u);

	const float pw = 1320.0f;
	const float ph = 880.0f;
	const float px = (1920.0f - pw) / 2.0f;
	const float py = (1080.0f - ph) / 2.0f;
	draw_quad(px, py, pw, ph, 0x161B25F0u);
	draw_quad(px, py, pw, 4,  COL_ACCENT);

	const int margin = 56;
	int tx = (int)px + margin;
	int ty = (int)py + 32;

	ps3ui_text_draw(tx, ty, COL_ACCENT, FT_HELP_TITLE, "Controls & glossary");
	ty += FT_HELP_TITLE + 6;
	ps3ui_text_draw(tx, ty, COL_DIM, FT_HELP_LINE,
		"Press SELECT again to close");
	ty += FT_HELP_LINE + 22;

	struct HelpRow {
		const ps3ui_image_t *btn;
		const char          *what;
		const char          *desc;
	};

	HelpRow games_rows[] = {
		{ &g_btn_cross,    "Games view",       "open the selected game's slots" },
		{ &g_btn_circle,   "Games view",       "exit to XMB" },
		{ &g_btn_cross,    "Slots view",       "export this slot as a .zip" },
		{ &g_btn_triangle, "Slots view",       "sign for PS3 (RPCS3 slots only)" },
		{ &g_btn_circle,   "Slots view",       "back to Games" },
		{ &g_btn_square,   "Anywhere",         "re-scan HDD + USB" },
	};

	const int icon_size = FT_HELP_LINE + 6;
	const int col_icon  = tx;
	const int col_what  = tx + icon_size + 28;
	const int col_desc  = col_what  + 240;
	const int row_h     = FT_HELP_LINE + 14;

	for (size_t i = 0; i < sizeof(games_rows)/sizeof(games_rows[0]); i++) {
		ps3ui_image_draw_inline(games_rows[i].btn, col_icon, ty, FT_HELP_LINE,
		                         icon_size, icon_size,
		                         PS3UI_VALIGN_TEXT_CENTER);
		ps3ui_text_draw(col_what, ty, COL_TEXT_DIM, FT_HELP_LINE, games_rows[i].what);
		ps3ui_text_draw(col_desc, ty, COL_TEXT,     FT_HELP_LINE, games_rows[i].desc);
		ty += row_h;
	}

	ty += 12;
	draw_quad((float)tx, (float)ty, pw - 2 * margin, 1, COL_DIVIDER);
	ty += 18;

	/* Glossary */
	ps3ui_text_draw(tx, ty, COL_ACCENT, FT_HELP_HEAD, "Glossary");
	ty += FT_HELP_HEAD + 10;

	struct GlossRow { const char *term; const char *desc; };
	GlossRow glossary[] = {
		{ "Slot",         "One PS3 save folder (e.g. BLES01807-AUTOSAVE)." },
		{ "RPCS3 format", "Plain files, no PARAM.PFD — what RPCS3 emits." },
		{ "PS3-signed",   "Has a PARAM.PFD HMAC; the only kind PS3 loads." },
		{ "Export",       "Pack the slot into a .zip RPCS3 can read. Original is untouched." },
		{ "Sign for PS3", "Add a PARAM.PFD to an RPCS3-format slot in place." },
	};

	const int gloss_col_term = tx;
	const int gloss_col_desc = tx + 240;
	const int gloss_row_h    = FT_HELP_LINE + 8;
	for (size_t i = 0; i < sizeof(glossary)/sizeof(glossary[0]); i++) {
		ps3ui_text_draw(gloss_col_term, ty, COL_ACCENT, FT_HELP_LINE,
			glossary[i].term);
		ps3ui_text_draw(gloss_col_desc, ty, COL_TEXT, FT_HELP_LINE,
			glossary[i].desc);
		ty += gloss_row_h;
	}

	ty += 14;
	draw_quad((float)tx, (float)ty, pw - 2 * margin, 1, COL_DIVIDER);
	ty += 18;

	/* System (info that used to live in the header). */
	ps3ui_text_draw(tx, ty, COL_ACCENT, FT_HELP_HEAD, "System");
	ty += FT_HELP_HEAD + 10;

	const char *ip = (st && st->lan_ip[0]) ? st->lan_ip : "<PS3>";
	ps3ui_text_drawf(tx, ty, COL_TEXT_DIM, FT_HELP_LINE,
		"Version       v%s", SAVESYNC_VERSION);
	ty += FT_HELP_LINE + 4;
	ps3ui_text_drawf(tx, ty, COL_TEXT_DIM, FT_HELP_LINE,
		"PFD self-test %s",
		st && st->pfd_test_result ? "ok" : "fail");
	ty += FT_HELP_LINE + 4;
	ps3ui_text_drawf(tx, ty, COL_TEXT_DIM, FT_HELP_LINE,
		"Web UI        http://%s:%d/", ip, SAVESYNC_HTTP_PORT);
	ty += FT_HELP_LINE + 12;

	ps3ui_text_draw(tx, ty, COL_DIM, FT_HELP_LINE,
		"The Web UI mirrors every action on this screen, plus drag-and-drop");
	ty += FT_HELP_LINE + 4;
	ps3ui_text_draw(tx, ty, COL_DIM, FT_HELP_LINE,
		"import: upload an RPCS3 zip and pick the target slot, optionally");
	ty += FT_HELP_LINE + 4;
	ps3ui_text_draw(tx, ty, COL_DIM, FT_HELP_LINE,
		"backing up or overwriting the existing save.");
}

}  // namespace

int tv_ui_init(void) {
	int rc = tiny3d_Init(1024 * 1024);
	if (rc < 0 && rc != TINY3D_OK) return -1;

	tiny3d_UserViewport(1, 0.0f, 0.0f,
		(float)Video_Resolution.width  / 1920.0f,
		(float)Video_Resolution.height / 1080.0f,
		1.0f, 1.0f);

	uint32_t *texture_mem = (uint32_t *)tiny3d_AllocTexture(1 * 1024 * 1024);
	if (!texture_mem) return -2;

	uint32_t *texture_pointer = texture_mem;
	ResetFont();
	texture_pointer = (uint32_t *)AddFontFromBitmapArray(
		(uint8_t *)font, (uint8_t *)texture_pointer, 32, 255, 16, 32, 2,
		BIT0_FIRST_PIXEL);
	texture_pointer = (uint32_t *)AddFontFromBitmapArray(
		(uint8_t *)msx, (uint8_t *)texture_pointer, 0, 254, 8, 8, 1,
		BIT7_FIRST_PIXEL);
	(void)texture_pointer;

	void *atlas = tiny3d_AllocTexture(PS3UI_TEXT_ATLAS_BYTES);
	if (atlas && ps3ui_text_init(atlas)) {
		if (ps3ui_text_load_memory(0, inter_ttf, (size_t)inter_ttf_size) == 0)
			g_ttf_ready = 1;
	}

	icon_cache_init();

	g_btn_cross    = ps3ui_image_load_png_memory(cross_png,    cross_png_size);
	g_btn_square   = ps3ui_image_load_png_memory(square_png,   square_png_size);
	g_btn_triangle = ps3ui_image_load_png_memory(triangle_png, triangle_png_size);
	g_btn_circle   = ps3ui_image_load_png_memory(circle_png,   circle_png_size);
	g_btn_l1       = ps3ui_image_load_png_memory(L1_png,       L1_png_size);
	g_btn_r1       = ps3ui_image_load_png_memory(R1_png,       R1_png_size);
	g_logo_ps3     = ps3ui_image_load_png_memory(ps3_png,      ps3_png_size);
	g_logo_rpcs3   = ps3ui_image_load_png_memory(rpcs3_png,    rpcs3_png_size);

	return 0;
}

void tv_ui_render(const AppState *st, int /*net_step*/) {
	tiny3d_Clear(0xff000000, TINY3D_CLEAR_ALL);
	tiny3d_AlphaTest(1, 0x10, TINY3D_ALPHA_FUNC_GEQUAL);
	tiny3d_BlendFunc(1,
		(blend_src_func)(TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA |
		                 TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA),
		(blend_dst_func)(TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA |
		                 TINY3D_BLEND_FUNC_DST_ALPHA_ZERO),
		(blend_func)(TINY3D_BLEND_RGB_FUNC_ADD |
		             TINY3D_BLEND_ALPHA_FUNC_ADD));
	tiny3d_Project2D();

	if (g_ttf_ready) ps3ui_text_frame_begin();

	draw_background_gradient();
	draw_breadcrumb(st);

	if (st->screen == TV_SCREEN_SLOTS) {
		draw_slots_view(st);
	} else {
		draw_games_view(st);
	}

	draw_jobs_strip();
	draw_toast(st);
	draw_footer(st);
	if (st->show_help) draw_help_overlay(st);

	tiny3d_Flip();
}

void tv_ui_shutdown(void) {
	tiny3d_Exit();
}
