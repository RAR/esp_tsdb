// Host regression for sidecar header persistence.
//
// The hot write path persists the header to <db>.h0/.h1 instead of offset 0 of
// the database, because an in-place rewrite at offset 0 costs ~3.7 s on
// littlefs. The in-file header is only refreshed on close/sync, so it can lag.
// This checks the three cases that matters for:
//
//   1. clean close  -> in-file header current, sidecar removed, data intact
//   2. simulated crash (no close) -> sidecar is ahead, reopen must adopt it
//   3. corrupt sidecar -> ignored, in-file header used, no crash
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH "/tmp/tsdb_sidecar_test.tsdb"
#define NPARAM 4
static const char *names[NPARAM] = {"a", "b", "c", "d"};

static tsdb_t *open_db(void) {
    tsdb_config_t cfg = {0};
    cfg.filepath = PATH;
    cfg.num_params = NPARAM;
    cfg.param_names = names;
    cfg.max_records = 500;
    cfg.index_stride = 64;
    cfg.buffer_pool_size = 8 * 1024;
    return tsdb_open(&cfg);
}

static void cleanup(void) {
    unlink(PATH);
    unlink(PATH ".h0");
    unlink(PATH ".h1");
}

static int count_records(tsdb_t *db) {
    tsdb_stats_t st;
    if (tsdb_get_stats_h(db, &st) != ESP_OK) return -1;
    return (int) st.total_records;
}

int main(void) {
    int fails = 0;
    cleanup();

    // --- 1. clean close ---------------------------------------------------
    tsdb_t *db = open_db();
    if (db == NULL) { printf("open FAIL\n"); return 1; }
    int16_t v[NPARAM] = {1, 2, 3, 4};
    for (int i = 0; i < 50; i++) tsdb_write_h(db, 1000 + i, v);
    tsdb_close_h(db);

    db = open_db();
    int n = count_records(db);
    printf("1. clean close        : %d records (expect 50) %s\n", n, n == 50 ? "PASS" : "FAIL");
    if (n != 50) fails++;

    // --- 2. simulated crash: write more, never close ----------------------
    for (int i = 0; i < 30; i++) tsdb_write_h(db, 2000 + i, v);
    // Deliberately NOT tsdb_close_h(db) — that is the whole point. Leak the
    // handle, exactly as a power cut would.
    db = open_db();
    n = count_records(db);
    printf("2. crash, sidecar wins: %d records (expect 80) %s\n", n, n == 80 ? "PASS" : "FAIL");
    if (n != 80) fails++;
    tsdb_close_h(db);

    // --- 3. corrupt sidecar must be ignored, not fatal --------------------
    db = open_db();
    for (int i = 0; i < 10; i++) tsdb_write_h(db, 3000 + i, v);
    // Corrupt both slots; a clean close already removed any older pair.
    for (int slot = 0; slot < 2; slot++) {
        char p[256];
        snprintf(p, sizeof(p), "%s.h%d", PATH, slot);
        FILE *f = fopen(p, "r+b");
        if (f != NULL) { fseek(f, 8, SEEK_SET); fputc(0xFF, f); fputc(0xFF, f); fclose(f); }
    }
    db = open_db();
    n = count_records(db);
    // In-file header is from the last clean close (80). A corrupt sidecar must
    // not be adopted and must not crash the open.
    printf("3. corrupt sidecar    : %d records (expect 80, ignored) %s\n",
           n, n == 80 ? "PASS" : "FAIL");
    if (n != 80) fails++;
    tsdb_close_h(db);

    // --- 4. tsdb_peek_span reads the span without opening -----------------
    // State here: case 3 corrupted the sidecar, so its 3000-series writes were
    // correctly discarded and the file still ends where case 2 left it — 80
    // records, newest ts 2029. peek_span must agree with what a real open sees.
    tsdb_span_t span;
    esp_err_t sr = tsdb_peek_span(PATH, &span);
    int ok = (sr == ESP_OK && span.total_records == 80 &&
              span.newest_timestamp == 2029 && span.num_params == NPARAM);
    printf("4. peek_span clean    : rc=%d records=%lu newest=%lu (expect 80/2029) %s\n",
           (int) sr, (unsigned long) span.total_records,
           (unsigned long) span.newest_timestamp, ok ? "PASS" : "FAIL");
    if (!ok) fails++;

    // --- 5. peek_span must reflect the SIDECAR, not the stale in-file copy --
    // Write without closing, so the in-file header still says 80 while the
    // sidecar knows the newer records. This is the case the rolling-file boot
    // scan depends on: a device that lost power mid-file must still report the
    // span it actually has, or the scan will mis-select files for a query.
    db = open_db();
    for (int i = 0; i < 5; i++) tsdb_write_h(db, 4000 + i, v);
    sr = tsdb_peek_span(PATH, &span);
    ok = (sr == ESP_OK && span.newest_timestamp == 4004);
    printf("5. peek_span vs stale : rc=%d newest=%lu (expect 4004) %s\n",
           (int) sr, (unsigned long) span.newest_timestamp, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
    tsdb_close_h(db);

    // --- 6. peek_span on a missing file ------------------------------------
    sr = tsdb_peek_span("/tmp/tsdb_peek_no_such_file.tsdb", &span);
    ok = (sr == ESP_ERR_NOT_FOUND);
    printf("6. peek_span missing  : rc=%d (expect NOT_FOUND) %s\n",
           (int) sr, ok ? "PASS" : "FAIL");
    if (!ok) fails++;

    cleanup();
    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails != 0;
}
