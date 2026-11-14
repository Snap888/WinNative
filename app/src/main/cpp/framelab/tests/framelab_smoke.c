#define _POSIX_C_SOURCE 200809L
#include "anr_frame_lab.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[] = "/tmp/anr-frame-lab-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    const unsigned char pixels[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const unsigned char params[] = {9, 8, 7};
    assert(anr_fl_start(dir) == 0);
    assert(anr_fl_submit(ANR_FL_STAGE_FRAMEGEN_INPUT, 42, 123456789ULL,
                         2, 1, 1, pixels, sizeof(pixels), params, sizeof(params)) == 0);
    assert(anr_fl_stop() == 0);
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
