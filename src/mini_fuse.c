#define FUSE_USE_VERSION 31

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <fuse3/fuse.h>

enum {
    CACHE_DEFAULT,
    CACHE_DIRECT_IO,
    CACHE_KERNEL
};

static const char hello_text[] =
    "Hello from a read-only FUSE filesystem.\n";
static const uint64_t random_file_size = UINT64_C(1024) * UINT64_C(1024);
static int cache_mode = CACHE_DEFAULT;

static uint64_t monotonic_ns(void) {
    struct timespec time;

    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        return 0;
    }
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) +
           (uint64_t)time.tv_nsec;
}

static void log_callback(const char *name, const char *path) {
    fprintf(stderr, "CALLBACK %s path=%s\n", name, path);
    fflush(stderr);
}

static int mini_getattr(const char *path, struct stat *info,
                        struct fuse_file_info *file_info) {
    (void)file_info;
    memset(info, 0, sizeof(*info));

    if (strcmp(path, "/") == 0) {
        info->st_mode = S_IFDIR | 0555;
        info->st_nlink = 2;
        log_callback("getattr", path);
        return 0;
    }
    if (strcmp(path, "/hello.txt") == 0) {
        info->st_mode = S_IFREG | 0444;
        info->st_nlink = 1;
        info->st_size = (off_t)(sizeof(hello_text) - 1);
    } else if (strcmp(path, "/random.bin") == 0) {
        info->st_mode = S_IFREG | 0444;
        info->st_nlink = 1;
        info->st_size = (off_t)random_file_size;
    } else {
        log_callback("getattr", path);
        return -ENOENT;
    }

    log_callback("getattr", path);
    return 0;
}

static int mini_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *file_info,
                        enum fuse_readdir_flags flags) {
    (void)offset;
    (void)file_info;
    (void)flags;
    log_callback("readdir", path);

    if (strcmp(path, "/") != 0) {
        return -ENOENT;
    }

    filler(buffer, ".", NULL, 0, 0);
    filler(buffer, "..", NULL, 0, 0);
    filler(buffer, "hello.txt", NULL, 0, 0);
    filler(buffer, "random.bin", NULL, 0, 0);
    return 0;
}

static int mini_open(const char *path, struct fuse_file_info *file_info) {
    int result = 0;

    if (strcmp(path, "/hello.txt") != 0 && strcmp(path, "/random.bin") != 0) {
        result = -ENOENT;
    } else if ((file_info->flags & O_ACCMODE) != O_RDONLY) {
        result = -EROFS;
    } else if (cache_mode == CACHE_DIRECT_IO) {
        file_info->direct_io = 1;
    } else if (cache_mode == CACHE_KERNEL) {
        file_info->keep_cache = 1;
    }

    fprintf(stderr, "CALLBACK open path=%s result=%d\n", path, result);
    fflush(stderr);
    return result;
}

static unsigned char random_byte(uint64_t index) {
    uint64_t value = index + UINT64_C(0x9e3779b97f4a7c15);

    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return (unsigned char)(value >> 56);
}

static int mini_read(const char *path, char *buffer, size_t size, off_t offset,
                     struct fuse_file_info *file_info) {
    uint64_t start = monotonic_ns();
    uint64_t end;
    uint64_t file_size;
    size_t bytes;
    size_t i;
    int result;

    (void)file_info;
    if (strcmp(path, "/hello.txt") == 0) {
        file_size = sizeof(hello_text) - 1;
    } else if (strcmp(path, "/random.bin") == 0) {
        file_size = random_file_size;
    } else {
        result = -ENOENT;
        goto done;
    }
    if (offset < 0) {
        result = -EINVAL;
        goto done;
    }
    if ((uint64_t)offset >= file_size) {
        result = 0;
        goto done;
    }

    bytes = size;
    if ((uint64_t)bytes > file_size - (uint64_t)offset) {
        bytes = (size_t)(file_size - (uint64_t)offset);
    }
    if (strcmp(path, "/hello.txt") == 0) {
        memcpy(buffer, hello_text + offset, bytes);
    } else {
        for (i = 0; i < bytes; ++i) {
            buffer[i] = (char)random_byte((uint64_t)offset + (uint64_t)i);
        }
    }
    result = (int)bytes;

done:
    end = monotonic_ns();
    fprintf(stderr,
            "CALLBACK read path=%s offset=%" PRIdMAX " size=%zu "
            "result=%d latency_ns=%" PRIu64 "\n",
            path, (intmax_t)offset, size, result,
            start != 0 && end >= start ? end - start : 0);
    fflush(stderr);
    return result;
}

static const struct fuse_operations operations = {
    .getattr = mini_getattr,
    .readdir = mini_readdir,
    .open = mini_open,
    .read = mini_read,
};

int main(int argc, char **argv) {
    char **fuse_argv;
    int fuse_argc = 1;
    int i;
    int status;
    int append_kernel_cache = 0;

    if (argc < 3 || strcmp(argv[1], "--cache") != 0) {
        fprintf(stderr,
                "Usage: %s --cache direct_io|kernel_cache MOUNTPOINT [FUSE options]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[2], "direct_io") == 0) {
        cache_mode = CACHE_DIRECT_IO;
    } else if (strcmp(argv[2], "kernel_cache") == 0) {
        cache_mode = CACHE_KERNEL;
        append_kernel_cache = 1;
    } else {
        fprintf(stderr, "Unknown cache mode: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    fuse_argv = calloc((size_t)argc + 2, sizeof(*fuse_argv));
    if (fuse_argv == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }
    fuse_argv[0] = argv[0];
    for (i = 3; i < argc; ++i) {
        fuse_argv[fuse_argc++] = argv[i];
    }
    if (append_kernel_cache) {
        fuse_argv[fuse_argc++] = "-o";
        fuse_argv[fuse_argc++] = "kernel_cache";
    }

    status = fuse_main(fuse_argc, fuse_argv, &operations, NULL);
    free(fuse_argv);
    return status;
}
