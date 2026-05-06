#include "app_state.h"

#include <stdio.h>
#include <string.h>

void tv_state_init(AppState *st) {
	memset(st, 0, sizeof(*st));
	savesync_save_list_init(&st->saves);
}

int tv_state_find_game(const AppState *st, const char *title_id) {
	for (int i = 0; i < st->games_count; i++) {
		if (strcmp(st->games[i].title_id, title_id) == 0) return i;
	}
	return -1;
}

void tv_state_rebuild_games(AppState *st) {
	st->games_count = 0;
	for (size_t i = 0; i < st->saves.count; i++) {
		const savesync_save_t *s = &st->saves.items[i];
		int found = tv_state_find_game(st, s->title_id);
		if (found < 0) {
			if (st->games_count >= TV_GAMES_MAX) continue;
			tv_game_entry_t *g = &st->games[st->games_count++];
			snprintf(g->title_id, sizeof(g->title_id), "%s", s->title_id);
			g->first_save_idx = (int)i;
			g->slot_count = 1;
		} else {
			st->games[found].slot_count++;
		}
	}
}

int tv_state_rebuild_slots(AppState *st) {
	st->slot_count = 0;
	if (!st->active_title_id[0]) return 0;
	for (size_t i = 0; i < st->saves.count && st->slot_count < TV_SLOTS_MAX; i++) {
		const savesync_save_t *s = &st->saves.items[i];
		if (strcmp(s->title_id, st->active_title_id) == 0) {
			st->slot_save_idx[st->slot_count++] = (int)i;
		}
	}
	return st->slot_count;
}
