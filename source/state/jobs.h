#ifndef SAVESYNC_STATE_JOBS_H
#define SAVESYNC_STATE_JOBS_H

#include <stddef.h>
#include <stdint.h>

#include "savedata.h"
#include "convert.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAVESYNC_MAX_JOBS 16
#define SAVESYNC_JOB_ID_LEN 16
#define SAVESYNC_JOB_LABEL_LEN 128
#define SAVESYNC_JOB_URL_LEN 192

typedef enum {
	SAVESYNC_JOB_KIND_EXPORT = 0,   /* PS3 save dir -> .zip download */
	SAVESYNC_JOB_KIND_IMPORT,       /* uploaded .zip -> install to PS3 */
	SAVESYNC_JOB_KIND_CONVERT,      /* in-place transform */
} savesync_job_kind_t;

typedef enum {
	SAVESYNC_JOB_PENDING = 0,
	SAVESYNC_JOB_RUNNING,
	SAVESYNC_JOB_DONE,
	SAVESYNC_JOB_FAILED,
	SAVESYNC_JOB_CANCELLED,
} savesync_job_state_t;

typedef struct {
	char id[SAVESYNC_JOB_ID_LEN];
	savesync_job_kind_t kind;
	savesync_job_state_t state;
	char label[SAVESYNC_JOB_LABEL_LEN];
	char source_dir[SAVESYNC_PATH_LEN];
	char zip_path[SAVESYNC_PATH_LEN];
	savesync_convert_options_t options;
	savesync_progress_t progress;
	int64_t created_at;
	int64_t finished_at;
	char download_url[SAVESYNC_JOB_URL_LEN];
} savesync_job_t;

int  savesync_jobs_init(void);
void savesync_jobs_shutdown(void);

/* Enqueue. Returns 0 on success and writes new id to out_id. -1 if queue full. */
int savesync_jobs_enqueue_export(const char *save_dir, const char *label,
                                 char out_id[SAVESYNC_JOB_ID_LEN]);
int savesync_jobs_enqueue_import(const char *zip_path,
                                 const savesync_convert_options_t *opts,
                                 const char *label,
                                 char out_id[SAVESYNC_JOB_ID_LEN]);
int savesync_jobs_enqueue_convert(const char *save_dir,
                                  const savesync_convert_options_t *opts,
                                  const char *label,
                                  char out_id[SAVESYNC_JOB_ID_LEN]);

/* Snapshot all jobs under lock; returns count copied. */
size_t savesync_jobs_snapshot(savesync_job_t *out, size_t capacity);
int    savesync_jobs_get(const char *id, savesync_job_t *out);
int    savesync_jobs_remove(const char *id);
int    savesync_jobs_cancel(const char *id);

#ifdef __cplusplus
}
#endif

#endif
