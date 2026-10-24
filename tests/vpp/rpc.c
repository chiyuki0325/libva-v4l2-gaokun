// SPDX-License-Identifier: MIT
// The transport checks fixed wire fixtures before handing messages to the
// production skeleton. DSP mocks check native arguments and output propagation.
// RPC_QAIC_STUB/RPC_QAIC_SKEL allow the same fixtures to audit archived QAIC
// endpoints, which lack the new native-pointer validation. Neither is required.
#include "iris_vpp.h"
#include <AEEStdErr.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int iris_vpp_skel_handle_invoke(remote_handle64, uint32_t, remote_arg *);
static const remote_handle64 host_handle = UINT64_C(0xfedcba9876543210);
static const remote_handle64 dsp_handle = UINT64_C(0x123456789abcdef0);
static const uint64_t elapsed = UINT64_C(0x1020304050607080);
static unsigned method, calls, transport_calls;
static int failure;
static uint8_t src[7], rgba[9], dst[11], overlay_params[5];

static int entered(unsigned id, remote_handle64 h) {
    CHECK(id == method && h == dsp_handle);
    calls++;
    return failure;
}
int impl_open(const char *uri, remote_handle64 *h) {
    CHECK(!strcmp(uri, "test-uri"));
    int err = entered(0, dsp_handle);
    *h = dsp_handle;
    return err;
}
int impl_close(remote_handle64 h) {
    return entered(1, h);
}
int impl_power(remote_handle64 h, int *mask) {
    *mask = -123;
    return entered(2, h);
}
int impl_configure(remote_handle64 h, int sw, int sh, int dw, int dh) {
    CHECK(sw == 1920 && sh == 1080 && dw == 1280 && dh == 720);
    return entered(3, h);
}
int impl_info(remote_handle64 h, unsigned int *hz, int *units) {
    *hz = UINT32_C(0xf1234567);
    *units = -7;
    return entered(5, h);
}
int impl_profile(remote_handle64 h, int enable, uint64_t *c, int n) {
    CHECK(enable == 1 && n == 2);
    int err = entered(6, h);
    if (!err) {
        c[0] = elapsed;
        c[1] = UINT64_MAX;
    }
    return err;
}
static int frame(unsigned id, remote_handle64 h, const uint8_t *s, int sn, uint8_t *d, int dn,
                 int ss, int ds, int sy, int suv, int dy, int duv, int workers, uint64_t *us) {
    CHECK(s == src && sn == 7 && dn == 11 && s[0] == 0x11 && d[10] == 0x22);
    CHECK(ss == 17 && ds == 19 && sy == 23 && suv == 29 && dy == 31 && duv == 37 && workers == 3);
    int err = entered(id, h);
    *us = elapsed;
    if (!err)
        d[0] = 0x55;
    return err;
}
int impl_run(remote_handle64 h, const uint8_t *s, int sn, uint8_t *d, int dn, int ss, int ds,
             int sy, int suv, int dy, int duv, int workers, uint64_t *us) {
    return frame(4, h, s, sn, d, dn, ss, ds, sy, suv, dy, duv, workers, us);
}
int impl_process(remote_handle64 h, const uint8_t *s, int sn, uint8_t *d, int dn, int ss, int ds,
                 int sy, int suv, int dy, int duv, int workers, int sf, int df, uint64_t *us) {
    CHECK(sf == 1 && df == 0);
    return frame(7, h, s, sn, d, dn, ss, ds, sy, suv, dy, duv, workers, us);
}
int impl_transform(remote_handle64 h, const uint8_t *s, int sn, uint8_t *d, int dn, int ss, int ds,
                   int sy, int suv, int dy, int duv, int workers, int sf, int df, int rotation,
                   uint64_t *us) {
    CHECK(sf == 1 && df == 0 && rotation == 2);
    return frame(8, h, s, sn, d, dn, ss, ds, sy, suv, dy, duv, workers, us);
}
int impl_overlay(remote_handle64 h, const uint8_t *s, int sn, const uint8_t *r, int rn, uint8_t *d,
                 int dn, const uint8_t *p, int pn, int workers, uint64_t *us) {
    CHECK(s == src && sn == 7 && r == rgba && rn == 9 && dn == 11);
    CHECK(p == overlay_params && pn == 5 && workers == 3 && d[10] == 0x22);
    CHECK(s[0] == 0x11 && r[0] == 0x33 && p[0] == 0x44);
    int err = entered(9, h);
    *us = elapsed;
    if (!err)
        d[0] = 0x55;
    return err;
}

static void rejected(uint32_t sc, remote_arg *a) {
    unsigned before = calls;
    CHECK(iris_vpp_skel_handle_invoke(dsp_handle, sc, a) != 0);
    CHECK(calls == before);
}

