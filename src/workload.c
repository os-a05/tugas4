#define _POSIX_C_SOURCE 200809L
#include "banner.h"
#include "iolib.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef enum {
    MODE_SEQWRITE,
    MODE_RANDWRITE,
    MODE_SEQREAD,
    MODE_RANDREAD
} workload_mode;

typedef struct {
    const char *directory;
    const char *device;
    uint64_t size_mb;
    uint64_t block_kb;
    workload_mode mode;
    int mode_set;
    int fsync_at_end;
} options;

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s --dir PATH --size MB --bs KB "
            "--mode seqwrite|randwrite|seqread|randread [--fsync] [--dev DEVICE]\n",
            program);
}

static int parse_positive_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || *text == '-') {
        return -1;
    }

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0) {
        return -1;
    }

    *value = (uint64_t)parsed;
    return 0;
}

static int parse_mode(const char *text, workload_mode *mode) {
    if (strcmp(text, "seqwrite") == 0) {
        *mode = MODE_SEQWRITE;
    } else if (strcmp(text, "randwrite") == 0) {
        *mode = MODE_RANDWRITE;
    } else if (strcmp(text, "seqread") == 0) {
        *mode = MODE_SEQREAD;
    } else if (strcmp(text, "randread") == 0) {
        *mode = MODE_RANDREAD;
    } else {
        return -1;
    }
    return 0;
}

static int parse_options(int argc, char **argv, options *opts) {
    int i;

    memset(opts, 0, sizeof(*opts));
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--fsync") == 0) {
            opts->fsync_at_end = 1;
        } else if (strcmp(argv[i], "--dir") == 0 ||
                   strcmp(argv[i], "--size") == 0 ||
                   strcmp(argv[i], "--bs") == 0 ||
                   strcmp(argv[i], "--mode") == 0 ||
                   strcmp(argv[i], "--dev") == 0) {
            const char *option = argv[i];
            const char *value;

            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", option);
                return -1;
            }
            value = argv[++i];

            if (strcmp(option, "--dir") == 0) {
                opts->directory = value;
            } else if (strcmp(option, "--size") == 0) {
                if (parse_positive_u64(value, &opts->size_mb) != 0) {
                    fprintf(stderr, "Invalid --size: %s\n", value);
                    return -1;
                }
            } else if (strcmp(option, "--bs") == 0) {
                if (parse_positive_u64(value, &opts->block_kb) != 0) {
                    fprintf(stderr, "Invalid --bs: %s\n", value);
                    return -1;
                }
            } else if (strcmp(option, "--mode") == 0) {
                if (parse_mode(value, &opts->mode) != 0) {
                    fprintf(stderr, "Invalid --mode: %s\n", value);
                    return -1;
                }
                opts->mode_set = 1;
            } else {
                opts->device = value;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    if (opts->directory == NULL || *opts->directory == '\0' ||
        opts->size_mb == 0 || opts->block_kb == 0 || !opts->mode_set) {
        return -1;
    }
    return 0;
}

static int write_all_at(int fd, const unsigned char *buffer, size_t length,
                        uint64_t offset) {
    size_t written = 0;

    while (written < length) {
        ssize_t result = pwrite(fd, buffer + written, length - written,
                                (off_t)(offset + written));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = EIO;
            }
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static int read_all_at(int fd, unsigned char *buffer, size_t length,
                       uint64_t offset) {
    size_t received = 0;

    while (received < length) {
        ssize_t result = pread(fd, buffer + received, length - received,
                               (off_t)(offset + received));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = EIO;
            }
            return -1;
        }
        received += (size_t)result;
    }
    return 0;
}

