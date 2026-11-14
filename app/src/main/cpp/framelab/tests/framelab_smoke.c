#define _POSIX_C_SOURCE 200809L
#include "anr_frame_lab.h"
#include "anr_frame_lab_metrics.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    const uint8_t a[] = {10,20,30,255, 40,50,60,255};
    const uint8_t same[] = {10,20,30,255, 40,50,60,255};
    const uint8_t changed[] = {10,20,30,255, 80,90,100,255};
    anr_fl_metrics metrics;
    assert(anr_fl_compare_rgba8(a, same, 2, 1, &metrics) == 0);
    assert(isinf(metrics.psnr_db) && metrics.ssim > 0.999);
    assert(anr_fl_compare_rgba8(a, changed, 2, 1, &metrics) == 0);
    assert(metrics.psnr_db < 100.0 && metrics.mean_absolute_error > 0.0);
    assert(anr_fl_temporal_rgba8(a, changed, 2, 1, &metrics) == 0);
    assert(metrics.temporal_flicker > 0.0 && metrics.temporal_stability < 1.0);
    char dir[] = "/tmp/anr-frame-lab-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    const unsigned char pixels[] = {0, 1, 2, 3, 4, 5, 6, 7};
    anr_fl_config_v1 config = {0};
    config.struct_size = sizeof(config);
    config.version = ANR_FL_CONFIG_VERSION;
    config.feature_flags = ANR_FL_CFG_SNAPFRAME | ANR_FL_CFG_WFG | ANR_FL_CFG_LSFG;
    config.engine = 2;
    config.snapframe_mode = 1;
    config.wfg_mode = 1;
    config.lsfg_mode = 1;
    config.multiplier = 2;
    config.target_fps = 60;
    config.flow_scale = 0.75f;
    config.effect_count = 1;
    config.effects[0].effect_id = 9;
    config.effects[0].params[0] = 0.5f;
    assert(anr_fl_start(dir) == 0);
    assert(anr_fl_submit_configured(ANR_FL_STAGE_FRAMEGEN_INPUT, 42, 123456789ULL,
                                    2, 1, 1, pixels, sizeof(pixels), &config) == 0);
    assert(anr_fl_stop() == 0);
    anr_fl_replay_session *session = NULL;
    assert(anr_fl_replay_open(dir, &session) == 0);
    anr_fl_replay_frame replayed;
    assert(anr_fl_replay_next(session, &replayed) == 1);
    assert(replayed.frame_id == 42 && replayed.width == 2 && replayed.params_bytes == sizeof(config));
    anr_fl_config_v1 *saved = (anr_fl_config_v1 *)replayed.params;
    assert(saved->version == ANR_FL_CONFIG_VERSION && saved->effect_count == 1);
    anr_fl_replay_frame_release(&replayed);
    assert(anr_fl_replay_next(session, &replayed) == 0);
    anr_fl_replay_close(session);
    anr_fl_stats stats;
    anr_fl_get_stats(&stats);
    assert(stats.submitted == 1 && stats.written == 1 && stats.dropped == 0);
    const char *names[] = {"record.json", "frames.idx", "frames.bin", "params.bin"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        struct stat st;
        assert(stat(path, &st) == 0 && st.st_size > 0);
        unlink(path);
    }
    rmdir(dir);
    puts("ANR Frame Lab smoke test passed");
    return 0;
}
