#include "jobs.h"
#include "savesync.h"
#include "convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/systime.h>

typedef struct {
	savesync_job_t job;
	int in_use;
	int cancel_requested;
} job_slot_t;

static job_slot_t      g_slots[SAVESYNC_MAX_JOBS];
static sys_mutex_t     g_mutex;
static sys_ppu_thread_t g_worker;
static volatile int    g_worker_running = 0;
static volatile int    g_shutdown = 0;
static uint32_t        g_id_counter = 0;

static int64_t now_ms(void) {
	return (int64_t)((uint64_t)sysGetSystemTime() / 1000);
}

static void make_id(char out[SAVESYNC_JOB_ID_LEN]) {
	g_id_counter++;
	snprintf(out, SAVESYNC_JOB_ID_LEN, "j-%04u", (unsigned)g_id_counter);
}

static job_slot_t *find_free_slot_locked(void) {
	size_t i;
	for (i = 0; i < SAVESYNC_MAX_JOBS; i++) {
		if (!g_slots[i].in_use) return &g_slots[i];
	}
	return NULL;
}

static job_slot_t *find_slot_by_id_locked(const char *id) {
	size_t i;
	for (i = 0; i < SAVESYNC_MAX_JOBS; i++) {
		if (g_slots[i].in_use && strcmp(g_slots[i].job.id, id) == 0)
			return &g_slots[i];
	}
	return NULL;
}

static job_slot_t *find_pending_locked(void) {
	size_t i;
	for (i = 0; i < SAVESYNC_MAX_JOBS; i++) {
		if (g_slots[i].in_use && g_slots[i].job.state == SAVESYNC_JOB_PENDING)
			return &g_slots[i];
	}
	return NULL;
}

/* Progress callback bridge: single worker, so a file-scope pointer is fine. */
static job_slot_t *g_active_slot = NULL;

static void on_progress(const savesync_progress_t *p, void *user_unused) {
	(void)user_unused;
	if (!g_active_slot) return;
	sysMutexLock(g_mutex, 0);
	g_active_slot->job.progress = *p;
	if (p->phase == SAVESYNC_PHASE_DONE) {
		g_active_slot->job.state = SAVESYNC_JOB_DONE;
		g_active_slot->job.finished_at = now_ms();
	} else if (p->phase == SAVESYNC_PHASE_FAILED) {
		g_active_slot->job.state = SAVESYNC_JOB_FAILED;
		g_active_slot->job.finished_at = now_ms();
	}
	sysMutexUnlock(g_mutex);
}

static void run_one_job(job_slot_t *slot) {
	savesync_job_t snap;
	int rc = -1;

	g_active_slot = slot;

	sysMutexLock(g_mutex, 0);
	slot->job.state = SAVESYNC_JOB_RUNNING;
	snap = slot->job;
	sysMutexUnlock(g_mutex);

	switch (snap.kind) {
		case SAVESYNC_JOB_KIND_EXPORT:
			rc = savesync_export_zip(snap.source_dir, snap.zip_path,
			                         on_progress, NULL);
			if (rc == 0) {
				sysMutexLock(g_mutex, 0);
				snprintf(slot->job.download_url, SAVESYNC_JOB_URL_LEN,
				         "/api/jobs/%s/download", slot->job.id);
				sysMutexUnlock(g_mutex);
			}
			break;
		case SAVESYNC_JOB_KIND_IMPORT:
			rc = savesync_import_zip(snap.zip_path, &snap.options,
			                          on_progress, NULL);
			break;
		case SAVESYNC_JOB_KIND_CONVERT:
			rc = savesync_convert_dir(snap.source_dir, &snap.options,
			                           on_progress, NULL);
			break;
	}

	/* If callback didn't already set terminal state, do it now */
	sysMutexLock(g_mutex, 0);
	if (slot->job.state == SAVESYNC_JOB_RUNNING) {
		slot->job.state = (rc == 0) ? SAVESYNC_JOB_DONE : SAVESYNC_JOB_FAILED;
		slot->job.finished_at = now_ms();
	}
	sysMutexUnlock(g_mutex);

	g_active_slot = NULL;
}

static void worker_main(void *arg_unused) {
	(void)arg_unused;
	while (!g_shutdown) {
		job_slot_t *next;
		sysMutexLock(g_mutex, 0);
		next = find_pending_locked();
		sysMutexUnlock(g_mutex);

		if (!next) {
			sysUsleep(150 * 1000);
			continue;
		}
		run_one_job(next);
	}
	g_worker_running = 0;
	sysThreadExit(0);
}

