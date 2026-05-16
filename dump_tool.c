/*
 * dump_tool.c - Userspace acquisition tool for /dev/memdump
 *
 * Features:
 *   - 1MB chunked reads (reduces syscall overhead vs 4KB)
 *   - mmap zero-copy mode (--mmap flag)
 *   - Inline SHA-256 hashing (OpenSSL)
 *   - Progress reporting
 *   - ioctl metadata query
 *   - Chain-of-custody output (JSON sidecar)
 *
 * Build:
 *   gcc -O2 -Wall dump_tool.c -lssl -lcrypto -o dump_tool
 *
 * Usage:
 *   sudo ./dump_tool [--mmap] [--out <file>]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <openssl/sha.h>

/* ---- ioctl definitions (must match kernel module) ----------------------- */

#define MEMDUMP_IOC_MAGIC   'M'
#define MEMDUMP_GET_SIZE    _IOR(MEMDUMP_IOC_MAGIC, 1, unsigned long)
#define MEMDUMP_GET_TS      _IOR(MEMDUMP_IOC_MAGIC, 2, unsigned long long)
#define MEMDUMP_GET_KVER    _IOR(MEMDUMP_IOC_MAGIC, 3, char[64])

/* ---- Constants ---------------------------------------------------------- */

#define DEVICE_PATH     "/dev/memdump"
#define DEFAULT_OUTPUT  "memory_dump.bin"
#define SIDECAR_EXT     ".json"
#define CHUNK_SIZE      (1024UL * 1024UL)   /* 1 MB per read() call */

/* ---- Helpers ------------------------------------------------------------ */

static void print_hash(const unsigned char *hash, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        printf("%02x", hash[i]);
}

static void progress(size_t done, size_t total)
{
    double pct = total ? (100.0 * done / total) : 0.0;
    fprintf(stderr, "\r  Acquired: %6.1f MB / %6.1f MB  [%5.1f%%]",
            done   / (1024.0 * 1024.0),
            total  / (1024.0 * 1024.0),
            pct);
    fflush(stderr);
}

/**
 * write_sidecar - Write a JSON chain-of-custody record alongside the dump.
 */
static void write_sidecar(const char *out_path,
                           size_t acquired,
                           unsigned long long ts_ns,
                           const char *kver,
                           const unsigned char *sha256)
{
    char sidecar_path[512];
    FILE *f;
    time_t now = time(NULL);
    char timebuf[64];
    size_t i;

    snprintf(sidecar_path, sizeof(sidecar_path), "%s%s", out_path, SIDECAR_EXT);
    f = fopen(sidecar_path, "w");
    if (!f) {
        perror("fopen sidecar");
        return;
    }

    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    fprintf(f, "{\n");
    fprintf(f, "  \"tool\"            : \"memdump dump_tool v2.0.0\",\n");
    fprintf(f, "  \"output_file\"     : \"%s\",\n", out_path);
    fprintf(f, "  \"acquired_bytes\"  : %zu,\n", acquired);
    fprintf(f, "  \"kernel_version\"  : \"%s\",\n", kver);
    fprintf(f, "  \"acq_timestamp_ns\": %llu,\n", ts_ns);
    fprintf(f, "  \"utc_time\"        : \"%s\",\n", timebuf);
    fprintf(f, "  \"sha256\"          : \"");
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
        fprintf(f, "%02x", sha256[i]);
    fprintf(f, "\"\n}\n");

    fclose(f);
    printf("  Sidecar saved:  %s\n", sidecar_path);
}

/* ---- Acquisition modes -------------------------------------------------- */

/**
 * acquire_chunked - Standard read()-based acquisition with 1MB chunks.
 *
 * Lower overhead than 4KB reads (fewer syscalls, better throughput).
 */
static int acquire_chunked(int fd, FILE *out,
                            size_t total_size,
                            SHA256_CTX *sha_ctx)
{
    char *buf;
    ssize_t bytes;
    size_t acquired = 0;

    buf = malloc(CHUNK_SIZE);
    if (!buf) {
        perror("malloc chunk buffer");
        return -1;
    }

    while ((bytes = read(fd, buf, CHUNK_SIZE)) > 0) {
        if (fwrite(buf, 1, (size_t)bytes, out) != (size_t)bytes) {
            perror("fwrite");
            free(buf);
            return -1;
        }
        SHA256_Update(sha_ctx, buf, (size_t)bytes);
        acquired += (size_t)bytes;
        progress(acquired, total_size);
    }

    if (bytes < 0)
        perror("read");

    free(buf);
    fprintf(stderr, "\n");
    return (bytes < 0) ? -1 : 0;
}

