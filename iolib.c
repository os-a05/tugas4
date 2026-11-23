#define _POSIX_C_SOURCE 200809L
#include "iolib.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <inttypes.h>

static int valid_component(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    if (text == NULL || *text == '\0') {
        return 0;
    }

    for (; *p != '\0'; ++p) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_')) {
            return 0;
        }
    }

    return 1;
}

static int ensure_logs_dir(void)
{
    struct stat info;

    if (mkdir("logs", 0755) == 0) {
        return 0;
    }

    if (stat("logs", &info) == 0 && S_ISDIR(info.st_mode)) {
        return 0;
    }

    return -1;
}

static void timestamp_now(char *out, size_t size)
{
    struct timespec ts;
    struct tm local_time;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 ||
        localtime_r(&ts.tv_sec, &local_time) == NULL) {
        (void)snprintf(out, size, "unknown-time");
        return;
    }

    (void)strftime(out, size, "%Y%m%d-%H%M%S", &local_time);
}

// Pengukur waktu
uint64_t io_now_ns(void)
{
    struct timespec ts;
    
    // jika gagal ret 0
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    // conver sec menjadi ns + ns pada ts
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

// Pembaca metrik OS (dirty & wrteback)
int io_read_memory_metrics(uint64_t *dirty_kb, uint64_t *writeback_kb) {
    if (dirty_kb == NULL || writeback_kb == NULL) return -1;

    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo == NULL) return -1;

    char line[256];
    int found_dirty = 0;
    int found_writeback = 0;

    while (fgets(line, sizeof(line), meminfo) != NULL) {
        if(strncmp(line, "Dirty:", 6) == 0) {
            if (sscanf(line, "%*s %" SCNu64, dirty_kb) == 1) {
                found_dirty = 1;
            } 
        } 
        else if (strncmp(line, "Writeback:", 10) == 0) {
            if (sscanf(line, "%*s %" SCNu64, writeback_kb) == 1) {
                found_writeback = 1;
            } 
        }

        if (found_dirty && found_writeback) break;
    }

    fclose(meminfo);
    return found_dirty && found_writeback ? 0 : -1;
}


// Pembaca statistik device (write ios, merges, dan sectors)
int io_read_device_stats(const char *device, io_device_stats *stats) {
    if (device == NULL || stats == NULL || strchr(device, '/') != NULL) return -1;
    
    char path[256];
    unsigned long long values[11];
    int n;

    // Build path dan cek apakah ukuran path cukup
    n = snprintf(path, sizeof(path), "/sys/block/%s/stat", device);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    // Read dan proses file
    FILE *fp = fopen(path, "r");
    if (fp == NULL) return -1;

    n = fscanf(fp, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &values[0], &values[1], &values[2], &values[3], &values[4],
               &values[5], &values[6], &values[7], &values[8], &values[9], &values[10]);
    fclose(fp);

    if (n < 7) return -1;
    stats->write_ios = (uint64_t)values[4];
    stats->write_merges = (uint64_t)values[5];
    stats->write_sectors = (uint64_t)values[6];
    return 0;
}


// Perhitungan statistik (avg, min, max, p50, p95, p99)
int io_calculate_average(const uint64_t *samples, size_t count, double *average){
    long double sum = 0;
    
    if (samples == NULL || count == 0 || average == NULL) return -1;

    for (size_t i = 0; i < count; ++i) {
        sum += (long double)samples[i];
    }

    *average = (double)(sum / (long double)count);
    return 0;
}


int io_calculate_min_max(const uint64_t *samples, size_t count, uint64_t *minimum, uint64_t *maximum) {
    if (samples == NULL || count == 0 || minimum == NULL || maximum == NULL) return -1;
    
    uint64_t min_value = samples[0];
    uint64_t max_value = samples[0];

    for (size_t i = 1; i < count; ++i) {
        if (samples[i] < min_value) {
            min_value = samples[i];
        }
        if (samples[i] > max_value) {
            max_value = samples[i];
        }
    }

    *minimum = min_value;
    *maximum = max_value;
    return 0;
}


int io_compare_u64(const void *a, const void *b) {
    uint64_t value_a = *(const uint64_t *)a;
    uint64_t value_b = *(const uint64_t *)b;

    if (value_a < value_b) return -1;
    if (value_a > value_b) return 1;
    return 0;
}


int io_calculate_percentile(const uint64_t *sorted_samples, size_t count, unsigned int percentile, uint64_t *result) {
    if (sorted_samples == NULL || count == 0 || result == NULL || percentile == 0 || percentile > 100) return -1;

    size_t rank = (count / 100) * percentile + ((count % 100) * percentile + 99) / 100;

    *result = sorted_samples[rank - 1];
    return 0;

}


int io_calculate_latency_stats(const uint64_t *samples, size_t count, io_latency_stats *stats) {
    if (samples == NULL || count == 0 || stats == NULL ) return -1;

    // copy samples untuk sorting
    if (count > SIZE_MAX / sizeof(uint64_t)) return -1;
    uint64_t *sorted_samples = malloc(count * sizeof(*sorted_samples));
    if (sorted_samples == NULL) return -1;
    memcpy(sorted_samples, samples, count * sizeof(*sorted_samples));

    // sort samples copy untuk perhitungan persentil
    qsort(sorted_samples, count, sizeof(*sorted_samples), io_compare_u64);

    // hitung statistik
    if (io_calculate_average(samples, count, &stats->average) != 0 ||
        io_calculate_min_max(sorted_samples, count, &stats->minimum, &stats->maximum) != 0 ||
        io_calculate_percentile(sorted_samples, count, 50, &stats->p50) != 0 ||
        io_calculate_percentile(sorted_samples, count, 95, &stats->p95) != 0 ||
        io_calculate_percentile(sorted_samples, count, 99, &stats->p99) != 0) {
        free(sorted_samples);
        return -1;
    }

    free(sorted_samples);
    return 0;
}


