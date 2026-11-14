#define _POSIX_C_SOURCE 200809L
#include "anr_frame_lab.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define ANR_FL_QUEUE_CAPACITY 3u
#define ANR_FL_MAX_PAYLOAD (256u * 1024u * 1024u)
#define ANR_FL_MAGIC 0x31464e41u /* "ANF1" little-endian */

typedef struct frame_slot {
    uint32_t stage, width, height, format;
    uint64_t frame_id, timestamp_ns;
    uint8_t *pixels, *params;
    size_t pixel_bytes, params_bytes;
} frame_slot;

typedef struct frame_index {
    uint32_t magic, stage, width, height, format, reserved;
    uint64_t frame_id, timestamp_ns, data_offset, data_bytes;
    uint64_t params_offset, params_bytes;
} frame_index;

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t worker;
    int initialized, running, stopping, worker_error;
    FILE *index_file, *frames_file, *params_file, *manifest_file;
    frame_slot queue[ANR_FL_QUEUE_CAPACITY];
    unsigned head, count;
    anr_fl_stats stats;
} g = { .mutex = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER };

static void free_slot(frame_slot *slot) {
    free(slot->pixels);
    free(slot->params);
    memset(slot, 0, sizeof(*slot));
}

static void *writer_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g.mutex);
        while (g.count == 0 && !g.stopping)
            pthread_cond_wait(&g.ready, &g.mutex);
        if (g.count == 0 && g.stopping) {
            pthread_mutex_unlock(&g.mutex);
            break;
        }
        frame_slot slot = g.queue[g.head];
        memset(&g.queue[g.head], 0, sizeof(frame_slot));
        g.head = (g.head + 1u) % ANR_FL_QUEUE_CAPACITY;
        --g.count;
        pthread_mutex_unlock(&g.mutex);

        off_t data_offset = ftello(g.frames_file);
        off_t params_offset = ftello(g.params_file);
        frame_index ix;
        memset(&ix, 0, sizeof(ix));
        ix.magic = ANR_FL_MAGIC;
        ix.stage = slot.stage;
        ix.width = slot.width;
        ix.height = slot.height;
        ix.format = slot.format;
        ix.frame_id = slot.frame_id;
        ix.timestamp_ns = slot.timestamp_ns;
        ix.data_offset = data_offset < 0 ? 0u : (uint64_t)data_offset;
        ix.data_bytes = slot.pixel_bytes;
        ix.params_offset = params_offset < 0 ? 0u : (uint64_t)params_offset;
        ix.params_bytes = slot.params_bytes;

        int failed = data_offset < 0 || params_offset < 0 ||
            fwrite(slot.pixels, 1, slot.pixel_bytes, g.frames_file) != slot.pixel_bytes ||
            (slot.params_bytes && fwrite(slot.params, 1, slot.params_bytes, g.params_file) != slot.params_bytes) ||
            fwrite(&ix, sizeof(ix), 1, g.index_file) != 1;
        if (failed) {
            pthread_mutex_lock(&g.mutex);
            g.worker_error = errno ? errno : EIO;
            g.stats.last_error = g.worker_error;
            /* A failed stream is no longer appendable as a valid recording. Stop accepting
             * queued records and account for them as dropped instead of writing past a hole. */
            g.stopping = 1;
            while (g.count) {
                frame_slot *pending = &g.queue[g.head];
                free_slot(pending);
                g.head = (g.head + 1u) % ANR_FL_QUEUE_CAPACITY;
                --g.count;
                ++g.stats.dropped;
            }
            pthread_cond_broadcast(&g.ready);
            pthread_mutex_unlock(&g.mutex);
        } else {
            pthread_mutex_lock(&g.mutex);
            ++g.stats.written;
            g.stats.bytes_written += slot.pixel_bytes + slot.params_bytes;
            pthread_mutex_unlock(&g.mutex);
        }
        free_slot(&slot);
    }
    return NULL;
}

int anr_fl_start(const char *directory) {
    if (!directory || !directory[0]) return EINVAL;
    pthread_mutex_lock(&g.mutex);
    if (g.running || g.initialized) {
        pthread_mutex_unlock(&g.mutex);
        return EBUSY;
    }
    char path[1024];
    struct stat st;
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        int e = errno ? errno : ENOTDIR;
        pthread_mutex_unlock(&g.mutex);
        return e;
    }
#define OPEN_FILE(member, name, mode) do { \
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) goto path_too_long; \
    g.member = fopen(path, mode); \
    if (!g.member) goto open_failed; \
} while (0)
    OPEN_FILE(manifest_file, "record.json", "wb");
    OPEN_FILE(index_file, "frames.idx", "wb");
    OPEN_FILE(frames_file, "frames.bin", "wb");
    OPEN_FILE(params_file, "params.bin", "wb");
#undef OPEN_FILE
    if (fputs("{\"format\":\"ANR-FL-1\",\"payload\":\"lossless-raw\",\"index\":\"frames.idx\",\"frames\":\"frames.bin\",\"params\":\"params.bin\"}\n", g.manifest_file) < 0)
        goto open_failed;
    fflush(g.manifest_file);
    memset(&g.stats, 0, sizeof(g.stats));
    g.head = g.count = 0;
    g.stopping = g.worker_error = 0;
    g.initialized = 1;
    int thread_error = pthread_create(&g.worker, NULL, writer_main, NULL);
    if (thread_error != 0) {
        g.initialized = 0;
        errno = thread_error;
        goto open_failed;
    }
    g.running = 1;
    pthread_mutex_unlock(&g.mutex);
    return 0;

