#include "fb_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAMFBCfg written by EDK2 QemuRamFbDxe via fw_cfg DMA to "etc/ramfb" (28 bytes, big-endian):
   struct RAMFBCfg {
       uint64_t addr;
       uint32_t fourcc;
       uint32_t flags;
       uint32_t width;
       uint32_t height;
       uint32_t stride;
   } __attribute__((packed));
*/

static uint32_t be32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static uint64_t be64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static void on_ramfb_write(void *opaque, const uint8_t *data, uint32_t len) {
    HvFbDisplay *fb = opaque;
    if (len < 28) return;

    uint64_t addr = be64(data + 0);
    uint32_t fourcc = be32(data + 8);
    uint32_t width = be32(data + 16);
    uint32_t height = be32(data + 20);
    uint32_t stride = be32(data + 24);

    if (width < 64 || width > 8192 || height < 64 || height > 8192) return;
    if (stride == 0) stride = width * 4;
    if (stride < width * 2 || stride > 65536) return;

    uint64_t total_bytes = (uint64_t)stride * (height - 1) + (uint64_t)width * 4;
    void *host_ptr = hv_phys_ptr(fb->m, addr, total_bytes);
    if (!host_ptr) {
        fprintf(stderr, "winvm: ramfb ignored invalid PA 0x%llx (%ux%u stride %u)\n",
                (unsigned long long)addr, width, height, stride);
        return;
    }

    pthread_mutex_lock(&fb->mu);
    fb->fb_pa = addr;
    fb->fb_host = (const uint8_t *)host_ptr;
    fb->width = width;
    fb->height = height;
    fb->stride = stride;
    fb->fourcc = fourcc;
    fb->active = 1;
    pthread_mutex_unlock(&fb->mu);

    fprintf(stderr, "winvm: ramfb configured %ux%u stride=%u fourcc=0x%08x at PA 0x%llx\n",
            width, height, stride, fourcc, (unsigned long long)addr);
}

HvFbDisplay *hv_fb_display_create(HvMachine *m, HvFwCfg *fw_cfg) {
    HvFbDisplay *fb = calloc(1, sizeof *fb);
    fb->m = m;
    pthread_mutex_init(&fb->mu, NULL);
    if (fw_cfg) {
        hv_fw_cfg_add_writable_file(fw_cfg, "etc/ramfb", 28, on_ramfb_write, fb);
    }
    return fb;
}

int hv_fb_display_get_frame(HvFbDisplay *fb, const uint8_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride) {
    if (!fb) return -1;
    pthread_mutex_lock(&fb->mu);
    if (!fb->active || !fb->fb_host || !fb->width || !fb->height) {
        pthread_mutex_unlock(&fb->mu);
        return -1;
    }
    *pixels = fb->fb_host;
    *w = fb->width;
    *h = fb->height;
    *stride = fb->stride;
    pthread_mutex_unlock(&fb->mu);
    return 0;
}

#ifndef __APPLE__
static uint32_t png_crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int write_png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t hdr[8], crc_buf[4];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    uint32_t crc = png_crc32_update(0xffffffffu, (const uint8_t *)type, 4);
    if (len) crc = png_crc32_update(crc, data, len);
    put_be32(crc_buf, crc ^ 0xffffffffu);
    return fwrite(crc_buf, 1, 4, f) == 4 ? 0 : -1;
}

int hv_fb_save_png(HvFbDisplay *fb, const char *path) {
    const uint8_t *px = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    if (!path || hv_fb_display_get_frame(fb, &px, &w, &h, &stride) != 0) return -1;

    size_t row_bytes = 1 + (size_t)w * 3;
    size_t raw_size = row_bytes * h;
    size_t num_blocks = (raw_size + 65534) / 65535;
    size_t idat_size = 2 + num_blocks * 5 + raw_size + 4;
    uint8_t *raw = malloc(raw_size);
    uint8_t *idat = malloc(idat_size);
    if (!raw || !idat) {
        free(raw);
        free(idat);
        return -1;
    }

    uint32_t s1 = 1, s2 = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = px + (size_t)y * stride;
        uint8_t *dst = raw + (size_t)y * row_bytes;
        dst[0] = 0; /* Filter type 0 (None) */
        for (uint32_t x = 0; x < w; x++) {
            /* ramfb is BGRX8888 in little-endian memory */
            dst[1 + x * 3 + 0] = src[x * 4 + 2]; /* R */
            dst[1 + x * 3 + 1] = src[x * 4 + 1]; /* G */
            dst[1 + x * 3 + 2] = src[x * 4 + 0]; /* B */
        }
        for (size_t i = 0; i < row_bytes; i++) {
            s1 = (s1 + dst[i]) % 65521u;
            s2 = (s2 + s1) % 65521u;
        }
    }

    size_t pos = 0;
    idat[pos++] = 0x78; /* zlib CMF */
    idat[pos++] = 0x01; /* zlib FLG */
    size_t rem = raw_size, off = 0;
    while (rem > 0) {
        uint16_t blk = rem > 65535 ? 65535 : (uint16_t)rem;
        uint16_t nblk = (uint16_t)~blk;
        idat[pos++] = (rem <= 65535) ? 0x01 : 0x00; /* BFINAL | BTYPE=00 */
        idat[pos++] = (uint8_t)(blk & 0xff);
        idat[pos++] = (uint8_t)(blk >> 8);
        idat[pos++] = (uint8_t)(nblk & 0xff);
        idat[pos++] = (uint8_t)(nblk >> 8);
        memcpy(idat + pos, raw + off, blk);
        pos += blk;
        off += blk;
        rem -= blk;
    }
    put_be32(idat + pos, (s2 << 16) | s1);
    pos += 4;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(idat);
        return -1;
    }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t ihdr[13] = {0};
    put_be32(ihdr + 0, w);
    put_be32(ihdr + 4, h);
    ihdr[8] = 8; /* 8 bits per channel */
    ihdr[9] = 2; /* Truecolor RGB */

    int rc = 0;
    if (fwrite(sig, 1, 8, f) != 8 ||
        write_png_chunk(f, "IHDR", ihdr, 13) != 0 ||
        write_png_chunk(f, "IDAT", idat, (uint32_t)pos) != 0 ||
        write_png_chunk(f, "IEND", NULL, 0) != 0) {
        rc = -1;
    }
    fclose(f);
    free(idat);
    return rc;
}
#endif
