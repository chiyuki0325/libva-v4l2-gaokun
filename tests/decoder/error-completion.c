// SPDX-License-Identifier: MIT
// Test-only shim: mark selected decoded pictures as recoverable V4L2 errors.
// cc -shared -fPIC error-completion.c -ldl -o error-completion.so
// IRIS_TEST_ERROR_QUEUE=capture|output|both, IRIS_TEST_ERROR_FIRST=46,
// IRIS_TEST_ERROR_COUNT=1. Set IRIS_TEST_ERROR_PAYLOAD=keep for nonempty errors.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static atomic_int fail_write;

int ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == DMA_BUF_IOCTL_SYNC &&
        (((struct dma_buf_sync *)arg)->flags & DMA_BUF_SYNC_WRITE) &&
        atomic_exchange(&fail_write, 0)) {
        errno = EIO;
        return -1;
    }
    int (*real_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
    int result = real_ioctl(fd, request, arg);
    const char *queue = getenv("IRIS_TEST_ERROR_QUEUE");
    if (result || request != VIDIOC_DQBUF || !queue)
        return result;

    struct v4l2_buffer *buffer = arg;
    int capture = buffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int output = buffer->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if ((!capture && !output) ||
        (strcmp(queue, "both") && strcmp(queue, capture ? "capture" : "output")))
        return result;
    const char *first_env = getenv("IRIS_TEST_ERROR_FIRST");
    const char *count_env = getenv("IRIS_TEST_ERROR_COUNT");
    uint64_t first = first_env ? strtoull(first_env, NULL, 10) : 46;
    uint64_t count = count_env ? strtoull(count_env, NULL, 10) : 1;
    uint64_t token = (uint64_t)buffer->timestamp.tv_sec * 1000000 + buffer->timestamp.tv_usec;
    if (token < first || token - first >= count)
        return result;

    buffer->flags |= V4L2_BUF_FLAG_ERROR;
    if (getenv("IRIS_TEST_FAIL_RECOVERY"))
        atomic_store(&fail_write, 1);
    const char *payload = getenv("IRIS_TEST_ERROR_PAYLOAD");
    if (capture && (!payload || strcmp(payload, "keep"))) {
        buffer->m.planes[0].bytesused = 0;
        buffer->m.planes[0].data_offset = 0;
    }
    fprintf(stderr, "injected %s error token=%llu\n", capture ? "CAPTURE" : "OUTPUT",
            (unsigned long long)token);
    if (getenv("IRIS_TEST_ZERO_TIMESTAMP"))
        buffer->timestamp = (struct timeval){0};
    return result;
}
