#include "savedata.h"
#include "sfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/systime.h>

/* Diagnostic logger: writes to /dev_hdd0/tmp/savesync.log (same file as main.cpp/server.c).
 * Tagged [scan] for grepping. Remove once savedata scan is verified working. */
static void scan_log(const char *fmt, ...) {
	FILE *f = fopen("/dev_hdd0/tmp/savesync.log", "a");
	if (!f) return;
	fprintf(f, "[%llu] [scan] ", (unsigned long long)sysGetSystemTime());
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fprintf(f, "\n");
	fclose(f);
}

/* Safe path-join: clip each component to prevent snprintf truncation warning. */
#define SCAN_HALF (SAVESYNC_PATH_LEN / 2 - 2)
static void scan_pjoin(char *out, size_t outsz,
                       const char *a, const char *b) {
    char da[SCAN_HALF + 1], db[SCAN_HALF + 1];
    strncpy(da, a, SCAN_HALF); da[SCAN_HALF] = '\0';
    strncpy(db, b, SCAN_HALF); db[SCAN_HALF] = '\0';
    snprintf(out, outsz, "%s/%s", da, db);
}

static int has_nonempty_param_pfd(const char *abs_path) {
	char path[SAVESYNC_PATH_LEN];
	snprintf(path, sizeof(path), "%s/PARAM.PFD", abs_path);
	struct stat st;
	if (stat(path, &st) != 0) return 0;
	return st.st_size > 0;
}

static uint64_t file_size_or_zero(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) return 0;
	return (uint64_t)st.st_size;
}