static uint64_t next_random(uint64_t *state) {
    uint64_t value = *state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static const char *mode_name(workload_mode mode) {
    switch (mode) {
    case MODE_SEQWRITE: return "seqwrite";
    case MODE_RANDWRITE: return "randwrite";
    case MODE_SEQREAD: return "seqread";
    case MODE_RANDREAD: return "randread";
    }
    return "unknown";
}

static int run_workload(int fd, FILE *csv, const options *opts,
                        uint64_t total_bytes, size_t block_size,
                        size_t operation_count, uint64_t *latencies,
                        uint64_t *bytes_processed) {
    unsigned char *buffer = malloc(block_size);
    uint64_t random_state = io_now_ns() ^ (uint64_t)getpid();
    uint64_t full_block_count = total_bytes / (uint64_t)block_size;
    int has_partial_block = total_bytes % (uint64_t)block_size != 0;
    size_t i;
    int writing = opts->mode == MODE_SEQWRITE || opts->mode == MODE_RANDWRITE;

    if (buffer == NULL) {
        perror("malloc");
        return -1;
    }
    if (random_state == 0) {
        random_state = UINT64_C(0x9e3779b97f4a7c15);
    }

    for (i = 0; i < operation_count; ++i) {
        uint64_t block_index = (uint64_t)i;
        uint64_t offset;
        uint64_t remaining;
        size_t bytes;
        uint64_t start;
        uint64_t end;
        int result;

        if (opts->mode == MODE_RANDWRITE || opts->mode == MODE_RANDREAD) {
            if (has_partial_block && i == operation_count - 1) {
                block_index = (uint64_t)operation_count - 1;
            } else {
                block_index = next_random(&random_state) % full_block_count;
            }
        }
        offset = block_index * (uint64_t)block_size;
        remaining = total_bytes - offset;
        bytes = remaining < (uint64_t)block_size ? (size_t)remaining : block_size;
        if (writing) {
            memset(buffer, 0xA5, bytes);
        }

        start = io_now_ns();
        if (start == 0) {
            fprintf(stderr, "Could not read monotonic clock\n");
            free(buffer);
            return -1;
        }
        result = writing
                     ? write_all_at(fd, buffer, bytes, offset)
                     : read_all_at(fd, buffer, bytes, offset);
        end = io_now_ns();
        if (result != 0 || end == 0 || end < start) {
            if (result != 0) {
                perror(writing ? "pwrite" : "pread");
            } else {
                fprintf(stderr, "Could not measure operation latency\n");
            }
            free(buffer);
            return -1;
        }

        latencies[i] = end - start;
        if (io_csv_write_sample(csv, 1, mode_name(opts->mode),
                                (uint64_t)i, latencies[i], (uint64_t)bytes) != 0) {
            fprintf(stderr, "Failed to write workload sample to CSV\n");
            free(buffer);
            return -1;
        }
        *bytes_processed += (uint64_t)bytes;
    }

    free(buffer);
    return 0;
}

static void print_device_stats(const char *device, const io_device_stats *stats) {
    printf("%s stat : write ios %" PRIu64 " | write merges %" PRIu64
           " | write sectors %" PRIu64 "\n",
           device, stats->write_ios, stats->write_merges,
           stats->write_sectors);
}

static void print_counter_delta(const char *name, uint64_t before, uint64_t after,
                               int first) {
    uint64_t magnitude;

    if (!first) {
        printf(" | ");
    }
    if (after >= before) {
        magnitude = after - before;
        printf("%s +%" PRIu64, name, magnitude);
    } else {
        magnitude = before - after;
        printf("%s -%" PRIu64, name, magnitude);
    }
}

int main(int argc, char **argv) {
    const uint64_t mb = UINT64_C(1024) * UINT64_C(1024);
    const uint64_t kb = UINT64_C(1024);
    const char *group = "05";
    options opts;
    uint64_t total_bytes;
    uint64_t block_bytes;
    uint64_t operation_count_u64;
    size_t block_size;
    size_t operation_count;
    uint64_t *latencies = NULL;
    uint64_t bytes_processed = 0;
    uint64_t dirty_before, writeback_before, dirty_after, writeback_after;
    io_device_stats device_before, device_after;
    int have_device = 0;
    int fd = -1;
    FILE *csv = NULL;
    char file_path[4096];
    char csv_path[4096];
    struct stat file_info;
    uint64_t start_time, end_time;
    io_latency_stats latency_stats;
    int exit_status = EXIT_FAILURE;
    int writing;
    size_t path_length;

    if (parse_options(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (opts.size_mb > UINT64_MAX / mb || opts.block_kb > UINT64_MAX / kb) {
        fprintf(stderr, "Requested size or block size is too large\n");
        return EXIT_FAILURE;
    }
    total_bytes = opts.size_mb * mb;
    block_bytes = opts.block_kb * kb;
    if (block_bytes > SIZE_MAX || block_bytes > (uint64_t)INT64_MAX ||
        total_bytes > (uint64_t)INT64_MAX) {
        fprintf(stderr, "Requested size or block size is not supported on this system\n");
        return EXIT_FAILURE;
    }

    operation_count_u64 = total_bytes / block_bytes +
                          (total_bytes % block_bytes != 0);
    if (operation_count_u64 > SIZE_MAX ||
        operation_count_u64 > SIZE_MAX / sizeof(*latencies)) {
        fprintf(stderr, "Too many I/O operations requested\n");
        return EXIT_FAILURE;
    }
    block_size = (size_t)block_bytes;
    operation_count = (size_t)operation_count_u64;
    latencies = malloc(operation_count * sizeof(*latencies));
    if (latencies == NULL) {
        perror("malloc");
        goto cleanup;
    }

    path_length = strlen(opts.directory);
    if (path_length + sizeof("/workload-test.dat") > sizeof(file_path)) {
        fprintf(stderr, "--dir path is too long\n");
        goto cleanup;
    }
    (void)snprintf(file_path, sizeof(file_path), "%s%sworkload-test.dat",
                   opts.directory,
                   path_length > 0 && opts.directory[path_length - 1] == '/' ? "" : "/");

    writing = opts.mode == MODE_SEQWRITE || opts.mode == MODE_RANDWRITE;
    fd = open(file_path, writing ? (O_CREAT | O_RDWR | O_TRUNC) : (O_CREAT | O_RDWR),
              0644);
    if (fd < 0) {
        perror(file_path);
        goto cleanup;
    }
    if (fstat(fd, &file_info) != 0) {
        perror("fstat");
        goto cleanup;
    }
    if ((uint64_t)file_info.st_size < total_bytes &&
        ftruncate(fd, (off_t)total_bytes) != 0) {
        perror("ftruncate");
        goto cleanup;
    }
    if (writing && ftruncate(fd, (off_t)total_bytes) != 0) {
        perror("ftruncate");
        goto cleanup;
    }

    csv = io_csv_open(group, "workload", mode_name(opts.mode),
                      csv_path, sizeof(csv_path));
    if (csv == NULL) {
        perror("Could not open workload CSV");
        goto cleanup;
    }
    if (io_csv_write_header(csv) != 0) {
        fprintf(stderr, "Could not write workload CSV header\n");
        goto cleanup;
    }

    if (io_read_memory_metrics(&dirty_before, &writeback_before) != 0) {
        fprintf(stderr, "Could not read Dirty/Writeback metrics from /proc/meminfo\n");
        goto cleanup;
    }
    if (opts.device != NULL) {
        if (io_read_device_stats(opts.device, &device_before) != 0) {
            fprintf(stderr, "Could not read stats for block device '%s'\n", opts.device);
            goto cleanup;
        }
        have_device = 1;
    }

    banner_print(group, "Sistem Operasi", "Filesystem dan I/O");
    printf("\n[Sebelum]\nDirty           : %" PRIu64 " kB\n"
           "Writeback       : %" PRIu64 " kB\n",
           dirty_before, writeback_before);
    if (have_device) {
        print_device_stats(opts.device, &device_before);
    }

    start_time = io_now_ns();
    if (start_time == 0) {
        fprintf(stderr, "Could not read monotonic clock\n");
        goto cleanup;
    }
    if (run_workload(fd, csv, &opts, total_bytes, block_size,
                     operation_count, latencies, &bytes_processed) != 0) {
        goto cleanup;
    }
    if (writing && opts.fsync_at_end && fsync(fd) != 0) {
        perror("fsync");
        goto cleanup;
    }
    end_time = io_now_ns();
    if (end_time == 0 || end_time <= start_time) {
        fprintf(stderr, "Could not measure workload execution time\n");
        goto cleanup;
    }
    if (fflush(csv) != 0) {
        perror("fflush");
        goto cleanup;
    }
    if (io_read_memory_metrics(&dirty_after, &writeback_after) != 0) {
        fprintf(stderr, "Could not read Dirty/Writeback metrics from /proc/meminfo\n");
        goto cleanup;
    }
    if (have_device && io_read_device_stats(opts.device, &device_after) != 0) {
        fprintf(stderr, "Could not read stats for block device '%s'\n", opts.device);
        goto cleanup;
    }
    if (io_calculate_latency_stats(latencies, operation_count, &latency_stats) != 0) {
        fprintf(stderr, "Could not calculate latency statistics\n");
        goto cleanup;
    }

    {
        double elapsed_seconds = (double)(end_time - start_time) / 1e9;
        double throughput_mb_s = ((double)bytes_processed / (double)mb) / elapsed_seconds;
        printf("\n[Sesudah]\nDirty           : %" PRIu64 " kB\n"
               "Writeback       : %" PRIu64 " kB\n",
               dirty_after, writeback_after);
        if (have_device) {
            print_device_stats(opts.device, &device_after);
            printf("Selisih %s      : ", opts.device);
            print_counter_delta("write ios", device_before.write_ios,
                                device_after.write_ios, 1);
            print_counter_delta("write merges", device_before.write_merges,
                                device_after.write_merges, 0);
            print_counter_delta("write sectors", device_before.write_sectors,
                                device_after.write_sectors, 0);
            putchar('\n');
        }
        printf("\nWaktu eksekusi  : %.3f s\n"
               "Throughput      : %.1f MB/s\n"
               "Latensi (ns)    : avg %.0f | min %" PRIu64 " | max %" PRIu64
               " | p50 %" PRIu64 " | p95 %" PRIu64 " | p99 %" PRIu64 "\n"
               "Log             : %s\n",
               elapsed_seconds, throughput_mb_s,
               latency_stats.average, latency_stats.minimum, latency_stats.maximum,
               latency_stats.p50, latency_stats.p95, latency_stats.p99, csv_path);
    }

    exit_status = EXIT_SUCCESS;

cleanup:
    if (csv != NULL && fclose(csv) != 0) {
        perror("fclose CSV");
        exit_status = EXIT_FAILURE;
    }
    if (fd >= 0 && close(fd) != 0) {
        perror("close workload file");
        exit_status = EXIT_FAILURE;
    }
    free(latencies);
    return exit_status;
}
