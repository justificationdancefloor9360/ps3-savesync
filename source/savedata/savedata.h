#ifndef SAVESYNC_SAVEDATA_H
#define SAVESYNC_SAVEDATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAVESYNC_TITLE_ID_LEN  10
#define SAVESYNC_DIR_NAME_LEN  64
#define SAVESYNC_TITLE_LEN     128
#define SAVESYNC_SUBTITLE_LEN  128
#define SAVESYNC_DETAIL_LEN    1024
#define SAVESYNC_PATH_LEN      1024
#define SAVESYNC_ACCOUNT_ID_HEX 17

typedef enum {
	SAVESYNC_FLAVOR_UNKNOWN = 0,
	SAVESYNC_FLAVOR_PS3_SIGNED,   /* has valid PARAM.PFD with HMAC entries */
	SAVESYNC_FLAVOR_PS3_UNSIGNED, /* PARAM.SFO + PARAM.PFD but PFD looks bad */
	SAVESYNC_FLAVOR_RPCS3,        /* no PARAM.PFD, plain files (RPCS3 layout) */
} savesync_flavor_t;

typedef enum {
	SAVESYNC_LOCATION_HDD = 0,
	SAVESYNC_LOCATION_USB,
	SAVESYNC_LOCATION_RPCS3_BUNDLE, /* uploaded zip extracted to tmp */
} savesync_location_t;

typedef struct {
	char dir_name[SAVESYNC_DIR_NAME_LEN];      /* e.g. BLES01807-MYSAVE00 */
	char title_id[SAVESYNC_TITLE_ID_LEN];      /* e.g. BLES01807 */
	char title[SAVESYNC_TITLE_LEN];            /* SFO TITLE (game name) */
	char subtitle[SAVESYNC_SUBTITLE_LEN];      /* SFO SUB_TITLE (slot/level) */
	char detail[SAVESYNC_DETAIL_LEN];          /* SFO DETAIL */
	char account_id_hex[SAVESYNC_ACCOUNT_ID_HEX]; /* 16 hex chars + NUL, "" if none */
	char path[SAVESYNC_PATH_LEN];              /* absolute path to save dir */
	uint64_t total_size_bytes;
	uint32_t file_count;
	savesync_flavor_t flavor;
	savesync_location_t location;
	int has_icon0;
} savesync_save_t;

typedef struct savesync_save_list {
	savesync_save_t *items;
	size_t count;
	size_t capacity;
} savesync_save_list_t;

void savesync_save_list_init(savesync_save_list_t *list);
void savesync_save_list_free(savesync_save_list_t *list);

/* Scan all known PS3 user dirs under /dev_hdd0/home/<user>/savedata. Appends to list. */
int savesync_scan_hdd(savesync_save_list_t *list);

/* Scan /dev_usb000..007 PS3/SAVEDATA dirs. Appends to list. */
int savesync_scan_usb(savesync_save_list_t *list);

/* Inspect a single save dir; fills `out`. Returns 0 on success. */
int savesync_inspect_dir(const char *abs_path, savesync_save_t *out);

const char *savesync_flavor_str(savesync_flavor_t f);
const char *savesync_location_str(savesync_location_t l);

#ifdef __cplusplus
}
#endif

#endif