void savesync_save_list_init(savesync_save_list_t *list) {
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

void savesync_save_list_free(savesync_save_list_t *list) {
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

static int list_push(savesync_save_list_t *list, const savesync_save_t *s) {
	if (list->count == list->capacity) {
		size_t cap = list->capacity ? list->capacity * 2 : 16;
		savesync_save_t *n = (savesync_save_t *)realloc(list->items, cap * sizeof(*n));
		if (!n) return -1;
		list->items = n;
		list->capacity = cap;
	}
	list->items[list->count++] = *s;
	return 0;
}

static void copy_str(char *dst, size_t dst_size, const char *src) {
	if (!src) { dst[0] = '\0'; return; }
	size_t n = strlen(src);
	if (n >= dst_size) n = dst_size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void extract_title_id(const char *dir_name, char out[SAVESYNC_TITLE_ID_LEN]) {
	out[0] = '\0';
	/* PS3 dir-name pattern: BLES01807-MYSAVE00, BCUS98174-... — first 9 chars are TITLE_ID */
	int len = 0;
	while (len < SAVESYNC_TITLE_ID_LEN - 1 && dir_name[len] && dir_name[len] != '-') {
		out[len] = dir_name[len];
		len++;
	}
	out[len] = '\0';
}

int savesync_inspect_dir(const char *abs_path, savesync_save_t *out) {
	memset(out, 0, sizeof(*out));

	const char *base = strrchr(abs_path, '/');
	base = base ? base + 1 : abs_path;
	copy_str(out->dir_name, sizeof(out->dir_name), base);
	copy_str(out->path,     sizeof(out->path),     abs_path);
	extract_title_id(out->dir_name, out->title_id);

	/* Heap-allocate sfo_path (1 KB) — see scan_savedata_root for context. */
	char *sfo_path = (char *)malloc(SAVESYNC_PATH_LEN);
	if (!sfo_path) return -1;
	snprintf(sfo_path, SAVESYNC_PATH_LEN, "%s/PARAM.SFO", abs_path);

	sfo_t sfo;
	if (sfo_load(&sfo, sfo_path) != 0) {
		free(sfo_path);
		return -1;
	}

	const char *title    = sfo_get_str(&sfo, "TITLE");
	const char *subtitle = sfo_get_str(&sfo, "SUB_TITLE");
	const char *detail   = sfo_get_str(&sfo, "DETAIL");
	if (title)    copy_str(out->title,    sizeof(out->title),    title);
	if (subtitle) copy_str(out->subtitle, sizeof(out->subtitle), subtitle);
	if (detail)   copy_str(out->detail,   sizeof(out->detail),   detail);
	sfo_get_account_id(&sfo, out->account_id_hex);

	sfo_free(&sfo);

	/* Scan dir contents for size + file count + icon presence */
	char *p = (char *)malloc(SAVESYNC_PATH_LEN);
	if (!p) { free(sfo_path); return -1; }
	DIR *d = opendir(abs_path);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.') continue;
			scan_pjoin(p, SAVESYNC_PATH_LEN, abs_path, de->d_name);
			out->total_size_bytes += file_size_or_zero(p);
			out->file_count++;
			if (strcmp(de->d_name, "ICON0.PNG") == 0) out->has_icon0 = 1;
		}
		closedir(d);
	}
	free(p);

	out->flavor = has_nonempty_param_pfd(abs_path)
	              ? SAVESYNC_FLAVOR_PS3_SIGNED
	              : SAVESYNC_FLAVOR_RPCS3;
	free(sfo_path);
	return 0;
}

static int is_savedata_dir(const char *parent, const char *entry, char *abs_out, size_t abs_size) {
	if (entry[0] == '.') return 0;
	scan_pjoin(abs_out, abs_size, parent, entry);
	struct stat st;
	if (stat(abs_out, &st) != 0) return 0;
	if (!S_ISDIR(st.st_mode)) return 0;
	char sfo[SAVESYNC_PATH_LEN];
	snprintf(sfo, sizeof(sfo), "%s/PARAM.SFO", abs_out);
	return access(sfo, 0) == 0;
}

static void scan_savedata_root(const char *root, savesync_save_list_t *list,
                               savesync_location_t loc) {
	DIR *d = opendir(root);
	if (!d) {
		scan_log("scan_savedata_root: opendir('%s') = NULL errno=%d", root, errno);
		return;
	}
	scan_log("scan_savedata_root: opendir('%s') OK", root);
	int considered = 0, accepted = 0, inspected_ok = 0;
	struct dirent *de;
	/* Heap-allocate the big locals — PPU main thread stack is limited and
	 * pushing savesync_save_t (~2.4 KB) + abs[1024] per iteration on top of
	 * main()'s AppState frame faulted the inspect_dir prologue. */
	char *abs = (char *)malloc(SAVESYNC_PATH_LEN);
	savesync_save_t *s = (savesync_save_t *)malloc(sizeof(*s));
	if (!abs || !s) {
		scan_log("scan_savedata_root: malloc failed abs=%p s=%p",
			(void *)abs, (void *)s);
		free(abs); free(s);
		closedir(d);
		return;
	}
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		considered++;
		if (!is_savedata_dir(root, de->d_name, abs, SAVESYNC_PATH_LEN)) {
			continue;
		}
		accepted++;
		int rc = savesync_inspect_dir(abs, s);
		if (rc == 0) {
			s->location = loc;
			list_push(list, s);
			inspected_ok++;
		} else {
			scan_log("  inspect_dir('%s') failed rc=%d", abs, rc);
		}
	}
	free(abs);
	free(s);
	closedir(d);
	scan_log("scan_savedata_root: '%s' considered=%d accepted=%d listed=%d",
	         root, considered, accepted, inspected_ok);
}

/* Try one user-dir candidate directly (no parent enumeration).
 * Apollo uses this approach because sandboxed selfs cannot opendir(/dev_hdd0/home/). */
