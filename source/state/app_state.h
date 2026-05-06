/*
 * app_state.h — single source of truth for AppState. Shared by main.cpp
 * (owner / mutator) and tv_ui.cpp (read-only renderer). Replaces the
 * earlier struct-mirror pattern that drifted twice.
 */

#ifndef SAVESYNC_APP_STATE_H
#define SAVESYNC_APP_STATE_H

#include <stdint.h>
#include "savedata.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TV_GAMES_MAX 64
#define TV_SLOTS_MAX 64

typedef enum {
	TV_SCREEN_GAMES = 0,
	TV_SCREEN_SLOTS = 1,
} tv_screen_t;

/* One entry per unique title_id. first_save_idx points into AppState.saves
 * and is used to grab a representative ICON0/title for the games-list row. */
typedef struct {
	char title_id[SAVESYNC_TITLE_ID_LEN];
	int  first_save_idx;
	int  slot_count;
} tv_game_entry_t;

typedef struct AppState {
	savesync_save_list_t saves;

	/* Drilldown state: GAMES view shows one row per title_id; pressing × on
	 * a game enters SLOTS view scoped to active_title_id. ○ in slots view
	 * pops back to games view; ○ in games view exits to XMB. */
	tv_screen_t screen;
	char        active_title_id[SAVESYNC_TITLE_ID_LEN];

	/* Per-screen navigation state — kept independent so hopping between
	 * views never resets where the user was on the games list. */
	int games_selected;
	int games_scroll;
	int slots_selected;
	int slots_scroll;

	/* Cached games index, rebuilt on every scan. */
	int             games_count;
	tv_game_entry_t games[TV_GAMES_MAX];

	/* Cached slot indices for active_title_id, rebuilt on scan and on
	 * entering slots view. Each entry indexes into saves.items[]. */
	int slot_count;
	int slot_save_idx[TV_SLOTS_MAX];

	int  pfd_test_result;
	int  show_help;
	char lan_ip[16];

	/* Transient toast (~2.5 s + fade). 0 = none. */
	char     toast_msg[160];
	uint64_t toast_until_us;

	/* Used by refresh_if_jobs_completed to detect newly-finished
	 * disk-mutating jobs and trigger an auto-rescan. */
	int completed_jobs_seen;
} AppState;

/* Clear the entire AppState to a known-zero baseline. */
void tv_state_init(AppState *st);

/* Rebuild the games[] index from saves[]. Stable order: games appear in
 * the order their first slot was encountered. Caller is responsible for
 * clamping/re-anchoring games_selected after a rebuild. */
void tv_state_rebuild_games(AppState *st);

/* Rebuild slot_save_idx[] for the current active_title_id. Returns the
 * number of slots found. Slot order matches save list order. */
int  tv_state_rebuild_slots(AppState *st);

/* Find the games[] index for a title_id, or -1 if not present. */
int  tv_state_find_game(const AppState *st, const char *title_id);

#ifdef __cplusplus
}
#endif

#endif