/**
 * acquire_mmap - Zero-copy acquisition via mmap.
 *
 * Avoids copy_to_user + read() overhead entirely.
 * Best for large dumps on memory-constrained systems.
 */
static int acquire_mmap(int fd, FILE *out,
                         size_t total_size,
                         SHA256_CTX *sha_ctx)
{
    void *mapped;
    size_t offset = 0;
    size_t chunk;
    int ret = 0;

    if (total_size == 0) {
        fprintf(stderr, "  [mmap] Cannot determine size; falling back to read()\n");
        return acquire_chunked(fd, out, 0, sha_ctx);
    }

    mapped = mmap(NULL, total_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        fprintf(stderr, "  [mmap] Falling back to read()-based acquisition\n");
        return acquire_chunked(fd, out, total_size, sha_ctx);
    }

    /* Advise sequential access pattern for prefetch */
    madvise(mapped, total_size, MADV_SEQUENTIAL);

    while (offset < total_size) {
        chunk = (total_size - offset < CHUNK_SIZE)
                ? (total_size - offset)
                : CHUNK_SIZE;

        if (fwrite((char *)mapped + offset, 1, chunk, out) != chunk) {
            perror("fwrite");
            ret = -1;
            break;
        }
        SHA256_Update(sha_ctx, (char *)mapped + offset, chunk);
        offset += chunk;
        progress(offset, total_size);
    }

    munmap(mapped, total_size);
    fprintf(stderr, "\n");
    return ret;
}

/* ---- Entry point -------------------------------------------------------- */

int main(int argc, char *argv[])
{
    int fd;
    FILE *out;
    int use_mmap = 0;
    const char *out_path = DEFAULT_OUTPUT;

    SHA256_CTX sha_ctx;
    unsigned char sha256[SHA256_DIGEST_LENGTH];

    size_t total_size        = 0;
    unsigned long long ts_ns = 0;
    char kver[64]            = "unknown";

    /* ---- Parse arguments ------------------------------------------------ */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mmap") == 0) {
            use_mmap = 1;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            fprintf(stderr, "Usage: %s [--mmap] [--out <file>]\n", argv[0]);
            return 1;
        }
    }

    /* ---- Open device ---------------------------------------------------- */
    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        perror("open " DEVICE_PATH);
        return 1;
    }

    /* ---- Query metadata via ioctl --------------------------------------- */
    if (ioctl(fd, MEMDUMP_GET_SIZE, &total_size) < 0)
        perror("ioctl MEMDUMP_GET_SIZE (non-fatal)");

    if (ioctl(fd, MEMDUMP_GET_TS, &ts_ns) < 0)
        perror("ioctl MEMDUMP_GET_TS (non-fatal)");

    if (ioctl(fd, MEMDUMP_GET_KVER, kver) < 0)
        perror("ioctl MEMDUMP_GET_KVER (non-fatal)");

    printf("\n=== memdump Acquisition Tool v2.0.0 ===\n");
    printf("  Device:         %s\n", DEVICE_PATH);
    printf("  Output:         %s\n", out_path);
    printf("  Kernel:         %s\n", kver);
    printf("  Declared size:  %.1f MB\n", total_size / (1024.0 * 1024.0));
    printf("  Mode:           %s\n\n", use_mmap ? "mmap (zero-copy)" : "read()");

    /* ---- Open output file ----------------------------------------------- */
    out = fopen(out_path, "wb");
    if (!out) {
        perror("fopen output");
        close(fd);
        return 1;
    }

    /* ---- Acquire -------------------------------------------------------- */
    SHA256_Init(&sha_ctx);

    int ret = use_mmap
        ? acquire_mmap(fd, out, total_size, &sha_ctx)
        : acquire_chunked(fd, out, total_size, &sha_ctx);

    fclose(out);
    close(fd);

    if (ret != 0) {
        fprintf(stderr, "Acquisition failed.\n");
        return 1;
    }

    /* ---- Finalise hash -------------------------------------------------- */
    SHA256_Final(sha256, &sha_ctx);

    /* Get actual written size from output file */
    struct stat st;
    size_t actual_size = 0;
    if (stat(out_path, &st) == 0)
        actual_size = (size_t)st.st_size;

    printf("  Written:        %.1f MB\n", actual_size / (1024.0 * 1024.0));
    printf("  SHA-256:        ");
    print_hash(sha256, SHA256_DIGEST_LENGTH);
    printf("\n\n");

    /* ---- Write chain-of-custody sidecar --------------------------------- */
    write_sidecar(out_path, actual_size, ts_ns, kver, sha256);

    printf("Done.\n\n");
    return 0;
}
