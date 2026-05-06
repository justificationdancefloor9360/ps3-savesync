/*
 * SaveSync — main.cpp.
 *
 * Drilldown TV UI on tiny3d:
 *   GAMES view  — one row per title_id (× to enter, ○ to exit to XMB)
 *   SLOTS view  — slots for the active game (× export, △ sign, ○ back)
 *
 * The browser at http://<PS3>:8080 remains the primary surface; the TV UI
 * is a status display + basic navigation.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <io/pad.h>
#include <sys/systime.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <sysutil/msg.h>
#include <net/net.h>
#include <net/netctl.h>

#include "savesync.h"
#include "savedata.h"
#include "convert.h"
#include "pfd.h"
#include "jobs.h"
#include "ps3http.h"
#include "routes.h"
#include "tv_ui.h"
#include "app_state.h"

extern "C" {
volatile int savesync_should_quit = 0;
}

static ps3http_server_t *g_http_server = nullptr;

extern "C" void savesync_http_log_cb(const char *msg, void * /*user*/) {
	FILE *f = fopen("/dev_hdd0/tmp/savesync.log", "a");
	if (!f) return;
	fprintf(f, "[%llu] %s\n", (unsigned long long)sysGetSystemTime(), msg);
	fclose(f);
}

namespace {

constexpr int VISIBLE_ROWS = 8;

void boot_log(const char *fmt, ...) {
	FILE *f = fopen("/dev_hdd0/tmp/savesync.log", "a");
	if (!f) return;
	fprintf(f, "[%llu] ", (unsigned long long)sysGetSystemTime());
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fprintf(f, "\n");
	fclose(f);
}

volatile int g_running = 1;

extern "C" void sysutil_exit_cb(uint64_t status, uint64_t /*p*/, void * /*u*/) {
	if (status == SYSUTIL_EXIT_GAME) g_running = 0;
}

int g_prev_buttons = 0;

/* Set while the system "Quit?" dialog is open. We swallow all pad input
 * during this window so the user's ○ can't bleed back into navigation,
 * and the dialog's own Yes/No is the sole input source. */
volatile int g_exit_confirm_open = 0;

extern "C" void exit_confirm_cb(msgButton btn, void * /*user*/) {
	g_exit_confirm_open = 0;
	if (btn == MSG_DIALOG_BTN_YES) {
		savesync_should_quit = 1;
	}
	/* NO / ESCAPE: stay in the app. */
}

void open_exit_confirm() {
	if (g_exit_confirm_open) return;
	g_exit_confirm_open = 1;
	msgType t = (msgType)(MSG_DIALOG_NORMAL
	                      | MSG_DIALOG_BTN_TYPE_YESNO
	                      | MSG_DIALOG_DEFAULT_CURSOR_NO);
	msgDialogOpen2(t, "Exit SaveSync?", exit_confirm_cb, nullptr, nullptr);
}

constexpr uint64_t TOAST_LIFETIME_US = 2500ull * 1000ull;

void toast_set(AppState *st, const char *msg) {
	snprintf(st->toast_msg, sizeof(st->toast_msg), "%s", msg ? msg : "");
	st->toast_until_us = sysGetSystemTime() + TOAST_LIFETIME_US;
}

/* Re-anchor selection after a rebuild: keeps the cursor on the same game
 * (or slot) the user was on, falling back to row 0 when the anchor is
 * gone. Clamps scroll to keep the selection on screen. */
void clamp_selection(int *sel, int *scroll, int count, int visible) {
	if (count <= 0) { *sel = 0; *scroll = 0; return; }
	if (*sel >= count) *sel = count - 1;
	if (*sel < 0)      *sel = 0;
	if (*sel < *scroll)               *scroll = *sel;
	if (*sel >= *scroll + visible)    *scroll = *sel - visible + 1;
	if (*scroll < 0)                  *scroll = 0;
}

/* Full rescan + view rebuild. Preserves the user's cursor on:
 *   - Games view: the same title_id the cursor was on.
 *   - Slots view: the same dir_name the cursor was on (and stays on the
 *     same game even if its slot count changes; if all its slots vanish,
 *     the slots view shows the empty state — we don't auto-pop). */
void rescan_preserving_selection(AppState *st) {
	char prev_game_tid[SAVESYNC_TITLE_ID_LEN] = "";
	if (st->games_selected >= 0 && st->games_selected < st->games_count) {
		snprintf(prev_game_tid, sizeof(prev_game_tid), "%s",
		         st->games[st->games_selected].title_id);
	}

	char prev_slot_dir[SAVESYNC_DIR_NAME_LEN] = "";
	if (st->screen == TV_SCREEN_SLOTS &&
	    st->slots_selected >= 0 && st->slots_selected < st->slot_count) {
		int idx = st->slot_save_idx[st->slots_selected];
		if (idx >= 0 && idx < (int)st->saves.count) {
			snprintf(prev_slot_dir, sizeof(prev_slot_dir), "%s",
			         st->saves.items[idx].dir_name);
		}
	}

	savesync_save_list_free(&st->saves);
	savesync_save_list_init(&st->saves);
	savesync_scan_hdd(&st->saves);
	savesync_scan_usb(&st->saves);
	tv_state_rebuild_games(st);

	/* Re-anchor games selection by title_id. */
	if (prev_game_tid[0]) {
		int g = tv_state_find_game(st, prev_game_tid);
		if (g >= 0) st->games_selected = g;
	}
	clamp_selection(&st->games_selected, &st->games_scroll,
	                st->games_count, VISIBLE_ROWS);

	if (st->screen == TV_SCREEN_SLOTS) {
		tv_state_rebuild_slots(st);
		/* Re-anchor slots selection by dir_name. */
		st->slots_selected = 0;
		if (prev_slot_dir[0]) {
			for (int i = 0; i < st->slot_count; i++) {
				int idx = st->slot_save_idx[i];
				if (strcmp(st->saves.items[idx].dir_name, prev_slot_dir) == 0) {
					st->slots_selected = i;
					break;
				}
			}
		}
		clamp_selection(&st->slots_selected, &st->slots_scroll,
		                st->slot_count, VISIBLE_ROWS);
	}
}

void enter_slots_for_selection(AppState *st) {
	if (st->games_selected < 0 || st->games_selected >= st->games_count) return;
	const tv_game_entry_t *g = &st->games[st->games_selected];
	snprintf(st->active_title_id, sizeof(st->active_title_id), "%s",
	         g->title_id);
	tv_state_rebuild_slots(st);
	st->slots_selected = 0;
	st->slots_scroll   = 0;
	st->screen = TV_SCREEN_SLOTS;
}

void back_to_games(AppState *st) {
	st->screen = TV_SCREEN_GAMES;
	st->active_title_id[0] = '\0';
	st->slot_count   = 0;
	st->slots_selected = 0;
	st->slots_scroll   = 0;
}

/* Polls the jobs queue once per frame; when a CONVERT or IMPORT job has
 * newly transitioned to DONE / FAILED, the on-disk save state has changed
 * — re-scan and rebuild so flavor tags flip immediately. */
void refresh_if_jobs_completed(AppState *st) {
	/* Static — sizeof(savesync_job_t) * 16 is ~40 KB. The PPU main thread
	 * stack can't hold that, see CLAUDE.md "stack budget" rule. */
	static savesync_job_t snap[SAVESYNC_MAX_JOBS];
	size_t n = savesync_jobs_snapshot(snap, SAVESYNC_MAX_JOBS);
	int completed_now = 0;
	for (size_t i = 0; i < n; i++) {
		bool terminal = snap[i].state == SAVESYNC_JOB_DONE
		             || snap[i].state == SAVESYNC_JOB_FAILED;
		bool mutates  = snap[i].kind == SAVESYNC_JOB_KIND_CONVERT
		             || snap[i].kind == SAVESYNC_JOB_KIND_IMPORT;
		if (terminal && mutates) completed_now++;
	}
	if (completed_now > st->completed_jobs_seen) {
		rescan_preserving_selection(st);
		toast_set(st, "Saves updated");
	}
	st->completed_jobs_seen = completed_now;
}

void poll_pad_games(AppState *st, int pressed) {
	int *sel    = &st->games_selected;
	int *scroll = &st->games_scroll;
	int  count  = st->games_count;

	if (count > 0) {
		if (pressed & (1 << 1) && *sel > 0)            (*sel)--;
		if (pressed & (1 << 2) && *sel < count - 1)    (*sel)++;
		if (pressed & (1 << 3)) { *sel -= VISIBLE_ROWS; if (*sel < 0) *sel = 0; }
		if (pressed & (1 << 4)) { *sel += VISIBLE_ROWS; if (*sel > count - 1) *sel = count - 1; }
	}

	/* × on a game enters slots view. */
	if ((pressed & (1 << 5)) && count > 0) {
		enter_slots_for_selection(st);
		return; /* clamp on slots side, not games */
	}

	/* □ rescan. */
	if (pressed & (1 << 7)) {
		rescan_preserving_selection(st);
		boot_log("pad: rescan saves=%u games=%d",
		         (unsigned)st->saves.count, st->games_count);
	}

	/* ○ exits to XMB (handled in poll_pad). */

	clamp_selection(sel, scroll, count, VISIBLE_ROWS);
}

void poll_pad_slots(AppState *st, int pressed) {
	int *sel    = &st->slots_selected;
	int *scroll = &st->slots_scroll;
	int  count  = st->slot_count;

	if (count > 0) {
		if (pressed & (1 << 1) && *sel > 0)            (*sel)--;
		if (pressed & (1 << 2) && *sel < count - 1)    (*sel)++;
		if (pressed & (1 << 3)) { *sel -= VISIBLE_ROWS; if (*sel < 0) *sel = 0; }
		if (pressed & (1 << 4)) { *sel += VISIBLE_ROWS; if (*sel > count - 1) *sel = count - 1; }
	}

	/* ○ goes back to games view. We swallow the press here so the global
	 * ○-quits handler doesn't ALSO fire and exit to XMB. */
	if (pressed & (1 << 0)) {
		back_to_games(st);
		return;
	}

	const savesync_save_t *cur = nullptr;
	if (count > 0 && *sel >= 0 && *sel < count) {
		int idx = st->slot_save_idx[*sel];
		if (idx >= 0 && idx < (int)st->saves.count) {
			cur = &st->saves.items[idx];
		}
	}

	/* × export. */
	if ((pressed & (1 << 5)) && cur) {
		char job_id[SAVESYNC_JOB_ID_LEN];
		char label [SAVESYNC_JOB_LABEL_LEN];
		snprintf(label, sizeof(label), "Export %s", cur->dir_name);
		int rc = savesync_jobs_enqueue_export(cur->path, label, job_id);
		boot_log("pad: export %s rc=%d job=%s",
		         cur->dir_name, rc, rc == 0 ? job_id : "(queue full)");
		if (rc == 0) {
			char msg[160];
			snprintf(msg, sizeof(msg),
				"Exporting \xE2\x80\x94 open http://%s:%d/ to download",
				st->lan_ip[0] ? st->lan_ip : "<PS3>",
				SAVESYNC_HTTP_PORT);
			toast_set(st, msg);
		} else {
			toast_set(st, "Export queue full");
		}
	}

	/* △ sign for PS3 (RPCS3-flavored slots only). */
	if ((pressed & (1 << 6)) && cur) {
		if (cur->flavor != SAVESYNC_FLAVOR_RPCS3) {
			toast_set(st, "Already signed for PS3 — nothing to do");
		} else {
			savesync_convert_options_t opts;
			savesync_convert_default_options(&opts);
			opts.direction = SAVESYNC_DIR_RPCS3_TO_PS3;
			char job_id[SAVESYNC_JOB_ID_LEN];
			char label [SAVESYNC_JOB_LABEL_LEN];
			snprintf(label, sizeof(label), "Sign %s", cur->dir_name);
			int rc = savesync_jobs_enqueue_convert(cur->path, &opts,
			                                       label, job_id);
			boot_log("pad: sign %s rc=%d job=%s",
			         cur->dir_name, rc, rc == 0 ? job_id : "(queue full)");
			if (rc != 0) toast_set(st, "Job queue full");
		}
	}

	/* □ rescan (stays in slots view; if the active game vanished, the
	 * empty-state guides the user back via ○). */
	if (pressed & (1 << 7)) {
		rescan_preserving_selection(st);
		boot_log("pad: rescan in slots, slots_now=%d", st->slot_count);
	}

	clamp_selection(sel, scroll, st->slot_count, VISIBLE_ROWS);
}

void poll_pad(AppState *st) {
	padInfo info;
	padData data;
	if (ioPadGetInfo(&info) != 0) return;
	if (!info.status[0]) return;
	if (ioPadGetData(0, &data) != 0) return;

	int btns = (data.BTN_CIRCLE   ? 1 << 0 : 0)
	         | (data.BTN_UP       ? 1 << 1 : 0)
	         | (data.BTN_DOWN     ? 1 << 2 : 0)
	         | (data.BTN_L1       ? 1 << 3 : 0)
	         | (data.BTN_R1       ? 1 << 4 : 0)
	         | (data.BTN_CROSS    ? 1 << 5 : 0)
	         | (data.BTN_TRIANGLE ? 1 << 6 : 0)
	         | (data.BTN_SQUARE   ? 1 << 7 : 0)
	         | (data.BTN_SELECT   ? 1 << 8 : 0);
	int pressed = btns & ~g_prev_buttons;
	g_prev_buttons = btns;

	/* While the system Yes/No dialog is up, the dialog handles input —
	 * swallow everything pad-side so it can't double-fire. */
	if (g_exit_confirm_open) return;

	/* SELECT toggles help. While the modal is up navigation is suppressed,
	 * but ○ still routes through to its per-screen meaning so the user
	 * always has a way out. */
	if (pressed & (1 << 8)) st->show_help = !st->show_help;
	if (st->show_help) return;

	if (st->screen == TV_SCREEN_SLOTS) {
		poll_pad_slots(st, pressed);
	} else {
		/* GAMES view: ○ asks for confirmation, doesn't quit immediately. */
		if (pressed & (1 << 0)) {
			open_exit_confirm();
			return;
		}
		poll_pad_games(st, pressed);
	}
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[]) {
	boot_log("=== savesync entered main() ===");

	ioPadInit(7);
	boot_log("ioPadInit ok");

	if (sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_exit_cb, nullptr) == 0) {
		boot_log("sysUtilRegisterCallback ok");
	} else {
		boot_log("sysUtilRegisterCallback failed (continuing)");
	}

