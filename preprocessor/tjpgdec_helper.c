// Written by Alun Morris and Claude Code
/* tjpgdec_helper.c — decodes JPEG via TJpgDec (exact firmware algorithm).
 * Reads JPEG from stdin, writes to stdout:
 *   uint32_t out_w, uint32_t out_h  (little-endian)
 *   then out_w * out_h * 3 bytes of RGB888
 * at 1/2 scale (matching firmware setJpgScale(2)).
 *
 * Compile:
 *   gcc -O2 -I../firmware/lib/TJpg_Decoder/src \
 *       ../firmware/lib/TJpg_Decoder/src/tjpgd.c \
 *       tjpgdec_helper.c -o tjpgdec_helper
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tjpgd.h"

typedef struct {
    const uint8_t *data;
    size_t size, pos;
    uint8_t *out;
    int out_w, out_h;
    uint32_t csum;
    uint32_t npix;
} Ctx;

static size_t in_func(JDEC *jd, uint8_t *buf, size_t sz) {
    Ctx *c = (Ctx *)jd->device;
    size_t avail = c->size - c->pos;
    if (sz > avail) sz = avail;
    if (buf) memcpy(buf, c->data + c->pos, sz);
    c->pos += sz;
    return sz;
}

static int out_func(JDEC *jd, void *bmp, JRECT *r) {
    Ctx *c = (Ctx *)jd->device;
    uint8_t *src = (uint8_t *)bmp;
    int bw = r->right  - r->left + 1;
    int bh = r->bottom - r->top  + 1;
    for (int row = 0; row < bh; row++) {
        int y = r->top + row;
        if (y >= c->out_h) continue;
        for (int col = 0; col < bw; col++) {
            int x = r->left + col;
            if (x >= c->out_w) continue;
            int di = (y * c->out_w + x) * 3;
            int si = (row * bw + col) * 3;
            c->out[di]   = src[si];
            c->out[di+1] = src[si+1];
            c->out[di+2] = src[si+2];
            uint16_t px = ((uint16_t)(src[si  ]>>3)<<11)
                        | ((uint16_t)(src[si+1]>>2)<< 5)
                        |  (uint16_t)(src[si+2]>>3);
            c->csum = c->csum * 31 + px;
            c->npix++;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    /* Optional first arg: scale factor 1, 2, 4, or 8 (default 2) */
    int scale = (argc > 1) ? atoi(argv[1]) : 2;
    uint8_t jd_scale = (scale==1)?0:(scale==2)?1:(scale==4)?2:3;

    uint8_t *buf = NULL;
    size_t sz = 0, cap = 0;
    int ch;
    while ((ch = getchar()) != EOF) {
        if (sz >= cap) { cap = cap ? cap * 2 : 65536; buf = realloc(buf, cap); }
        buf[sz++] = (uint8_t)ch;
    }

    uint8_t workspace[TJPGD_WORKSPACE_SIZE];
    JDEC jd;
    Ctx ctx = { buf, sz, 0, NULL, 0, 0 };

    JRESULT rc = jd_prepare(&jd, in_func, workspace, sizeof(workspace), &ctx);
    if (rc != JDR_OK) { fprintf(stderr, "jd_prepare failed: %d\n", (int)rc); return 1; }

    ctx.out_w = (int)(jd.width  >> jd_scale);
    ctx.out_h = (int)(jd.height >> jd_scale);
    ctx.out   = calloc((size_t)ctx.out_w * ctx.out_h * 3, 1);
    if (!ctx.out) { fprintf(stderr, "OOM\n"); return 1; }

    rc = jd_decomp(&jd, out_func, jd_scale);
    if (rc != JDR_OK) { fprintf(stderr, "jd_decomp failed: %d\n", (int)rc); return 1; }

    uint32_t w = (uint32_t)ctx.out_w, h = (uint32_t)ctx.out_h;
    fwrite(&w, 4, 1, stdout);
    fwrite(&h, 4, 1, stdout);
    fwrite(ctx.out, 3, (size_t)w * h, stdout);
    fwrite(&ctx.csum, 4, 1, stdout);
    fwrite(&ctx.npix, 4, 1, stdout);

    free(ctx.out);
    free(buf);
    return 0;
}
