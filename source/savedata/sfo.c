#include "sfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PARAM.SFO is always little-endian on disk. PSL1GHT targets PowerPC (BE),
 * so on-disk fields must be byte-swapped on read/write. */
static inline uint32_t le32_load(const void *p) {
	const uint8_t *b = (const uint8_t *)p;
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint16_t le16_load(const void *p) {
	const uint8_t *b = (const uint8_t *)p;
	return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline void le32_store(void *p, uint32_t v) {
	uint8_t *b = (uint8_t *)p;
	b[0] = (uint8_t)(v & 0xFF);
	b[1] = (uint8_t)((v >> 8) & 0xFF);
	b[2] = (uint8_t)((v >> 16) & 0xFF);
	b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void le16_store(void *p, uint16_t v) {
	uint8_t *b = (uint8_t *)p;
	b[0] = (uint8_t)(v & 0xFF);
	b[1] = (uint8_t)((v >> 8) & 0xFF);
}

#define SFO_MAGIC   0x46535000u
#define SFO_VERSION 0x0101u

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t key_table_offset;
	uint32_t data_table_offset;
	uint32_t num_entries;
} sfo_header_t;

typedef struct {
	uint16_t key_offset;
	uint16_t param_format;
	uint32_t param_length;
	uint32_t param_max_length;
	uint32_t data_offset;
} sfo_index_t;

#define PARAMS_BLOCK_SIZE   1024
#define ACCOUNT_ID_OFFSET   64  /* offset within PARAMS block where account_id lives (8 bytes) */

void sfo_init(sfo_t *sfo) {
	if (!sfo) return;
	sfo->head = NULL;
	sfo->count = 0;
}

void sfo_free(sfo_t *sfo) {
	if (!sfo) return;
	sfo_param_t *p = sfo->head;
	while (p) {
		sfo_param_t *next = p->next;
		free(p->key);
		free(p->value);
		free(p);
		p = next;
	}
	sfo->head = NULL;
	sfo->count = 0;
}

static sfo_param_t *sfo_find_mut(sfo_t *sfo, const char *key) {
	for (sfo_param_t *p = sfo->head; p; p = p->next) {
		if (strcmp(p->key, key) == 0) return p;
	}
	return NULL;
}

const sfo_param_t *sfo_find(const sfo_t *sfo, const char *key) {
	return sfo_find_mut((sfo_t *)sfo, key);
}

const char *sfo_get_str(const sfo_t *sfo, const char *key) {
	const sfo_param_t *p = sfo_find(sfo, key);
	if (!p) return NULL;
	if (p->format != SFO_PARAM_UTF8_S && p->format != SFO_PARAM_UTF8) return NULL;
	return (const char *)p->value;
}

int sfo_get_int(const sfo_t *sfo, const char *key, int default_value) {
	const sfo_param_t *p = sfo_find(sfo, key);
	if (!p || p->format != SFO_PARAM_INT32 || p->length < 4) return default_value;
	return (int)((uint32_t)p->value[0] |
	             ((uint32_t)p->value[1] << 8) |
	             ((uint32_t)p->value[2] << 16) |
	             ((uint32_t)p->value[3] << 24));
}

static int sfo_append(sfo_t *sfo, const char *key, uint16_t format,
                      const uint8_t *value, uint32_t length, uint32_t max_length) {
	sfo_param_t *p = (sfo_param_t *)calloc(1, sizeof(sfo_param_t));
	if (!p) return -1;
	p->key = strdup(key);
	p->format = format;
	p->length = length;
	p->max_length = max_length;
	p->value = (uint8_t *)malloc(max_length);
	if (!p->key || !p->value) {
		free(p->key); free(p->value); free(p);
		return -1;
	}
	memset(p->value, 0, max_length);
	memcpy(p->value, value, length);

	if (!sfo->head) {
		sfo->head = p;
	} else {
		sfo_param_t *tail = sfo->head;
		while (tail->next) tail = tail->next;
		tail->next = p;
	}
	sfo->count++;
	return 0;
}

int sfo_set_str(sfo_t *sfo, const char *key, const char *value, uint32_t max_length) {
	uint32_t len = (uint32_t)strlen(value) + 1;
	if (max_length < len) max_length = len;

	sfo_param_t *p = sfo_find_mut(sfo, key);
	if (p) {
		uint8_t *nv = (uint8_t *)realloc(p->value, max_length);
		if (!nv) return -1;
		memset(nv, 0, max_length);
		memcpy(nv, value, len);
		p->value = nv;
		p->length = len;
		p->max_length = max_length;
		p->format = SFO_PARAM_UTF8_S;
		return 0;
	}
	return sfo_append(sfo, key, SFO_PARAM_UTF8_S, (const uint8_t *)value, len, max_length);
}

int sfo_set_int(sfo_t *sfo, const char *key, int32_t value) {
	uint8_t buf[4];
	buf[0] = (uint8_t)(value & 0xFF);
	buf[1] = (uint8_t)((value >> 8) & 0xFF);
	buf[2] = (uint8_t)((value >> 16) & 0xFF);
	buf[3] = (uint8_t)((value >> 24) & 0xFF);

	sfo_param_t *p = sfo_find_mut(sfo, key);
	if (p) {
		memcpy(p->value, buf, 4);
		p->length = 4;
		p->max_length = 4;
		p->format = SFO_PARAM_INT32;
		return 0;
	}
	return sfo_append(sfo, key, SFO_PARAM_INT32, buf, 4, 4);
}

int sfo_get_account_id(const sfo_t *sfo, char hex_out[17]) {
	const sfo_param_t *p = sfo_find(sfo, "PARAMS");
	hex_out[0] = '\0';
	if (!p || p->length < ACCOUNT_ID_OFFSET + 8) return -1;
	for (int i = 0; i < 8; i++) {
		static const char hexd[] = "0123456789ABCDEF";
		uint8_t b = p->value[ACCOUNT_ID_OFFSET + i];
		hex_out[i * 2]     = hexd[(b >> 4) & 0xF];
		hex_out[i * 2 + 1] = hexd[b & 0xF];
	}
	hex_out[16] = '\0';
	return 0;
}

int sfo_set_account_id_zero(sfo_t *sfo) {
	sfo_param_t *p = sfo_find_mut(sfo, "PARAMS");
	if (!p || p->length < ACCOUNT_ID_OFFSET + 8) return -1;
	memset(p->value + ACCOUNT_ID_OFFSET, 0, 8);
	return 0;
}

int sfo_load(sfo_t *sfo, const char *path) {
	sfo_init(sfo);
	FILE *f = fopen(path, "rb");
	if (!f) return -1;

	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
	long fsz = ftell(f);
	if (fsz <= (long)sizeof(sfo_header_t)) { fclose(f); return -1; }
	rewind(f);

	uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
	if (!buf) { fclose(f); return -1; }
	if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) { free(buf); fclose(f); return -1; }
	fclose(f);

	/* PARAM.SFO header is little-endian; load each field with byte-swap. */
	sfo_header_t h;
	h.magic             = le32_load(buf + 0);
	h.version           = le32_load(buf + 4);
	h.key_table_offset  = le32_load(buf + 8);
	h.data_table_offset = le32_load(buf + 12);
	h.num_entries       = le32_load(buf + 16);
	if (h.magic != SFO_MAGIC) { free(buf); return -1; }
	if (h.key_table_offset > (uint32_t)fsz) { free(buf); return -1; }
	if (h.data_table_offset > (uint32_t)fsz) { free(buf); return -1; }

	const uint8_t *idx_base = buf + sizeof(sfo_header_t);
	const char *keys = (const char *)(buf + h.key_table_offset);
	const uint8_t *data = buf + h.data_table_offset;

	for (uint32_t i = 0; i < h.num_entries; i++) {
		const uint8_t *e = idx_base + i * sizeof(sfo_index_t);
		uint16_t key_off = le16_load(e + 0);
		uint16_t format  = le16_load(e + 2);
		uint32_t length  = le32_load(e + 4);
		uint32_t max_len = le32_load(e + 8);
		uint32_t data_off = le32_load(e + 12);

		if ((const char *)keys + key_off >= (const char *)(buf + fsz)) continue;
		if (data + data_off + length > buf + fsz) continue;

		const char *key = keys + key_off;
		const uint8_t *value = data + data_off;
		sfo_append(sfo, key, format, value, length, max_len);
	}

	free(buf);
	return 0;
}

int sfo_save(const sfo_t *sfo, const char *path) {
	if (!sfo || !sfo->head) return -1;

	uint32_t num = (uint32_t)sfo->count;
	uint32_t key_table_size = 0;
	uint32_t data_table_size = 0;
	for (sfo_param_t *p = sfo->head; p; p = p->next) {
		key_table_size += (uint32_t)strlen(p->key) + 1;
		data_table_size += p->max_length;
	}
	/* round key table up to 4 */
	uint32_t key_pad = (4 - (key_table_size & 3)) & 3;
	key_table_size += key_pad;

	uint32_t header_size = (uint32_t)sizeof(sfo_header_t) + num * (uint32_t)sizeof(sfo_index_t);
	uint32_t total = header_size + key_table_size + data_table_size;

	uint8_t *buf = (uint8_t *)calloc(1, total);
	if (!buf) return -1;

	uint32_t key_table_offset = header_size;
	uint32_t data_table_offset = header_size + key_table_size;

	/* PARAM.SFO is little-endian on disk; PSL1GHT runs big-endian PowerPC,
	 * so emit each header / index field via explicit LE stores. */
	le32_store(buf + 0,  SFO_MAGIC);
	le32_store(buf + 4,  SFO_VERSION);
	le32_store(buf + 8,  key_table_offset);
	le32_store(buf + 12, data_table_offset);
	le32_store(buf + 16, num);

	uint8_t *idx_base = buf + sizeof(sfo_header_t);
	uint8_t *keyp = buf + key_table_offset;
	uint8_t *datap = buf + data_table_offset;
	uint32_t key_off = 0, data_off = 0;
	uint32_t i = 0;

	for (sfo_param_t *p = sfo->head; p; p = p->next, i++) {
		uint32_t klen = (uint32_t)strlen(p->key) + 1;
		memcpy(keyp + key_off, p->key, klen);
		memcpy(datap + data_off, p->value, p->length);
		uint8_t *e = idx_base + i * sizeof(sfo_index_t);
		le16_store(e + 0,  (uint16_t)key_off);
		le16_store(e + 2,  p->format);
		le32_store(e + 4,  p->length);
		le32_store(e + 8,  p->max_length);
		le32_store(e + 12, data_off);
		key_off += klen;
		data_off += p->max_length;
	}

	FILE *f = fopen(path, "wb");
	if (!f) { free(buf); return -1; }
	size_t written = fwrite(buf, 1, total, f);
	fclose(f);
	free(buf);
	return (written == total) ? 0 : -1;
}

int sfo_remove(sfo_t *sfo, const char *key) {
	if (!sfo || !key) return -1;
	sfo_param_t *prev = NULL;
	for (sfo_param_t *p = sfo->head; p; prev = p, p = p->next) {
		if (strcmp(p->key, key) != 0) continue;
		if (prev) prev->next = p->next;
		else      sfo->head  = p->next;
		free(p->key);
		free(p->value);
		free(p);
		sfo->count--;
		return 0;
	}
	return -1;
}

int sfo_strip_rpcs3_specific(sfo_t *sfo) {
	if (!sfo) return -1;
	int removed = 0;
	/* Walk + collect, then delete — can't mutate while iterating. */
	for (;;) {
		sfo_param_t *target = NULL;
		for (sfo_param_t *p = sfo->head; p; p = p->next) {
			/* RPCS3 emits per-file protection flags as keys starting with '*' and
			 * a non-standard "RPCS3_BLIST" key. lv2 doesn't expect either. */
			if (p->key[0] == '*' || strcmp(p->key, "RPCS3_BLIST") == 0) {
				target = p;
				break;
			}
		}
		if (!target) break;
		sfo_remove(sfo, target->key);
		removed++;
	}
	return removed;
}