	AppState st;
	tv_state_init(&st);

	st.pfd_test_result = (savesync_pfd_self_test() == 0) ? 1 : 0;
	boot_log("pfd_self_test = %d", st.pfd_test_result);

	savesync_scan_hdd(&st.saves);
	savesync_scan_usb(&st.saves);
	boot_log("scan ok (count=%u)", (unsigned)st.saves.count);

	tv_state_rebuild_games(&st);
	boot_log("games index built (count=%d)", st.games_count);

	savesync_jobs_init();
	boot_log("jobs_init ok");

	int tv_ui_ok = (tv_ui_init() == 0) ? 1 : 0;
	boot_log("tv_ui_init = %d", tv_ui_ok);

	int net_ready = 0, net_step = 0;
	int rc = sysModuleLoad(SYSMODULE_NET);
	boot_log("sysModuleLoad(NET) = %d", rc);
	if (rc == 0) {
		net_step = 1;
		rc = sysModuleLoad(SYSMODULE_NETCTL);
		boot_log("sysModuleLoad(NETCTL) = %d", rc);
		if (rc == 0) {
			net_step = 2;
			rc = netInitialize();
			boot_log("netInitialize() = %d", rc);
			if (rc == 0) {
				net_step = 3;
				rc = netCtlInit();
				boot_log("netCtlInit() = %d", rc);
				if (rc == 0) {
					net_step = 4;
					union net_ctl_info ipinfo;
					if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &ipinfo) == 0) {
						snprintf(st.lan_ip, sizeof(st.lan_ip), "%s",
						         ipinfo.ip_address);
						boot_log("netCtlGetInfo IP = %s", st.lan_ip);
					}
					ps3http_config_t hcfg;
					memset(&hcfg, 0, sizeof(hcfg));
					hcfg.port             = SAVESYNC_HTTP_PORT;
					hcfg.max_workers      = 8;
					hcfg.max_inmem_body   = 1 * 1024 * 1024;
					hcfg.read_timeout_sec = 30;
					hcfg.send_timeout_sec = 30;
					hcfg.handler          = savesync_routes_dispatch;
					hcfg.log              = savesync_http_log_cb;
					g_http_server = ps3http_server_new(&hcfg);
					rc = g_http_server ? ps3http_server_start(g_http_server) : -1;
					boot_log("ps3http_server_start(%d) = %d", SAVESYNC_HTTP_PORT, rc);
					if (rc == 0) {
						net_step = 5;
						net_ready = 1;
					}
				}
			}
		}
	}
	boot_log("net_step=%d net_ready=%d, entering main loop", net_step, net_ready);

	uint64_t frame = 0;
	while (g_running && !savesync_should_quit) {
		sysUtilCheckCallback();
		poll_pad(&st);
		refresh_if_jobs_completed(&st);

		if (tv_ui_ok) {
			tv_ui_render(&st, net_step);
		} else {
			sysUsleep(16 * 1000);
		}

		frame++;
		if ((frame % 600) == 0) {
			boot_log("heartbeat frame=%llu running=%d quit=%d",
			         (unsigned long long)frame, g_running, savesync_should_quit);
		}
	}

	boot_log("main loop exited running=%d quit=%d frame=%llu",
	         g_running, savesync_should_quit, (unsigned long long)frame);

	if (tv_ui_ok) tv_ui_shutdown();

	if (net_ready) {
		if (g_http_server) {
			ps3http_server_stop(g_http_server);
			ps3http_server_free(g_http_server);
			g_http_server = nullptr;
		}
		netCtlTerm();
		netDeinitialize();
		sysModuleUnload(SYSMODULE_NETCTL);
		sysModuleUnload(SYSMODULE_NET);
	}
	savesync_jobs_shutdown();
	ioPadEnd();
	savesync_save_list_free(&st.saves);
	boot_log("clean exit complete");
	return 0;
}