// Logging

// CSV
// Membuat folder log jika belum ada dan open file, return null jika gagal
FILE *io_csv_open(const char *group, const char *program, const char *scenario, char *path, size_t path_size) {
    if (!valid_component(group) || !valid_component(program) || 
        !valid_component(scenario) || path == NULL || path_size == 0) return NULL; 

    if (ensure_logs_dir() != 0) return NULL;

    char time_stamp[32];
    int n;

    timestamp_now(time_stamp, sizeof(time_stamp));
    n = snprintf(path, path_size, "logs/%s-%s-%s-%s.csv", group, program, scenario, time_stamp);

    // cek apakah path size cukup
    if (n < 0 || (size_t)n >= path_size) return NULL;

    return fopen(path, "w");
}

// Menulis header table di csv, return -1 jika penulisan gagal
int io_csv_write_header(FILE *stream)
{
    if (stream == NULL) return -1;
    return fputs("run_id,scenario,op_index,latency_ns,bytes\n", stream) < 0 ? -1 : 0;
}

// Menulis isi table di csv, return -1 jika penulisan gagal
int io_csv_write_sample(FILE *stream, uint64_t run_id, const char *scenario, 
                        uint64_t op_index, uint64_t latency_ns, uint64_t bytes) {
    if (stream == NULL || !valid_component(scenario)) { errno = EINVAL; return -1; }
    return fprintf(stream, "%llu,%s,%llu,%llu,%llu\n",
                   (unsigned long long)run_id, scenario,
                   (unsigned long long)op_index,
                   (unsigned long long)latency_ns,
                   (unsigned long long)bytes) < 0 ? -1 : 0;
}


// Penyimpanan OS specs ke txt, labe
// Tulis output untuk data yang bisa langsung dibaca
static int write_file_value(FILE *out, const char *label, const char *file_path)
{
    FILE *in = fopen(file_path, "r");
    char buf[256];

    if (in == NULL) {
        return fprintf(out, "%s : unavailable\n", label) < 0 ? -1 : 0;
    }
    
    // Mengambil info dari file ke buf
    if (fgets(buf, sizeof(buf), in) == NULL) {
        snprintf(buf, sizeof(buf), "unavailable");
    }

    fclose(in);

    // Hilangin \r dan \n di akhir line
    buf[strcspn(buf, "\r\n")] = '\0';

    return fprintf(out, "%s : %s\n", label, buf) < 0 ? -1 : 0;
}

// Tulis output untuk data yang didapatkan dari menjalankan cmd
static int write_command_output(FILE *out, const char *label, const char *command) {   
    // menjalankan cmd dan membuka file nya
    FILE *pipe = popen(command, "r");
    char line[512];
    int wrote_output = 0;

    if (pipe == NULL) {
        return fprintf(out, "%s : unavailable\n", label) < 0 ? -1 : 0;
    }

    // menulis label / judul, jika gagal close file dan ret -1
    if (fprintf(out, "%s :\n", label) < 0) {
        (void)pclose(pipe);
        return -1;
    }

    // Menyalin data output ke file log, jika gagal close file dan ret -1
    while (fgets(line, sizeof(line), pipe) != NULL) {
        if (fputs(line, out) == EOF) {
            (void)pclose(pipe);
            return -1;
        }
        wrote_output = 1;
    }

    // jika penulisan gagal close file dan ret -1
    if (!wrote_output && fputs("  unavailable\n", out) == EOF) {
        (void)pclose(pipe);
        return -1;
    }

    return pclose(pipe) == -1 ? -1 : 0;
}

int io_write_environment(const char *group, char *path, size_t path_size) {
    if (!valid_component(group) || path == NULL || path_size == 0) return -1;
    if (ensure_logs_dir() != 0) return -1;

    char time_stamp[32];
    struct utsname uts;
    int n, failed = 0;
    timestamp_now(time_stamp, sizeof(time_stamp));

    // cek apakah path size cukup 
    n = snprintf(path, path_size, "logs/%s-env-%s.txt", group, time_stamp);
    if (n < 0 || (size_t)n >= path_size) return -1; 

    FILE *out = fopen(path, "w");
    if (out == NULL) {
        return -1;
    }

    if (uname(&uts) == 0) {
        if (fprintf(out, "Kernel : %s\nSystem : %s\nMachine : %s\n",
                    uts.release, uts.sysname, uts.machine) < 0) {
            failed = 1;
        }
    } else {
        if (fprintf(out, "Kernel : unavailable\n") < 0) {
            failed = 1;
        }
    }

    if (write_file_value(out, "RAM (MemTotal)", "/proc/meminfo") != 0) {
        failed = 1;
    }
    if (write_file_value(out, "vm.dirty_ratio", "/proc/sys/vm/dirty_ratio") != 0) {
        failed = 1;
    }
    if (write_file_value(out, "vm.dirty_background_ratio",
                         "/proc/sys/vm/dirty_background_ratio") != 0) {
        failed = 1;
    }
    if (write_command_output(out, "lsblk", "lsblk") != 0) {
        failed = 1;
    }
    if (write_command_output(out, "Read-ahead per device",
                             "for f in /sys/block/*/queue/read_ahead_kb; do "
                             "[ -r \"$f\" ] && printf '%s : ' \"$f\" && "
                             "cat \"$f\"; done") != 0) {
        failed = 1;
    }

    if (fclose(out) != 0) {
        failed = 1;
    }
    return failed ? -1 : 0;
}