static int probe_user_savedata(uint32_t uid, savesync_save_list_t *list) {
	char savedata[SAVESYNC_PATH_LEN];
	snprintf(savedata, sizeof(savedata), "/dev_hdd0/home/%08u/savedata", (unsigned)uid);
	struct stat st;
	if (stat(savedata, &st) != 0) return 0;
	if (!S_ISDIR(st.st_mode)) return 0;
	scan_log("probe_user_savedata: uid=%08u exists, scanning", (unsigned)uid);
	scan_savedata_root(savedata, list, SAVESYNC_LOCATION_HDD);
	return 1;
}

int savesync_scan_hdd(savesync_save_list_t *list) {
	/* Diagnostic: stat parent dirs to map the access boundary. */
	{
		struct stat st;
		const char *probes[] = {
			"/dev_hdd0",
			"/dev_hdd0/home",
			"/dev_hdd0/home/00000018",
			"/dev_hdd0/home/00000018/savedata",
			"/dev_hdd0/savedata",
			"/dev_hdd0/game",
		};
		for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
			int rc = stat(probes[i], &st);
			scan_log("probe stat('%s') rc=%d errno=%d mode=%o",
			         probes[i], rc, rc ? errno : 0,
			         rc ? 0 : (unsigned)(st.st_mode & 0777));
		}
	}

	const char *home = "/dev_hdd0/home";
	DIR *d = opendir(home);
	if (!d) {
		scan_log("scan_hdd: opendir('%s') = NULL errno=%d — falling back to direct probe",
		         home, errno);
		/* Fallback: probe known user IDs directly. PS3 user IDs start at 00000001
		 * and increment per-user; rarely beyond ~16. Iterate generously. */
		int found = 0;
		for (uint32_t uid = 1; uid <= 32; uid++) {
			found += probe_user_savedata(uid, list);
		}
		scan_log("scan_hdd: direct-probe found %d user dir(s)", found);
		return found > 0 ? 0 : -1;
	}
	scan_log("scan_hdd: opendir('%s') OK", home);

	int users_found = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		/* user dirs are 8 ascii digits */
		int ok = 1;
		for (int i = 0; i < 8 && de->d_name[i]; i++) {
			if (!isdigit((unsigned char)de->d_name[i])) { ok = 0; break; }
		}
		if (!ok || strlen(de->d_name) != 8) {
			scan_log("  skip non-user entry '%s'", de->d_name);
			continue;
		}
		users_found++;

		char user_dir[SAVESYNC_PATH_LEN];
		char savedata[SAVESYNC_PATH_LEN];
		scan_pjoin(user_dir, sizeof(user_dir), home, de->d_name);
		scan_pjoin(savedata, sizeof(savedata), user_dir, "savedata");
		scan_savedata_root(savedata, list, SAVESYNC_LOCATION_HDD);
	}
	closedir(d);
	scan_log("scan_hdd: enumerated %d user dir(s)", users_found);
	return 0;
}

int savesync_scan_usb(savesync_save_list_t *list) {
	scan_log("scan_usb: probing /dev_usb000..7/PS3/SAVEDATA");
	for (int i = 0; i < 8; i++) {
		char root[SAVESYNC_PATH_LEN];
		snprintf(root, sizeof(root), "/dev_usb00%d/PS3/SAVEDATA", i);
		scan_savedata_root(root, list, SAVESYNC_LOCATION_USB);
	}
	return 0;
}

const char *savesync_flavor_str(savesync_flavor_t f) {
	switch (f) {
		case SAVESYNC_FLAVOR_PS3_SIGNED:   return "ps3-signed";
		case SAVESYNC_FLAVOR_PS3_UNSIGNED: return "ps3-unsigned";
		case SAVESYNC_FLAVOR_RPCS3:        return "rpcs3";
		default:                           return "unknown";
	}
}

const char *savesync_location_str(savesync_location_t l) {
	switch (l) {
		case SAVESYNC_LOCATION_HDD:           return "hdd";
		case SAVESYNC_LOCATION_USB:           return "usb";
		case SAVESYNC_LOCATION_RPCS3_BUNDLE:  return "rpcs3-bundle";
		default:                              return "unknown";
	}
}
