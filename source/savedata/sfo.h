#ifndef SAVESYNC_SFO_H
#define SAVESYNC_SFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFO_PARAM_UTF8_S    0x0004 /* string, NUL-terminated */
#define SFO_PARAM_UTF8      0x0204 /* string, may not be NUL-terminated */
#define SFO_PARAM_INT32     0x0404

typedef struct sfo_param {
	char     *key;
	uint16_t  format;
	uint32_t  length;
	uint32_t  max_length;
	uint8_t  *value;
	struct sfo_param *next;
} sfo_param_t;

typedef struct {
	sfo_param_t *head;
	size_t count;
} sfo_t;

void sfo_init(sfo_t *sfo);
void sfo_free(sfo_t *sfo);

int sfo_load(sfo_t *sfo, const char *path);
int sfo_save(const sfo_t *sfo, const char *path);

const sfo_param_t *sfo_find(const sfo_t *sfo, const char *key);

/* Convenience: returns string value for utf-8 keys, or NULL. Borrowed pointer. */
const char *sfo_get_str(const sfo_t *sfo, const char *key);
int         sfo_get_int(const sfo_t *sfo, const char *key, int default_value);

/* Mutators: replace or insert. value is copied. */
int sfo_set_str(sfo_t *sfo, const char *key, const char *value, uint32_t max_length);
int sfo_set_int(sfo_t *sfo, const char *key, int32_t value);
int sfo_remove(sfo_t *sfo, const char *key);

/* Removes RPCS3-only entries (keys starting with '*' and "RPCS3_BLIST") so the
 * SFO is acceptable to lv2. Returns the number of entries removed. */
int sfo_strip_rpcs3_specific(sfo_t *sfo);

/* PARAM_PARAMS region: 1024-byte raw block keyed under "PARAMS". Contains account_id. */
int sfo_get_account_id(const sfo_t *sfo, char hex_out[17]);
int sfo_set_account_id_zero(sfo_t *sfo); /* clears account binding for cross-account sharing */

#ifdef __cplusplus
}
#endif

#endif