static void malformed(uint32_t sc, remote_arg *a) {
    rejected(sc ^ (1u << 16), a);
    rejected(sc ^ (1u << 8), a);
    rejected(sc ^ (1u << 4), a);
    rejected(sc ^ 1u, a);
    unsigned bufs = REMOTE_SCALARS_INBUFS(sc) + REMOTE_SCALARS_OUTBUFS(sc);
    for (unsigned i = 0; i < bufs; i++) {
        size_t size = a[i].buf.nLen;
        a[i].buf.nLen = size - 1;
        rejected(sc, a);
        a[i].buf.nLen = size;
#ifndef RPC_QAIC_SKEL
        void *ptr = a[i].buf.pv;
        a[i].buf.pv = NULL;
        rejected(sc, a);
        a[i].buf.pv = ptr;
#endif
    }
#ifndef RPC_QAIC_SKEL
    rejected(sc, NULL);
#endif
}

int remote_handle64_open(const char *uri, remote_handle64 *h) {
    CHECK(method == 0 && !strcmp(uri, "test-uri"));
    transport_calls++;
    uint32_t n = strlen(uri) + 1;
    remote_arg a[] = {{.buf = {&n, 4}}, {.buf = {(void *)uri, n}}, {.h64 = 0}};
    uint32_t sc = REMOTE_SCALARS_MAKEX(0, 0, 2, 0, 0, 1);
    malformed(sc, a);
    n--;
    rejected(sc, a);
    n++;
    n = 0;
    rejected(sc, a);
    n = strlen(uri) + 1;
    int err = iris_vpp_skel_handle_invoke(0, sc, a);
    if (!err) {
        CHECK(a[2].h64 == dsp_handle);
        *h = host_handle;
    } else
        CHECK(a[2].h64 == 0);
    return err;
}
int remote_handle64_close(remote_handle64 h) {
    CHECK(method == 1 && h == host_handle);
    transport_calls++;
    remote_arg a = {.h64 = dsp_handle};
    uint32_t sc = REMOTE_SCALARS_MAKEX(0, 1, 0, 0, 1, 0);
    malformed(sc, &a);
    return iris_vpp_skel_handle_invoke(0, sc, &a);
}

int remote_handle64_invoke(remote_handle64 h, uint32_t sc, remote_arg *a) {
    CHECK(h == host_handle && REMOTE_SCALARS_METHOD(sc) == method);
    transport_calls++;
    // Independent fixtures for the original IDL, not shared implementation constants.
    static const unsigned inbufs[] = {0, 0, 0, 1, 3, 0, 1, 3, 3, 5};
    static const unsigned outbufs[] = {0, 0, 1, 0, 2, 1, 1, 2, 2, 2};
    CHECK(sc == REMOTE_SCALARS_MAKE(method, inbufs[method], outbufs[method]));
    const int32_t geometry[] = {7, 11, 11, 17, 19, 23, 29, 31, 37, 3, 1, 0, 2};
    const int32_t configure[] = {1920, 1080, 1280, 720};
    const int32_t profile[] = {1, 2};
    const int32_t overlay[] = {7, 9, 11, 11, 5, 3};
    const void *primary = NULL;
    size_t sizes[7] = {0};
    unsigned inout_in = 0, inout_out = 0, length_word = 0;
    switch (method) {
    case 2:
        sizes[0] = 4;
        break;
    case 3:
        primary = configure;
        sizes[0] = 16;
        break;
    case 5:
        sizes[0] = 8;
        break;
    case 6:
        primary = profile;
        sizes[0] = 8;
        sizes[1] = 16;
        break;
    case 4:
    case 7:
    case 8:
        primary = geometry;
        sizes[0] = method == 4 ? 40 : method == 7 ? 48 : 52;
        sizes[1] = 7;
        sizes[2] = 11;
        sizes[3] = 8;
        sizes[4] = 11;
        CHECK(a[1].buf.pv == src && a[2].buf.pv == dst && a[4].buf.pv == dst);
        inout_in = 2;
        inout_out = 4;
        length_word = 1;
        break;
    case 9:
        primary = overlay;
        sizes[0] = 24;
        sizes[1] = 7;
        sizes[2] = 9;
        sizes[3] = 11;
        sizes[4] = 5;
        sizes[5] = 8;
        sizes[6] = 11;
        CHECK(a[1].buf.pv == src && a[2].buf.pv == rgba && a[3].buf.pv == dst &&
              a[4].buf.pv == overlay_params && a[6].buf.pv == dst);
        inout_in = 3;
        inout_out = 6;
        length_word = 2;
        break;
    default:
        CHECK(0);
    }
    if (primary)
        CHECK(!memcmp(a[0].buf.pv, primary, sizes[0]));
    for (unsigned i = 0; i < inbufs[method] + outbufs[method]; i++)
        CHECK(a[i].buf.nLen == sizes[i]);
    malformed(sc, a);
    if (inout_in || method == 6) {
        int32_t *p = a[0].buf.pv;
        unsigned word = inout_in ? length_word : 1;
        int32_t saved = p[word];
        p[word] = -1;
        rejected(sc, a);
        p[word] = INT_MAX;
        rejected(sc, a);
        p[word] = saved;
        if (inout_in) {
            // Both buffers individually fit, but input must fit inside output.
            p[word + 1] = saved - 1;
            rejected(sc, a);
            p[word + 1] = saved;
        }
    }
    if (inout_in) {
        // Exercise transports that give input and output different addresses.
        uint8_t separate[11] = {0};
        remote_arg original = a[inout_out];
        a[inout_out].buf.pv = separate;
        int err = iris_vpp_skel_handle_invoke(dsp_handle, sc, a);
        CHECK(err == failure && separate[10] == 0x22);
        CHECK(separate[0] == (failure ? 0x22 : 0x55) && dst[0] == 0x22);
        a[inout_out] = original;
    }
    return iris_vpp_skel_handle_invoke(dsp_handle, sc, a);
}

