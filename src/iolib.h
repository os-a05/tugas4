#ifndef IOLIB_H
#define IOLIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t write_ios;
    uint64_t write_merges;
    uint64_t write_sectors;
} io_device_stats;

typedef struct {
    double average;
    uint64_t minimum;
    uint64_t maximum;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
} io_latency_stats;

/* Nanoseconds since an unspecified monotonic epoch; returns 0 on failure. */
uint64_t io_now_ns(void);

/* Values are reported in kB, as in /proc/meminfo. */
int io_read_memory_metrics(uint64_t *dirty_kb, uint64_t *writeback_kb);

/* Read the first three write fields from /sys/block/<device>/stat. */
int io_read_device_stats(const char *device, io_device_stats *stats);

int io_calculate_average(const uint64_t *samples, size_t count, double *average);
int io_calculate_min_max(const uint64_t *samples, size_t count,
                         uint64_t *minimum, uint64_t *maximum);
int io_compare_u64(const void *a, const void *b);
int io_calculate_percentile(const uint64_t *sorted_samples, size_t count,
                            unsigned int percentile, uint64_t *result);
/* Calculate average, min, max, and nearest-rank percentiles. */
int io_calculate_latency_stats(const uint64_t *samples, size_t count,
                               io_latency_stats *stats);

/* Create logs/ if needed and open a timestamped CSV. */
FILE *io_csv_open(const char *group, const char *program,
                  const char *scenario, char *path, size_t path_size);
int io_csv_write_header(FILE *stream);
int io_csv_write_sample(FILE *stream, uint64_t run_id,
                        const char *scenario, uint64_t op_index,
                        uint64_t latency_ns, uint64_t bytes);

/* Write uname, memory, block-device, and dirty-page settings to logs/. */
int io_write_environment(const char *group, char *path, size_t path_size);

#ifdef __cplusplus
}
#endif

#endif