int savesync_jobs_init(void) {
	sys_mutex_attr_t attr;

	memset(g_slots, 0, sizeof(g_slots));
	g_id_counter = 0;
	g_shutdown = 0;

	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol  = SYS_MUTEX_PROTOCOL_FIFO;
	attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	attr.attr_pshared   = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive  = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strcpy(attr.name, "ssyncj");

	if (sysMutexCreate(&g_mutex, &attr) != 0) return -1;

	g_worker_running = 1;
	if (sysThreadCreate(&g_worker, worker_main, NULL, 1500, 64 * 1024,
	                    THREAD_JOINABLE, (char *)"savesync_worker") != 0) {
		g_worker_running = 0;
		sysMutexDestroy(g_mutex);
		return -1;
	}
	return 0;
}

void savesync_jobs_shutdown(void) {
	g_shutdown = 1;
	if (g_worker_running) {
		uint64_t rv = 0;
		sysThreadJoin(g_worker, &rv);
	}
	sysMutexDestroy(g_mutex);
}

static int enqueue_common(savesync_job_kind_t kind,
                          const char *source_dir, const char *zip_path,
                          const savesync_convert_options_t *opts,
                          const char *label,
                          char out_id[SAVESYNC_JOB_ID_LEN]) {
	job_slot_t *slot;

	sysMutexLock(g_mutex, 0);
	slot = find_free_slot_locked();
	if (!slot) {
		sysMutexUnlock(g_mutex);
		return -1;
	}
	memset(slot, 0, sizeof(*slot));
	slot->in_use = 1;
	slot->job.kind = kind;
	slot->job.state = SAVESYNC_JOB_PENDING;
	slot->job.created_at = now_ms();
	make_id(slot->job.id);

	if (source_dir) strncpy(slot->job.source_dir, source_dir, SAVESYNC_PATH_LEN - 1);
	if (zip_path)   strncpy(slot->job.zip_path,   zip_path,   SAVESYNC_PATH_LEN - 1);
	if (opts)        slot->job.options = *opts;
	if (label)       strncpy(slot->job.label, label, SAVESYNC_JOB_LABEL_LEN - 1);

	memcpy(out_id, slot->job.id, SAVESYNC_JOB_ID_LEN);
	sysMutexUnlock(g_mutex);
	return 0;
}

int savesync_jobs_enqueue_export(const char *save_dir, const char *label,
                                  char out_id[SAVESYNC_JOB_ID_LEN]) {
	const char *base = strrchr(save_dir, '/');
	char zip[SAVESYNC_PATH_LEN];
	base = base ? base + 1 : save_dir;
	snprintf(zip, sizeof(zip), "%s/%s.zip", SAVESYNC_TMP_DIR, base);
	return enqueue_common(SAVESYNC_JOB_KIND_EXPORT, save_dir, zip,
	                      NULL, label, out_id);
}

int savesync_jobs_enqueue_import(const char *zip_path,
                                  const savesync_convert_options_t *opts,
                                  const char *label,
                                  char out_id[SAVESYNC_JOB_ID_LEN]) {
	return enqueue_common(SAVESYNC_JOB_KIND_IMPORT, NULL, zip_path,
	                      opts, label, out_id);
}

int savesync_jobs_enqueue_convert(const char *save_dir,
                                   const savesync_convert_options_t *opts,
                                   const char *label,
                                   char out_id[SAVESYNC_JOB_ID_LEN]) {
	return enqueue_common(SAVESYNC_JOB_KIND_CONVERT, save_dir, NULL,
	                      opts, label, out_id);
}

size_t savesync_jobs_snapshot(savesync_job_t *out, size_t capacity) {
	size_t i, n = 0;
	sysMutexLock(g_mutex, 0);
	for (i = 0; i < SAVESYNC_MAX_JOBS && n < capacity; i++) {
		if (g_slots[i].in_use) out[n++] = g_slots[i].job;
	}
	sysMutexUnlock(g_mutex);
	return n;
}

int savesync_jobs_get(const char *id, savesync_job_t *out) {
	int rc = -1;
	sysMutexLock(g_mutex, 0);
	{
		job_slot_t *s = find_slot_by_id_locked(id);
		if (s) { *out = s->job; rc = 0; }
	}
	sysMutexUnlock(g_mutex);
	return rc;
}

int savesync_jobs_remove(const char *id) {
	int rc = -1;
	sysMutexLock(g_mutex, 0);
	{
		job_slot_t *s = find_slot_by_id_locked(id);
		if (s && (s->job.state == SAVESYNC_JOB_DONE    ||
		          s->job.state == SAVESYNC_JOB_FAILED   ||
		          s->job.state == SAVESYNC_JOB_CANCELLED)) {
			memset(s, 0, sizeof(*s));
			rc = 0;
		}
	}
	sysMutexUnlock(g_mutex);
	return rc;
}

int savesync_jobs_cancel(const char *id) {
	int rc = -1;
	sysMutexLock(g_mutex, 0);
	{
		job_slot_t *s = find_slot_by_id_locked(id);
		if (s && s->job.state == SAVESYNC_JOB_PENDING) {
			s->job.state = SAVESYNC_JOB_CANCELLED;
			s->job.finished_at = now_ms();
			rc = 0;
		}
	}
	sysMutexUnlock(g_mutex);
	return rc;
}