int main(void) {
    memset(src, 0x11, sizeof(src));
    memset(rgba, 0x33, sizeof(rgba));
    memset(overlay_params, 0x44, sizeof(overlay_params));
    for (unsigned fail = 0; fail < 2; fail++) {
        failure = fail ? 0x1234 : 0;
        method = 0;
        remote_handle64 h = 42;
        CHECK(iris_vpp_open("test-uri", &h) == failure);
        CHECK(h == (fail ? 42 : host_handle));
        method = 1;
        CHECK(iris_vpp_close(host_handle) == failure);
        method = 2;
        int mask = 42;
        CHECK(iris_vpp_power(host_handle, &mask) == failure && mask == (fail ? 42 : -123));
        method = 3;
        CHECK(iris_vpp_configure(host_handle, 1920, 1080, 1280, 720) == failure);
        method = 5;
        unsigned hz = 42;
        int units = 42;
        CHECK(iris_vpp_info(host_handle, &hz, &units) == failure);
        CHECK(hz == (fail ? 42 : UINT32_C(0xf1234567)) && units == (fail ? 42 : -7));
        method = 6;
        uint64_t counters[2] = {42, 42};
        CHECK(iris_vpp_profile(host_handle, 1, counters, 2) == failure);
        CHECK(counters[0] == (fail ? 42 : elapsed) && counters[1] == (fail ? 42 : UINT64_MAX));
        const unsigned methods[] = {4, 7, 8, 9};
        for (unsigned i = 0; i < 4; i++) {
            method = methods[i];
            memset(dst, 0x22, sizeof(dst));
            uint64_t us = 42;
            int err;
            if (method == 4)
                err = iris_vpp_run(host_handle, src, 7, dst, 11, 17, 19, 23, 29, 31, 37, 3, &us);
            else if (method == 7)
                err = iris_vpp_process(host_handle, src, 7, dst, 11, 17, 19, 23, 29, 31, 37, 3, 1,
                                       0, &us);
            else if (method == 8)
                err = iris_vpp_transform(host_handle, src, 7, dst, 11, 17, 19, 23, 29, 31, 37, 3, 1,
                                         0, 2, &us);
            else
                err = iris_vpp_overlay(host_handle, src, 7, rgba, 9, dst, 11, overlay_params, 5, 3,
                                       &us);
            CHECK(err == failure && us == (fail ? 42 : elapsed));
            CHECK(dst[0] == (fail ? 0x22 : 0x55));
        }
    }
    CHECK(calls == 28 && transport_calls == 20);
    CHECK(iris_vpp_skel_handle_invoke(dsp_handle, REMOTE_SCALARS_MAKE(10, 0, 0), NULL) ==
          AEE_EUNSUPPORTED);
#ifndef RPC_QAIC_STUB
    // Invalid native inputs must fail before invoking the transport.
    CHECK(iris_vpp_open(NULL, NULL) == AEE_EBADPARM);
    CHECK(iris_vpp_power(host_handle, NULL) == AEE_EBADPARM);
    unsigned hz;
    CHECK(iris_vpp_info(host_handle, &hz, NULL) == AEE_EBADPARM);
    CHECK(iris_vpp_profile(host_handle, 1, NULL, 1) == AEE_EBADPARM);
    uint64_t us;
    CHECK(iris_vpp_profile(host_handle, 1, &us, -1) == AEE_EBADPARM);
    CHECK(iris_vpp_run(host_handle, src, -1, dst, 11, 0, 0, 0, 0, 0, 0, 1, &us) == AEE_EBADPARM);
    CHECK(iris_vpp_process(host_handle, src, 7, dst, 11, 0, 0, 0, 0, 0, 0, 1, 0, 0, NULL) ==
          AEE_EBADPARM);
    CHECK(iris_vpp_transform(host_handle, NULL, 7, dst, 11, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, &us) ==
          AEE_EBADPARM);
    CHECK(iris_vpp_overlay(host_handle, src, 7, rgba, 9, dst, 11, NULL, 5, 1, &us) == AEE_EBADPARM);
    CHECK(transport_calls == 20);
#endif
    puts("PASS: all 10 RPC methods, wire fixtures, error outputs, inrout buffers and malformed "
         "messages");
    return 0;
}
