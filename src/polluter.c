#include "../include/common.h"
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define DEF_STRIDE       64
#define DEF_BUF_SIZE     (64 * 1024 * 1024)
#define DEF_THREADS      1
#define DEF_MEASUREMENTS 0

typedef struct
{
    volatile i64 *buf;
    u64 buf_size;
    u64 stride;
    volatile u64 *access_count;
    i32 thread_id;
} thread_args_t;

static volatile u64 *global_counters;
static i32 total_threads;
static i32 max_measurements = 0;
static volatile i32 should_exit = 0;

static void print_usage(const char *prog)
{
    printf("usage: %s [options]\n", prog);
    printf("options:\n");
    printf("  -s <stride>    stride in bytes (must be >0)\n");
    printf("  -b <buf_size>  buffer size in bytes (must be >= stride)\n");
    printf("  -t <threads>   number of threads (>0)\n");
    printf("  -m <measures>  number of measurements before exit (0=infinite)\n");
    printf("  -h             display this help message\n");
}

static void *poison_thread(void *arg)
{
    thread_args_t *a = arg;
    volatile i64 *buf = a->buf;
    u64 size = a->buf_size;
    u64 stride = a->stride;
    u64 idx = 0;
    u64 max_offset = size / sizeof(i64);
    volatile u64 *counter = &global_counters[a->thread_id];

    while (!should_exit) {
        buf[idx] = buf[idx] + 1;
        (*counter)++;
        idx += stride / sizeof(i64);
        if (idx >= max_offset)
            idx = 0;
    }
    return NULL;
}

static void *reporter_thread(void *arg)
{
    (void)arg;
    u64 prev_total = 0;
    u64 current_total;
    i32 i, measurement = 0;
    f64 *throughputs = NULL;
    f64 sum = 0.0;

    if (max_measurements > 0) {
        throughputs = malloc(max_measurements * sizeof(f64));
        if (!throughputs) {
            fprintf(stderr, "error: malloc failed for throughputs\n");
            should_exit = 1;
            return NULL;
        }
    }

    while (1) {
        sleep(1);

        current_total = 0;
        for (i = 0; i < total_threads; i++)
            current_total += global_counters[i];

        f64 throughput_mbps = (f64)(current_total - prev_total) / 1000000.0;

        if (max_measurements > 0) {
            throughputs[measurement] = throughput_mbps;
            printf("measurement %d: %.2f million accesses/sec\n",
                   measurement + 1, throughput_mbps);
            measurement++;

            if (measurement >= max_measurements) {
                for (i = 0; i < max_measurements; i++)
                    sum += throughputs[i];
                printf("average: %.2f million accesses/sec\n",
                       sum / max_measurements);
                free(throughputs);
                should_exit = 1;
                return NULL;
            }
        } else {
            printf("\rthroughput: %.2f million accesses/sec", throughput_mbps);
            fflush(stdout);
        }

        prev_total = current_total;
    }
    return NULL;
}

int main(i32 argc, char *argv[])
{
    i32 opt;
    u64 stride = DEF_STRIDE;
    u64 buf_size = DEF_BUF_SIZE;
    i32 n_threads = DEF_THREADS;
    i32 measurements = DEF_MEASUREMENTS;
    i32 i;
    pthread_t *tids = NULL;
    pthread_t reporter_tid;
    thread_args_t *thread_args = NULL;
    volatile i64 *buffer = NULL;

    while ((opt = getopt(argc, argv, "s:b:t:m:h")) != -1) {
        switch (opt) {
        case 's':
            stride = strtoul(optarg, NULL, 10);
            if (stride == 0) {
                fprintf(stderr, "error: stride must be >0\n");
                return EXIT_FAILURE;
            }
            break;
        case 'b':
            buf_size = strtoul(optarg, NULL, 10);
            if (buf_size < stride) {
                fprintf(stderr, "error: buf_size must be >= stride\n");
                return EXIT_FAILURE;
            }
            break;
        case 't':
            n_threads = atoi(optarg);
            if (n_threads <= 0) {
                fprintf(stderr, "error: threads must be >0\n");
                return EXIT_FAILURE;
            }
            break;
        case 'm':
            measurements = atoi(optarg);
            if (measurements < 0) {
                fprintf(stderr, "error: measurements must be >= 0\n");
                return EXIT_FAILURE;
            }
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    total_threads = n_threads;
    max_measurements = measurements;

    buffer = malloc(buf_size);
    if (!buffer) {
        fprintf(stderr, "error: malloc failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    for (u64 j = 0; j < buf_size / sizeof(i64); j++)
        buffer[j] = 0;

    global_counters = calloc(n_threads, sizeof(u64));
    if (!global_counters) {
        fprintf(stderr, "error: calloc failed for counters: %s\n", strerror(errno));
        free((void *)buffer);
        return EXIT_FAILURE;
    }

    tids = calloc(n_threads, sizeof(pthread_t));
    if (!tids) {
        fprintf(stderr, "error: calloc failed: %s\n", strerror(errno));
        free((void *)buffer);
        free((void *)global_counters);
        return EXIT_FAILURE;
    }

    thread_args = calloc(n_threads, sizeof(thread_args_t));
    if (!thread_args) {
        fprintf(stderr, "error: calloc failed for thread_args: %s\n", strerror(errno));
        free((void *)buffer);
        free((void *)global_counters);
        free(tids);
        return EXIT_FAILURE;
    }

    u64 per_thread_size = buf_size / n_threads;

    for (i = 0; i < n_threads; i++) {
        thread_args[i].buf = buffer + (i * per_thread_size / sizeof(i64));
        thread_args[i].buf_size = per_thread_size;
        thread_args[i].stride = stride;
        thread_args[i].thread_id = i;

        if (pthread_create(&tids[i], NULL, poison_thread, &thread_args[i]) != 0) {
            fprintf(stderr, "error: pthread_create failed at %d: %s\n",
                    i, strerror(errno));
            for (i32 j = 0; j < i; j++)
                pthread_cancel(tids[j]);
            free(tids);
            free(thread_args);
            free((void *)buffer);
            free((void *)global_counters);
            return EXIT_FAILURE;
        }
    }

    if (pthread_create(&reporter_tid, NULL, reporter_thread, NULL) != 0) {
        fprintf(stderr, "error: pthread_create failed for reporter: %s\n", strerror(errno));
        for (i32 j = 0; j < n_threads; j++)
            pthread_cancel(tids[j]);
        free(tids);
        free(thread_args);
        free((void *)buffer);
        free((void *)global_counters);
        return EXIT_FAILURE;
    }

    for (i = 0; i < n_threads; i++)
        pthread_join(tids[i], NULL);

    pthread_join(reporter_tid, NULL);

    free(tids);
    free(thread_args);
    free((void *)buffer);
    free((void *)global_counters);
    return EXIT_SUCCESS;
}