path_too_long:
    errno = ENAMETOOLONG;
open_failed: {
    int e = errno ? errno : EIO;
    if (g.params_file) fclose(g.params_file);
    if (g.frames_file) fclose(g.frames_file);
    if (g.index_file) fclose(g.index_file);
    if (g.manifest_file) fclose(g.manifest_file);
    g.params_file = g.frames_file = g.index_file = g.manifest_file = NULL;
    pthread_mutex_unlock(&g.mutex);
    return e;
}
}

int anr_fl_submit(uint32_t stage, uint64_t frame_id, uint64_t timestamp_ns,
                  uint32_t width, uint32_t height, uint32_t format,
                  const void *pixels, size_t pixel_bytes,
                  const void *params, size_t params_bytes) {
    if (stage < ANR_FL_STAGE_SOURCE || stage > ANR_FL_STAGE_FINAL_OUTPUT ||
        !width || !height || !pixels || !pixel_bytes || pixel_bytes > ANR_FL_MAX_PAYLOAD ||
        (params_bytes && !params) || params_bytes > 1024u * 1024u)
        return EINVAL;
    frame_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.pixels = (uint8_t *)malloc(pixel_bytes);
    slot.params = params_bytes ? (uint8_t *)malloc(params_bytes) : NULL;
    if (!slot.pixels || (params_bytes && !slot.params)) {
        free_slot(&slot);
        return ENOMEM;
    }
    memcpy(slot.pixels, pixels, pixel_bytes);
    if (params_bytes) memcpy(slot.params, params, params_bytes);
    slot.stage = stage; slot.frame_id = frame_id; slot.timestamp_ns = timestamp_ns;
    slot.width = width; slot.height = height; slot.format = format;
    slot.pixel_bytes = pixel_bytes; slot.params_bytes = params_bytes;

    pthread_mutex_lock(&g.mutex);
    if (!g.running || g.stopping || g.worker_error) {
        int e = g.worker_error ? g.worker_error : EPIPE;
        pthread_mutex_unlock(&g.mutex);
        free_slot(&slot);
        return e;
    }
    if (g.count == ANR_FL_QUEUE_CAPACITY) {
        ++g.stats.dropped;
        pthread_mutex_unlock(&g.mutex);
        free_slot(&slot);
        return EAGAIN;
    }
    unsigned tail = (g.head + g.count) % ANR_FL_QUEUE_CAPACITY;
    g.queue[tail] = slot;
    ++g.count;
    ++g.stats.submitted;
    pthread_cond_signal(&g.ready);
    pthread_mutex_unlock(&g.mutex);
    return 0;
}


int anr_fl_submit_configured(uint32_t stage, uint64_t frame_id, uint64_t timestamp_ns,
                             uint32_t width, uint32_t height, uint32_t format,
                             const void *pixels, size_t pixel_bytes,
                             const anr_fl_config_v1 *config) {
    if (!config || config->version != ANR_FL_CONFIG_VERSION ||
        config->struct_size != sizeof(*config) ||
        config->effect_count > ANR_FL_CONFIG_MAX_EFFECTS)
        return EINVAL;
    return anr_fl_submit(stage, frame_id, timestamp_ns, width, height, format,
                         pixels, pixel_bytes, config, sizeof(*config));
}

void anr_fl_get_stats(anr_fl_stats *out_stats) {
    if (!out_stats) return;
    pthread_mutex_lock(&g.mutex);
    *out_stats = g.stats;
    if (g.worker_error) out_stats->last_error = g.worker_error;
    pthread_mutex_unlock(&g.mutex);
}

int anr_fl_stop(void) {
    pthread_mutex_lock(&g.mutex);
    if (!g.running) {
        pthread_mutex_unlock(&g.mutex);
        return 0;
    }
    g.stopping = 1;
    pthread_cond_broadcast(&g.ready);
    pthread_mutex_unlock(&g.mutex);
    pthread_join(g.worker, NULL);

    pthread_mutex_lock(&g.mutex);
    int result = g.worker_error;
    if (fflush(g.frames_file) != 0 && !result) result = errno ? errno : EIO;
    if (fflush(g.params_file) != 0 && !result) result = errno ? errno : EIO;
    if (fflush(g.index_file) != 0 && !result) result = errno ? errno : EIO;
    if (fflush(g.manifest_file) != 0 && !result) result = errno ? errno : EIO;
    if (fclose(g.params_file) != 0 && !result) result = errno ? errno : EIO;
    if (fclose(g.frames_file) != 0 && !result) result = errno ? errno : EIO;
    if (fclose(g.index_file) != 0 && !result) result = errno ? errno : EIO;
    if (fclose(g.manifest_file) != 0 && !result) result = errno ? errno : EIO;
    g.params_file = g.frames_file = g.index_file = g.manifest_file = NULL;
    g.running = g.stopping = g.initialized = 0;
    pthread_mutex_unlock(&g.mutex);
    return result;
}
