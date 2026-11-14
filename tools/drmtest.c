/* drmtest — عميل DRM/KMS صغير يعمل داخل الضيف لاختبار العرض دون fbdev.
 * يكتشف الموصل والنمط، وينشئ مخزنًا (dumb buffer)، ويرسم نمطًا، ثم يعرضه عبر SETCRTC.
 * هذا المسار نفسه الذي تستخدمه طبقة hwcomposer في أندرويد.
 * البناء: zig cc -target aarch64-linux-musl -static -O2 -o drmtest tools/drmtest.c */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>

#define IOWR(nr, sz) ((3u << 30) | ((sz) << 16) | ('d' << 8) | (nr))

struct card_res { uint64_t fb, crtc, conn, enc; uint32_t nfb, ncrtc, nconn, nenc, minw, maxw, minh, maxh; };
struct modeinfo { uint32_t clock; uint16_t hd, hss, hse, ht, hskew, vd, vss, vse, vt, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct get_conn { uint64_t enc, modes, props, propv; uint32_t nmodes, nprops, nenc, enc_id, conn_id, type, type_id, connection, mmw, mmh, subpix, pad; };
struct get_enc { uint32_t id, type, crtc, possible_crtcs, clones; };
struct create_dumb { uint32_t h, w, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct fb_cmd { uint32_t fb_id, w, h, pitch, bpp, depth, handle; };
struct set_crtc { uint64_t conns; uint32_t nconn, crtc_id, fb_id, x, y, gamma, mode_valid; struct modeinfo mode; };
struct dirty { uint32_t fb_id, flags, color, nclips; uint64_t clips; };

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }
    uint32_t crtcs[8], conns[8], encs[8];
    struct card_res r = {0};
    if (ioctl(fd, IOWR(0xA0, sizeof r), &r)) { perror("GETRESOURCES"); return 1; }
    r.crtc = (uintptr_t)crtcs; r.conn = (uintptr_t)conns; r.enc = (uintptr_t)encs; r.fb = 0; r.nfb = 0;
    if (r.ncrtc > 8) r.ncrtc = 8;
    if (r.nconn > 8) r.nconn = 8;
    if (r.nenc > 8) r.nenc = 8;
    if (ioctl(fd, IOWR(0xA0, sizeof r), &r)) { perror("GETRESOURCES"); return 1; }
    printf("drm: crtcs=%u connectors=%u encoders=%u\n", r.ncrtc, r.nconn, r.nenc);
    for (uint32_t i = 0; i < r.nconn; i++) {
        static struct modeinfo modes[128];
        uint32_t cencs[8];
        struct get_conn c = { .conn_id = conns[i] };
        if (ioctl(fd, IOWR(0xA7, sizeof c), &c)) continue;          /* يطلب فحص الموصل */
        if (c.nmodes > 128) continue;
        if (c.nenc > 8) c.nenc = 8;
        c.modes = (uintptr_t)modes; c.enc = (uintptr_t)cencs; c.nprops = 0;
        if (ioctl(fd, IOWR(0xA7, sizeof c), &c)) continue;
        printf("drm: connector %u connection=%u modes=%u\n", c.conn_id, c.connection, c.nmodes);
        if (c.connection != 1 || !c.nmodes) continue;
        struct modeinfo *m = &modes[0];
        printf("drm: mode %s %ux%u@%u\n", m->name, m->hd, m->vd, m->vrefresh);
        uint32_t crtc = crtcs[0];
        if (c.enc_id || c.nenc) {
            struct get_enc e = { .id = c.enc_id ? c.enc_id : cencs[0] };
            if (!ioctl(fd, IOWR(0xA6, sizeof e), &e))
                for (uint32_t k = 0; k < r.ncrtc; k++) if (e.possible_crtcs & (1u << k)) { crtc = crtcs[k]; break; }
        }
        struct create_dumb cd = { .h = m->vd, .w = m->hd, .bpp = 32 };
        if (ioctl(fd, IOWR(0xB2, sizeof cd), &cd)) { perror("CREATE_DUMB"); return 1; }
        struct fb_cmd fb = { .w = m->hd, .h = m->vd, .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle };
        if (ioctl(fd, IOWR(0xAE, sizeof fb), &fb)) { perror("ADDFB"); return 1; }
        struct map_dumb md = { .handle = cd.handle };
        if (ioctl(fd, IOWR(0xB3, sizeof md), &md)) { perror("MAP_DUMB"); return 1; }
        uint32_t *px = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
        if (px == MAP_FAILED) { perror("mmap"); return 1; }
        /* نمط اختبار: ثمانية أشرطة ألوان مع مربع أبيض في المنتصف */
        static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x101010 };
        for (uint32_t y = 0; y < m->vd; y++)
            for (uint32_t x = 0; x < m->hd; x++) {
                uint32_t v = bars[x * 8 / m->hd];
                if (x > m->hd / 2 - 60 && x < m->hd / 2 + 60 && y > m->vd / 2 - 60 && y < m->vd / 2 + 60) v = 0xFFFFFF;
                px[y * (cd.pitch / 4) + x] = v;
            }
        struct set_crtc sc = { .conns = (uintptr_t)&c.conn_id, .nconn = 1, .crtc_id = crtc, .fb_id = fb.fb_id, .mode_valid = 1, .mode = *m };
        if (ioctl(fd, IOWR(0xA2, sizeof sc), &sc)) { perror("SETCRTC"); return 1; }
        struct dirty d = { .fb_id = fb.fb_id };
        ioctl(fd, IOWR(0xB1, sizeof d), &d);
        printf("drm: DISPLAY-OK fb=%u crtc=%u %ux%u\n", fb.fb_id, crtc, m->hd, m->vd);
        if (argc > 2) sleep(atoi(argv[2]));
        return 0;
    }
    printf("drm: no connected output\n");
    return 1;
}
