/*
 * ps1_tim_tool.c - self-contained PS1 TIM image converter
 *
 * Reimplementation of ps1_tim_tool.py using libpng, libjpeg, giflib,
 * libtiff and libwebp. BMP/TGA/PNM/ICO/DDS codecs are implemented here.
 * No external converter or child process is used.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <dirent.h>

#include <png.h>
#include <jpeglib.h>
#include <gif_lib.h>
#include <tiffio.h>
#include <webp/decode.h>
#include <webp/encode.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TIM_ID 0x00000010u
#define PMODE_4BIT 0
#define PMODE_8BIT 1
#define PMODE_16BIT 2
#define PMODE_24BIT 3
#define CF_CLUT_PRESENT 0x8u

static char g_error[1024];
static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return p;
}
static void *xcalloc(size_t n, size_t s) {
    if (s && n > SIZE_MAX / s) {
        fprintf(stderr, "fatal: allocation overflow\n");
        exit(2);
    }
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(2);
    }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t rds32(const uint8_t *p) {
    return (int32_t)rd32(p);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static int align_up(int n, int m) {
    return ((n + m - 1) / m) * m;
}
static bool is_power2(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}
static int prev_power2(int n) {
    int p = 1;
    if (n <= 0)
        return 1;
    while (p <= INT_MAX / 2 && p * 2 <= n)
        p *= 2;
    return p;
}
static int next_power2(int n) {
    int p = 1;
    if (n <= 0)
        return 1;
    while (p < n && p <= INT_MAX / 2)
        p *= 2;
    return p;
}

static bool checked_pixels(int w, int h, size_t channels, size_t *out) {
    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / (size_t)h ||
        (size_t)w * (size_t)h > SIZE_MAX / channels) {
        set_error("invalid or excessive image dimensions: %dx%d", w, h);
        return false;
    }
    *out = (size_t)w * (size_t)h * channels;
    return true;
}

typedef struct {
    int w, h;
    uint8_t *rgba;
} Image;
static Image *image_new(int w, int h) {
    size_t n;
    if (!checked_pixels(w, h, 4, &n))
        return NULL;
    Image *im = xcalloc(1, sizeof(*im));
    im->w = w;
    im->h = h;
    im->rgba = xcalloc(n, 1);
    return im;
}
static void image_free(Image *im) {
    if (im) {
        free(im->rgba);
        free(im);
    }
}

static bool read_file(const char *path, uint8_t **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error("cannot open '%s': %s", path, strerror(errno));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        set_error("cannot seek '%s'", path);
        fclose(f);
        return false;
    }
    long z = ftell(f);
    if (z < 0) {
        set_error("cannot size '%s'", path);
        fclose(f);
        return false;
    }
    rewind(f);
    *data = xmalloc((size_t)z + 1);
    *len = (size_t)z;
    if (*len && fread(*data, 1, *len, f) != *len) {
        set_error("cannot read '%s'", path);
        free(*data);
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}
static bool write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_error("cannot create '%s': %s", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        set_error("cannot write '%s'", path);
    return ok;
}
static const char *path_ext(const char *p) {
    const char *slash = strrchr(p, '/'), *dot = strrchr(p, '.');
    return dot && (!slash || dot > slash) ? dot : "";
}
static const char *path_base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static char *replace_ext(const char *p, const char *ext) {
    const char *dot = path_ext(p);
    size_t base = dot[0] ? (size_t)(dot - p) : strlen(p), ne = strlen(ext);
    char *r = xmalloc(base + ne + 1);
    memcpy(r, p, base);
    memcpy(r + base, ext, ne + 1);
    return r;
}
static char *join_output(const char *dir, const char *src, const char *ext) {
    const char *b = path_base(src), *dot = path_ext(b);
    size_t stem = dot[0] ? (size_t)(dot - b) : strlen(b);
    size_t nd = strlen(dir), ne = strlen(ext);
    char *r = xmalloc(nd + 1 + stem + ne + 1);
    memcpy(r, dir, nd);
    if (nd && dir[nd - 1] != '/')
        r[nd++] = '/';
    memcpy(r + nd, b, stem);
    memcpy(r + nd + stem, ext, ne + 1);
    return r;
}
static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool mkdir_p(const char *p) {
    char tmp[PATH_MAX];
    size_t n = strlen(p);
    if (n >= sizeof(tmp)) {
        set_error("path too long");
        return false;
    }
    memcpy(tmp, p, n + 1);
    if (!n)
        return true;
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            if (mkdir(tmp, 0777) && errno != EEXIST) {
                set_error("mkdir '%s': %s", tmp, strerror(errno));
                return false;
            }
            *q = '/';
        }
    if (mkdir(tmp, 0777) && errno != EEXIST) {
        set_error("mkdir '%s': %s", tmp, strerror(errno));
        return false;
    }
    return true;
}

/* ---------- PNG ---------- */
static Image *load_png_memory(const uint8_t *buf, size_t len) {
    png_image p;
    memset(&p, 0, sizeof(p));
    p.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&p, buf, len)) {
        set_error("PNG: %s", p.message);
        return NULL;
    }
    if (p.width > INT_MAX || p.height > INT_MAX) {
        png_image_free(&p);
        set_error("PNG dimensions too large");
        return NULL;
    }
    p.format = PNG_FORMAT_RGBA;
    Image *im = image_new((int)p.width, (int)p.height);
    if (!im) {
        png_image_free(&p);
        return NULL;
    }
    if (!png_image_finish_read(&p, NULL, im->rgba, 0, NULL)) {
        set_error("PNG: %s", p.message);
        png_image_free(&p);
        image_free(im);
        return NULL;
    }
    png_image_free(&p);
    return im;
}
static Image *load_png(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    Image *im = load_png_memory(d, n);
    free(d);
    return im;
}

/* ---------- JPEG ---------- */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
    char msg[JMSG_LENGTH_MAX];
} JpegErr;
static void jpeg_fail(j_common_ptr c) {
    JpegErr *e = (JpegErr *)c->err;
    (*c->err->format_message)(c, e->msg);
    longjmp(e->jump, 1);
}
static Image *load_jpeg(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error("cannot open '%s'", path);
        return NULL;
    }
    struct jpeg_decompress_struct c;
    JpegErr e;
    Image *im = NULL;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = jpeg_fail;
    if (setjmp(e.jump)) {
        set_error("JPEG: %s", e.msg);
        jpeg_destroy_decompress(&c);
        fclose(f);
        image_free(im);
        return NULL;
    }
    jpeg_create_decompress(&c);
    jpeg_stdio_src(&c, f);
    jpeg_read_header(&c, TRUE);
    c.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c);
    if (c.output_width > INT_MAX || c.output_height > INT_MAX) {
        set_error("JPEG dimensions too large");
        jpeg_destroy_decompress(&c);
        fclose(f);
        return NULL;
    }
    im = image_new((int)c.output_width, (int)c.output_height);
    if (!im) {
        jpeg_destroy_decompress(&c);
        fclose(f);
        return NULL;
    }
    size_t row = (size_t)im->w * c.output_components;
    uint8_t *scan = xmalloc(row);
    while (c.output_scanline < c.output_height) {
        JSAMPROW rp = scan;
        jpeg_read_scanlines(&c, &rp, 1);
        size_t y = (size_t)c.output_scanline - 1;
        for (int x = 0; x < im->w; x++) {
            uint8_t *o = im->rgba + (y * (size_t)im->w + x) * 4;
            if (c.output_components == 1)
                o[0] = o[1] = o[2] = scan[x];
            else {
                o[0] = scan[x * c.output_components];
                o[1] = scan[x * c.output_components + 1];
                o[2] = scan[x * c.output_components + 2];
            }
            o[3] = 255;
        }
    }
    free(scan);
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    fclose(f);
    return im;
}

/* ---------- GIF (first frame) ---------- */
static Image *load_gif(const char *path) {
    int ec = 0;
    GifFileType *g = DGifOpenFileName(path, &ec);
    if (!g) {
        set_error("GIF open error %d", ec);
        return NULL;
    }
    if (DGifSlurp(g) != GIF_OK || g->ImageCount < 1) {
        set_error("GIF decode failed");
        DGifCloseFile(g, &ec);
        return NULL;
    }
    SavedImage *s = &g->SavedImages[0];
    int w = g->SWidth, h = g->SHeight;
    Image *im = image_new(w, h);
    if (!im) {
        DGifCloseFile(g, &ec);
        return NULL;
    }
    ColorMapObject *cm = s->ImageDesc.ColorMap ? s->ImageDesc.ColorMap : g->SColorMap;
    if (!cm) {
        set_error("GIF has no color map");
        image_free(im);
        DGifCloseFile(g, &ec);
        return NULL;
    }
    int trans = -1;
    GraphicsControlBlock cb;
    if (DGifSavedExtensionToGCB(g, 0, &cb) == GIF_OK)
        trans = cb.TransparentColor;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
            o[0] = o[1] = o[2] = 0;
            o[3] = 0;
        }
    for (int y = 0; y < s->ImageDesc.Height; y++)
        for (int x = 0; x < s->ImageDesc.Width; x++) {
            int dx = s->ImageDesc.Left + x, dy = s->ImageDesc.Top + y;
            if (dx < 0 || dy < 0 || dx >= w || dy >= h)
                continue;
            int idx = s->RasterBits[(size_t)y * s->ImageDesc.Width + x];
            uint8_t *o = im->rgba + ((size_t)dy * w + dx) * 4;
            if (idx >= 0 && idx < cm->ColorCount) {
                o[0] = cm->Colors[idx].Red;
                o[1] = cm->Colors[idx].Green;
                o[2] = cm->Colors[idx].Blue;
                o[3] = (idx == trans) ? 0 : 255;
            }
        }
    DGifCloseFile(g, &ec);
    return im;
}

/* ---------- TIFF ---------- */
static Image *load_tiff(const char *path) {
    TIFF *t = TIFFOpen(path, "r");
    if (!t) {
        set_error("TIFF open failed");
        return NULL;
    }
    uint32_t w = 0, h = 0;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    if (!w || !h || w > INT_MAX || h > INT_MAX) {
        set_error("bad TIFF dimensions");
        TIFFClose(t);
        return NULL;
    }
    size_t np = (size_t)w * h;
    if (np > SIZE_MAX / sizeof(uint32_t)) {
        set_error("TIFF too large");
        TIFFClose(t);
        return NULL;
    }
    uint32_t *r = xmalloc(np * sizeof(*r));
    if (!TIFFReadRGBAImageOriented(t, w, h, r, ORIENTATION_TOPLEFT, 0)) {
        set_error("TIFF decode failed");
        free(r);
        TIFFClose(t);
        return NULL;
    }
    Image *im = image_new((int)w, (int)h);
    if (im)
        for (size_t i = 0; i < np; i++) {
            im->rgba[i * 4] = TIFFGetR(r[i]);
            im->rgba[i * 4 + 1] = TIFFGetG(r[i]);
            im->rgba[i * 4 + 2] = TIFFGetB(r[i]);
            im->rgba[i * 4 + 3] = TIFFGetA(r[i]);
        }
    free(r);
    TIFFClose(t);
    return im;
}

/* ---------- WebP ---------- */
static Image *load_webp(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    int w, h;
    if (!WebPGetInfo(d, n, &w, &h)) {
        set_error("invalid WebP");
        free(d);
        return NULL;
    }
    Image *im = image_new(w, h);
    if (!im) {
        free(d);
        return NULL;
    }
    if (!WebPDecodeRGBAInto(d, n, im->rgba, (size_t)w * h * 4, w * 4)) {
        set_error("WebP decode failed");
        image_free(im);
        im = NULL;
    }
    free(d);
    return im;
}

/* ---------- Netpbm P1..P6 ---------- */
typedef struct {
    const uint8_t *p, *end;
} PnmScan;
static bool pnm_token(PnmScan *s, char *out, size_t cap) {
    while (s->p < s->end) {
        if (isspace(*s->p)) {
            s->p++;
            continue;
        }
        if (*s->p == '#') {
            while (s->p < s->end && *s->p != '\n')
                s->p++;
            continue;
        }
        break;
    }
    if (s->p >= s->end)
        return false;
    size_t n = 0;
    while (s->p < s->end && !isspace(*s->p) && *s->p != '#') {
        if (n + 1 < cap)
            out[n++] = (char)*s->p;
        s->p++;
    }
    out[n] = 0;
    return n > 0;
}
static bool pnm_int(PnmScan *s, int *v) {
    char t[64], *e;
    if (!pnm_token(s, t, sizeof(t)))
        return false;
    long x = strtol(t, &e, 10);
    if (*e || x < 0 || x > INT_MAX)
        return false;
    *v = (int)x;
    return true;
}
static Image *load_pnm(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    PnmScan s = {d, d + n};
    char magic[8];
    int w, h, maxv = 1;
    if (!pnm_token(&s, magic, sizeof(magic)) || strlen(magic) != 2 || magic[0] != 'P' ||
        magic[1] < '1' || magic[1] > '6' || !pnm_int(&s, &w) || !pnm_int(&s, &h)) {
        set_error("invalid PNM header");
        free(d);
        return NULL;
    }
    int type = magic[1] - '0';
    if (type != 1 && type != 4) {
        if (!pnm_int(&s, &maxv) || maxv <= 0 || maxv > 65535) {
            set_error("invalid PNM maxval");
            free(d);
            return NULL;
        }
    }
    Image *im = image_new(w, h);
    if (!im) {
        free(d);
        return NULL;
    }
    size_t np = (size_t)w * h;
    if (type <= 3) {
        for (size_t i = 0; i < np; i++) {
            int a = 0, b = 0, c = 0;
            if (type == 1) {
                if (!pnm_int(&s, &a))
                    goto bad;
            } else if (type == 2) {
                if (!pnm_int(&s, &a))
                    goto bad;
            } else {
                if (!pnm_int(&s, &a) || !pnm_int(&s, &b) || !pnm_int(&s, &c))
                    goto bad;
            }
            uint8_t *o = im->rgba + i * 4;
            if (type == 1)
                o[0] = o[1] = o[2] = (uint8_t)(a ? 0 : 255);
            else if (type == 2)
                o[0] = o[1] = o[2] = (uint8_t)((a * 255L + maxv / 2) / maxv);
            else {
                o[0] = (uint8_t)((a * 255L + maxv / 2) / maxv);
                o[1] = (uint8_t)((b * 255L + maxv / 2) / maxv);
                o[2] = (uint8_t)((c * 255L + maxv / 2) / maxv);
            }
            o[3] = 255;
        }
    } else {
        /* A binary PNM raster starts immediately after the single whitespace
           delimiter following the last header token. Skipping every whitespace
           byte would eat legitimate first pixels such as 0x0A and 0x20. */
        if (s.p < s.end && isspace((unsigned char)*s.p)) {
            uint8_t first = *s.p++;
            if (first == '\r' && s.p < s.end && *s.p == '\n')
                s.p++;
        }
        if (type == 4) {
            size_t stride = ((size_t)w + 7) / 8;
            if ((size_t)(s.end - s.p) < stride * (size_t)h)
                goto bad;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int bit = (s.p[(size_t)y * stride + x / 8] >> (7 - (x & 7))) & 1;
                    uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
                    o[0] = o[1] = o[2] = (uint8_t)(bit ? 0 : 255);
                    o[3] = 255;
                }
        } else {
            int chans = type == 6 ? 3 : 1, bps = maxv > 255 ? 2 : 1;
            size_t need = np * (size_t)chans * bps;
            if ((size_t)(s.end - s.p) < need)
                goto bad;
            const uint8_t *q = s.p;
            for (size_t i = 0; i < np; i++) {
                int v[3] = {0, 0, 0};
                for (int k = 0; k < chans; k++) {
                    v[k] = bps == 1 ? *q++ : ((int)q[0] << 8) | q[1];
                    q += bps == 2 ? 2 : 0;
                }
                uint8_t *o = im->rgba + i * 4;
                if (chans == 1)
                    o[0] = o[1] = o[2] = (uint8_t)((v[0] * 255L + maxv / 2) / maxv);
                else
                    for (int k = 0; k < 3; k++)
                        o[k] = (uint8_t)((v[k] * 255L + maxv / 2) / maxv);
                o[3] = 255;
            }
        }
    }
    free(d);
    return im;
bad:
    set_error("truncated or malformed PNM pixel data");
    image_free(im);
    free(d);
    return NULL;
}

/* ---------- BMP/DIB ---------- */
static uint8_t scale_mask(uint32_t v, uint32_t mask) {
    if (!mask)
        return 0;
    unsigned sh = 0, bits = 0;
    while (((mask >> sh) & 1u) == 0u && sh < 32)
        sh++;
    uint32_t m = mask >> sh;
    while (m & 1u) {
        bits++;
        m >>= 1;
    }
    uint32_t x = (v & mask) >> sh, maxv = bits >= 32 ? UINT32_MAX : ((1u << bits) - 1u);
    return maxv ? (uint8_t)((x * 255ULL + maxv / 2) / maxv) : 0;
}
static bool decode_bmp_rle(const uint8_t *src, size_t n, int w, int h, int bpp, uint8_t *idx) {
    size_t p = 0;
    int x = 0, y = 0;
    while (p < n && y < h) {
        uint8_t a = src[p++];
        if (a) {
            if (p >= n)
                return false;
            uint8_t v = src[p++];
            for (int k = 0; k < a; k++) {
                if (x < w && y < h)
                    idx[(size_t)y * w + x] = (bpp == 8 ? v : (uint8_t)((k & 1) ? v & 15 : v >> 4));
                x++;
            }
        } else {
            if (p >= n)
                return false;
            uint8_t cmd = src[p++];
            if (cmd == 0) {
                x = 0;
                y++;
            } else if (cmd == 1)
                return true;
            else if (cmd == 2) {
                if (p + 2 > n)
                    return false;
                x += src[p++];
                y += src[p++];
            } else {
                int count = cmd;
                if (bpp == 8) {
                    if (p + (size_t)count > n)
                        return false;
                    for (int k = 0; k < count; k++) {
                        if (x < w && y < h)
                            idx[(size_t)y * w + x] = src[p + k];
                        x++;
                    }
                    p += (size_t)count;
                    if (count & 1)
                        p++;
                } else {
                    size_t bytes = (size_t)(count + 1) / 2;
                    if (p + bytes > n)
                        return false;
                    for (int k = 0; k < count; k++) {
                        uint8_t v = src[p + k / 2];
                        if (x < w && y < h)
                            idx[(size_t)y * w + x] = (uint8_t)((k & 1) ? v & 15 : v >> 4);
                        x++;
                    }
                    p += bytes;
                    if (bytes & 1)
                        p++;
                }
            }
        }
    }
    return y >= h;
}
static Image *load_bmp_memory(const uint8_t *d, size_t n, bool dib_only) {
    size_t off = 0, hs_off = 0;
    if (!dib_only) {
        if (n < 14 || d[0] != 'B' || d[1] != 'M') {
            set_error("invalid BMP signature");
            return NULL;
        }
        off = rd32(d + 10);
        hs_off = 14;
    }
    if (n < hs_off + 4) {
        set_error("truncated BMP");
        return NULL;
    }
    uint32_t hs = rd32(d + hs_off);
    if (hs < 12 || hs_off + hs > n) {
        set_error("unsupported BMP/DIB header");
        return NULL;
    }
    int w = 0, hraw = 0, bpp = 0;
    uint32_t compression = 0, colors = 0;
    size_t palette_off = hs_off + hs;
    bool core = hs == 12;
    if (core) {
        w = rd16(d + hs_off + 4);
        hraw = rd16(d + hs_off + 6);
        bpp = rd16(d + hs_off + 10);
    } else {
        w = rds32(d + hs_off + 4);
        hraw = rds32(d + hs_off + 8);
        bpp = rd16(d + hs_off + 14);
        compression = rd32(d + hs_off + 16);
        colors = rd32(d + hs_off + 32);
    }
    if (w <= 0 || hraw == 0 || abs(hraw) > INT_MAX / 2) {
        set_error("bad BMP dimensions");
        return NULL;
    }
    bool top = hraw < 0;
    int h = abs(hraw);
    if (dib_only && off == 0) {
        size_t pals = (bpp <= 8 ? (colors ? colors : (1u << bpp)) : 0);
        off = palette_off + pals * (core ? 3 : 4);
    }
    if (off > n) {
        set_error("bad BMP pixel offset");
        return NULL;
    }
    uint32_t rm = 0, gm = 0, bm = 0, am = 0;
    if (compression == 3 || compression == 6) {
        size_t mo = hs >= 52 ? hs_off + 40 : hs_off + hs;
        if (mo + 12 > n) {
            set_error("truncated BMP masks");
            return NULL;
        }
        rm = rd32(d + mo);
        gm = rd32(d + mo + 4);
        bm = rd32(d + mo + 8);
        if ((hs >= 56 || compression == 6) && mo + 16 <= n)
            am = rd32(d + mo + 12);
        if (hs == 40)
            palette_off = mo + (compression == 6 ? 16 : 12);
    } else if (bpp == 16) {
        rm = 0x7c00;
        gm = 0x03e0;
        bm = 0x001f;
    } else if (bpp == 32) {
        rm = 0x00ff0000;
        gm = 0x0000ff00;
        bm = 0x000000ff;
        am = 0xff000000;
    }
    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) {
        set_error("unsupported BMP bit depth %d", bpp);
        return NULL;
    }
    if (!(compression == 0 || compression == 3 || compression == 6 || compression == 1 ||
          compression == 2)) {
        set_error("unsupported BMP compression %u", compression);
        return NULL;
    }
    uint8_t pal[256][4];
    memset(pal, 0, sizeof(pal));
    if (bpp <= 8) {
        unsigned cnt = colors ? colors : (1u << bpp);
        if (cnt > 256)
            cnt = 256;
        size_t es = core ? 3 : 4;
        if (palette_off + (size_t)cnt * es > n) {
            set_error("truncated BMP palette");
            return NULL;
        }
        for (unsigned i = 0; i < cnt; i++) {
            pal[i][2] = d[palette_off + i * es];
            pal[i][1] = d[palette_off + i * es + 1];
            pal[i][0] = d[palette_off + i * es + 2];
            pal[i][3] = 255;
        }
    }
    Image *im = image_new(w, h);
    if (!im)
        return NULL;
    if (compression == 1 || compression == 2) {
        uint8_t *idx = xcalloc((size_t)w * h, 1);
        if (!decode_bmp_rle(d + off, n - off, w, h, bpp, idx)) {
            set_error("malformed BMP RLE");
            free(idx);
            image_free(im);
            return NULL;
        }
        for (int sy = 0; sy < h; sy++) {
            int dy = top ? sy : h - 1 - sy;
            for (int x = 0; x < w; x++) {
                uint8_t *o = im->rgba + ((size_t)dy * w + x) * 4;
                memcpy(o, pal[idx[(size_t)sy * w + x]], 4);
            }
        }
        free(idx);
        return im;
    }
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if (stride * (size_t)h > n - off) {
        set_error("truncated BMP pixels");
        image_free(im);
        return NULL;
    }
    bool any_alpha = false;
    for (int sy = 0; sy < h; sy++) {
        int dy = top ? sy : h - 1 - sy;
        const uint8_t *row = d + off + (size_t)sy * stride;
        for (int x = 0; x < w; x++) {
            uint8_t *o = im->rgba + ((size_t)dy * w + x) * 4;
            if (bpp <= 8) {
                unsigned idx = bpp == 8   ? row[x]
                               : bpp == 4 ? ((x & 1) ? row[x / 2] & 15 : row[x / 2] >> 4)
                                          : ((row[x / 8] >> (7 - (x & 7))) & 1);
                memcpy(o, pal[idx], 4);
            } else if (bpp == 24) {
                o[2] = row[x * 3];
                o[1] = row[x * 3 + 1];
                o[0] = row[x * 3 + 2];
                o[3] = 255;
            } else {
                uint32_t v = bpp == 16 ? rd16(row + x * 2) : rd32(row + x * 4);
                o[0] = scale_mask(v, rm);
                o[1] = scale_mask(v, gm);
                o[2] = scale_mask(v, bm);
                o[3] = am ? scale_mask(v, am) : 255;
                if (o[3])
                    any_alpha = true;
            }
        }
    }
    if (bpp == 32 && am && !any_alpha)
        for (size_t i = 0; i < (size_t)w * h; i++)
            im->rgba[i * 4 + 3] = 255;
    return im;
}
static Image *load_bmp(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    Image *im = load_bmp_memory(d, n, false);
    free(d);
    return im;
}

/* ---------- TGA ---------- */
static Image *load_tga(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    if (n < 18) {
        set_error("truncated TGA");
        free(d);
        return NULL;
    }
    int idlen = d[0], cmaptype = d[1], type = d[2], cfirst = rd16(d + 3), clen = rd16(d + 5),
        cdepth = d[7], w = rd16(d + 12), h = rd16(d + 14), depth = d[16], desc = d[17];
    bool rle = type == 9 || type == 10 || type == 11;
    int base = rle ? type - 8 : type;
    if (!(base == 1 || base == 2 || base == 3) || w <= 0 || h <= 0) {
        set_error("unsupported TGA type/dimensions");
        free(d);
        return NULL;
    }
    size_t p = 18 + (size_t)idlen;
    if (p > n) {
        set_error("truncated TGA id");
        free(d);
        return NULL;
    }
    uint8_t pal[256][4];
    memset(pal, 0, sizeof(pal));
    if (cmaptype) {
        if (cfirst + clen > 256 ||
            !(cdepth == 15 || cdepth == 16 || cdepth == 24 || cdepth == 32)) {
            set_error("unsupported TGA palette");
            free(d);
            return NULL;
        }
        int cb = (cdepth + 7) / 8;
        if (p + (size_t)clen * cb > n) {
            set_error("truncated TGA palette");
            free(d);
            return NULL;
        }
        for (int i = 0; i < clen; i++) {
            const uint8_t *q = d + p + (size_t)i * cb;
            uint8_t *o = pal[cfirst + i];
            if (cb == 2) {
                uint16_t v = rd16(q);
                o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                o[2] = (uint8_t)((v & 31) * 255 / 31);
                o[3] = (cdepth == 16 && !(v & 0x8000)) ? 0 : 255;
            } else {
                o[2] = q[0];
                o[1] = q[1];
                o[0] = q[2];
                o[3] = cb == 4 ? q[3] : 255;
            }
        }
        p += (size_t)clen * cb;
    }
    int bytes = (depth + 7) / 8;
    if (base == 1 && !(depth == 8 || depth == 16)) {
        set_error("unsupported TGA index depth");
        free(d);
        return NULL;
    }
    if (base == 2 && !(depth == 15 || depth == 16 || depth == 24 || depth == 32)) {
        set_error("unsupported TGA depth");
        free(d);
        return NULL;
    }
    if (base == 3 && !(depth == 8 || depth == 16)) {
        set_error("unsupported TGA gray depth");
        free(d);
        return NULL;
    }
    Image *im = image_new(w, h);
    if (!im) {
        free(d);
        return NULL;
    }
    size_t total = (size_t)w * h, pos = 0;
    bool top = !!(desc & 0x20), right = !!(desc & 0x10);
    uint8_t pix[4];
    while (pos < total) {
        int count = 1;
        bool repeat = false;
        if (rle) {
            if (p >= n)
                goto bad;
            uint8_t ph = d[p++];
            repeat = !!(ph & 0x80);
            count = (ph & 0x7f) + 1;
        }
        for (int j = 0; j < count && pos < total; j++) {
            if (!repeat || j == 0) {
                if (p + (size_t)bytes > n)
                    goto bad;
                memcpy(pix, d + p, bytes);
                p += (size_t)bytes;
            }
            int sx = (int)(pos % (size_t)w), sy = (int)(pos / (size_t)w);
            int x = right ? w - 1 - sx : sx, y = top ? sy : h - 1 - sy;
            uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
            if (base == 1) {
                unsigned idx = bytes == 1 ? pix[0] : rd16(pix);
                if (idx > 255)
                    goto bad;
                memcpy(o, pal[idx], 4);
            } else if (base == 3) {
                o[0] = o[1] = o[2] = pix[0];
                o[3] = bytes == 2 ? pix[1] : 255;
            } else if (bytes == 2) {
                uint16_t v = rd16(pix);
                /* TGA 15/16-bit words use A1R5G5B5 ordering. */
                o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                o[2] = (uint8_t)((v & 31) * 255 / 31);
                o[3] = (depth == 16 && (desc & 15) && !(v & 0x8000)) ? 0 : 255;
            } else {
                o[2] = pix[0];
                o[1] = pix[1];
                o[0] = pix[2];
                o[3] = bytes == 4 ? pix[3] : 255;
            }
            pos++;
        }
    }
    free(d);
    return im;
bad:
    set_error("truncated or malformed TGA");
    free(d);
    image_free(im);
    return NULL;
}

/* ---------- ICO (PNG or DIB image; chooses largest entry) ---------- */
static Image *load_ico(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    if (n < 6 || rd16(d) != 0 || rd16(d + 2) != 1 || rd16(d + 4) == 0) {
        set_error("invalid ICO");
        free(d);
        return NULL;
    }
    unsigned count = rd16(d + 4);
    if (n < 6 + (size_t)count * 16) {
        set_error("truncated ICO directory");
        free(d);
        return NULL;
    }
    unsigned best = 0;
    uint64_t area = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *e = d + 6 + i * 16;
        uint64_t w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, a = w * h;
        if (a > area) {
            area = a;
            best = i;
        }
    }
    const uint8_t *e = d + 6 + best * 16;
    size_t sz = rd32(e + 8), off = rd32(e + 12);
    if (off > n || sz > n - off) {
        set_error("bad ICO entry");
        free(d);
        return NULL;
    }
    Image *im = NULL;
    if (sz >= 8 && !memcmp(d + off, "\x89PNG\r\n\x1a\n", 8))
        im = load_png_memory(d + off, sz);
    else {
        uint8_t *copy = xmalloc(sz);
        memcpy(copy, d + off, sz);
        if (sz >= 12) {
            int32_t fullh = rds32(copy + 8);
            if (fullh > 1)
                wr32(copy + 8, (uint32_t)(fullh / 2));
        }
        im = load_bmp_memory(copy, sz, true);
        if (im) { /* DIB alpha may be accompanied by AND mask. */
            size_t hs = rd32(copy), bpp = hs >= 16 ? rd16(copy + 14) : 0;
            size_t pals = bpp <= 8 ? (1u << bpp) : 0, poff = hs + pals * 4,
                   row = (((size_t)im->w * bpp + 31) / 32) * 4,
                   and_off = poff + row * (size_t)im->h, size_and = ((size_t)im->w + 31) / 32 * 4;
            if (and_off + size_and * (size_t)im->h <= sz) {
                for (int y = 0; y < im->h; y++) {
                    const uint8_t *m = copy + and_off + (size_t)(im->h - 1 - y) * size_and;
                    for (int x = 0; x < im->w; x++)
                        if ((m[x / 8] >> (7 - (x & 7))) & 1)
                            im->rgba[((size_t)y * im->w + x) * 4 + 3] = 0;
                }
            }
        }
        free(copy);
    }
    free(d);
    return im;
}

/* ---------- DDS (uncompressed RGB(A), DXT1/DXT3/DXT5) ---------- */
static void dds_color(uint16_t v, uint8_t *c) {
    c[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    c[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    c[2] = (uint8_t)((v & 31) * 255 / 31);
    c[3] = 255;
}
static Image *load_dds(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    if (n < 128 || memcmp(d, "DDS ", 4) || rd32(d + 4) != 124) {
        set_error("invalid DDS");
        free(d);
        return NULL;
    }
    int h = (int)rd32(d + 12), w = (int)rd32(d + 16);
    uint32_t pf = rd32(d + 80), four = rd32(d + 84), bpp = rd32(d + 88), rm = rd32(d + 92),
             gm = rd32(d + 96), bm = rd32(d + 100), am = rd32(d + 104);
    Image *im = image_new(w, h);
    if (!im) {
        free(d);
        return NULL;
    }
    const uint8_t *p = d + 128, *end = d + n;
    if (pf & 4) {
        int mode = four == 0x31545844u ? 1 : four == 0x33545844u ? 3 : four == 0x35545844u ? 5 : 0;
        if (!mode) {
            set_error("unsupported DDS FourCC");
            goto bad;
        }
        int block = mode == 1 ? 8 : 16;
        size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * block;
        if ((size_t)(end - p) < need) {
            set_error("truncated DDS blocks");
            goto bad;
        }
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4) {
                uint8_t alpha[16];
                memset(alpha, 255, 16);
                const uint8_t *q = p;
                p += block;
                if (mode == 3) {
                    uint64_t a = 0;
                    for (int i = 0; i < 8; i++)
                        a |= (uint64_t)q[i] << (8 * i);
                    for (int i = 0; i < 16; i++)
                        alpha[i] = (uint8_t)(((a >> (i * 4)) & 15) * 17);
                    q += 8;
                } else if (mode == 5) {
                    uint8_t at[8], a0 = q[0], a1 = q[1];
                    at[0] = a0;
                    at[1] = a1;
                    if (a0 > a1) {
                        for (int i = 1; i <= 6; i++)
                            at[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
                    } else {
                        for (int i = 1; i <= 4; i++)
                            at[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
                        at[6] = 0;
                        at[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++)
                        bits |= (uint64_t)q[2 + i] << (8 * i);
                    for (int i = 0; i < 16; i++)
                        alpha[i] = at[(bits >> (i * 3)) & 7];
                    q += 8;
                }
                uint16_t c0 = rd16(q), c1 = rd16(q + 2);
                uint8_t c[4][4];
                dds_color(c0, c[0]);
                dds_color(c1, c[1]);
                if (c0 > c1 || mode != 1) {
                    for (int k = 0; k < 3; k++) {
                        c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3);
                        c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3);
                    }
                    c[2][3] = c[3][3] = 255;
                } else {
                    for (int k = 0; k < 3; k++)
                        c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2);
                    c[2][3] = 255;
                    memset(c[3], 0, 4);
                }
                uint32_t bits = rd32(q + 4);
                for (int py = 0; py < 4; py++)
                    for (int px = 0; px < 4; px++) {
                        int x = bx + px, y = by + py;
                        if (x >= w || y >= h)
                            continue;
                        int i = py * 4 + px, ci = (bits >> (2 * i)) & 3;
                        uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
                        memcpy(o, c[ci], 4);
                        o[3] = (mode == 1 && ci == 3 && c0 <= c1) ? 0 : alpha[i];
                    }
            }
    } else if (pf & 0x40) {
        int bytes = (int)((bpp + 7) / 8);
        if (!(bpp == 16 || bpp == 24 || bpp == 32)) {
            set_error("unsupported uncompressed DDS depth");
            goto bad;
        }
        size_t row = ((size_t)w * bytes + 3) & ~3u;
        if ((size_t)(end - p) < row * (size_t)h) {
            set_error("truncated DDS pixels");
            goto bad;
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t v = 0;
                memcpy(&v, p + (size_t)y * row + (size_t)x * bytes, bytes);
                uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
                o[0] = scale_mask(v, rm);
                o[1] = scale_mask(v, gm);
                o[2] = scale_mask(v, bm);
                o[3] = am ? scale_mask(v, am) : 255;
            }
    } else {
        set_error("unsupported DDS pixel format");
        goto bad;
    }
    free(d);
    return im;
bad:
    image_free(im);
    free(d);
    return NULL;
}

static Image *load_image(const char *path) {
    const char *e = path_ext(path);
    if (!strcasecmp(e, ".png"))
        return load_png(path);
    if (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg"))
        return load_jpeg(path);
    if (!strcasecmp(e, ".gif"))
        return load_gif(path);
    if (!strcasecmp(e, ".tif") || !strcasecmp(e, ".tiff"))
        return load_tiff(path);
    if (!strcasecmp(e, ".webp"))
        return load_webp(path);
    if (!strcasecmp(e, ".bmp"))
        return load_bmp(path);
    if (!strcasecmp(e, ".tga"))
        return load_tga(path);
    if (!strcasecmp(e, ".ppm") || !strcasecmp(e, ".pgm") || !strcasecmp(e, ".pbm"))
        return load_pnm(path);
    if (!strcasecmp(e, ".ico"))
        return load_ico(path);
    if (!strcasecmp(e, ".dds"))
        return load_dds(path);
    set_error("unsupported input extension '%s'", e);
    return NULL;
}

/* ---------- image writers used by --extract ---------- */
static bool save_png(const char *path, const Image *im) {
    png_image p;
    memset(&p, 0, sizeof(p));
    p.version = PNG_IMAGE_VERSION;
    p.width = (png_uint_32)im->w;
    p.height = (png_uint_32)im->h;
    p.format = PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(&p, path, 0, im->rgba, 0, NULL)) {
        set_error("PNG write: %s", p.message);
        return false;
    }
    return true;
}
static bool save_bmp(const char *path, const Image *im) {
    size_t row = ((size_t)im->w * 4 + 3) & ~3u, total = 14 + 40 + row * (size_t)im->h;
    uint8_t *d = xcalloc(total, 1);
    d[0] = 'B';
    d[1] = 'M';
    wr32(d + 2, (uint32_t)total);
    wr32(d + 10, 54);
    wr32(d + 14, 40);
    wr32(d + 18, (uint32_t)im->w);
    wr32(d + 22, (uint32_t)im->h);
    wr16(d + 26, 1);
    wr16(d + 28, 32);
    wr32(d + 34, (uint32_t)(row * (size_t)im->h));
    for (int y = 0; y < im->h; y++) {
        uint8_t *r = d + 54 + (size_t)(im->h - 1 - y) * row;
        for (int x = 0; x < im->w; x++) {
            const uint8_t *s = im->rgba + ((size_t)y * im->w + x) * 4;
            r[x * 4] = s[2];
            r[x * 4 + 1] = s[1];
            r[x * 4 + 2] = s[0];
            r[x * 4 + 3] = s[3];
        }
    }
    bool ok = write_file(path, d, total);
    free(d);
    return ok;
}
static bool save_tga(const char *path, const Image *im) {
    size_t n = 18 + (size_t)im->w * im->h * 4;
    uint8_t *d = xcalloc(n, 1);
    d[2] = 2;
    wr16(d + 12, (uint16_t)im->w);
    wr16(d + 14, (uint16_t)im->h);
    d[16] = 32;
    d[17] = 0x28;
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) {
        d[18 + i * 4] = im->rgba[i * 4 + 2];
        d[19 + i * 4] = im->rgba[i * 4 + 1];
        d[20 + i * 4] = im->rgba[i * 4];
        d[21 + i * 4] = im->rgba[i * 4 + 3];
    }
    bool ok = write_file(path, d, n);
    free(d);
    return ok;
}
static bool save_ppm(const char *path, const Image *im) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_error("cannot create '%s'", path);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    bool ok = true;
    for (size_t i = 0; i < (size_t)im->w * im->h; i++)
        if (fwrite(im->rgba + i * 4, 1, 3, f) != 3) {
            ok = false;
            break;
        }
    if (fclose(f))
        ok = false;
    if (!ok)
        set_error("PPM write failed");
    return ok;
}
static bool save_tiff(const char *path, const Image *im) {
    TIFF *t = TIFFOpen(path, "w");
    if (!t) {
        set_error("TIFF create failed");
        return false;
    }
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, im->w);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, im->h);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(t, TIFFTAG_EXTRASAMPLES, 1, &extra);
    bool ok = true;
    for (int y = 0; y < im->h; y++)
        if (TIFFWriteScanline(t, im->rgba + (size_t)y * im->w * 4, y, 0) < 0) {
            ok = false;
            break;
        }
    TIFFClose(t);
    if (!ok)
        set_error("TIFF write failed");
    return ok;
}
static bool save_webp(const char *path, const Image *im) {
    uint8_t *out = NULL;
    size_t n = WebPEncodeLosslessRGBA(im->rgba, im->w, im->h, im->w * 4, &out);
    if (!n || !out) {
        set_error("WebP encode failed");
        return false;
    }
    bool ok = write_file(path, out, n);
    WebPFree(out);
    return ok;
}
static bool save_image(const char *path, const Image *im) {
    const char *e = path_ext(path);
    if (!strcasecmp(e, ".png"))
        return save_png(path, im);
    if (!strcasecmp(e, ".bmp"))
        return save_bmp(path, im);
    if (!strcasecmp(e, ".tga"))
        return save_tga(path, im);
    if (!strcasecmp(e, ".ppm"))
        return save_ppm(path, im);
    if (!strcasecmp(e, ".tif") || !strcasecmp(e, ".tiff"))
        return save_tiff(path, im);
    if (!strcasecmp(e, ".webp"))
        return save_webp(path, im);
    set_error("unsupported output extension '%s'", e);
    return false;
}

/* ========================================================================== */
/* IMAGE PROCESSING: alpha premultiplication, padding and Lanczos resampling   */
/* ========================================================================== */

static void premultiply_alpha(Image *im) {
    size_t np = (size_t)im->w * im->h;
    for (size_t i = 0; i < np; i++) {
        unsigned a = im->rgba[i * 4 + 3];
        /* Python round() and C round() differ on exact .5 ties.  Integer
           half-up is deterministic and differs only in rare tie cases. */
        im->rgba[i * 4] = (uint8_t)((im->rgba[i * 4] * a + 127) / 255);
        im->rgba[i * 4 + 1] = (uint8_t)((im->rgba[i * 4 + 1] * a + 127) / 255);
        im->rgba[i * 4 + 2] = (uint8_t)((im->rgba[i * 4 + 2] * a + 127) / 255);
    }
}

static Image *pad_canvas(Image *src, int multiple) {
    int nw = align_up(src->w, multiple);
    if (nw == src->w)
        return src;
    Image *out = image_new(nw, src->h);
    if (!out) {
        image_free(src);
        return NULL;
    }
    for (int y = 0; y < src->h; y++)
        memcpy(out->rgba + (size_t)y * nw * 4, src->rgba + (size_t)y * src->w * 4,
               (size_t)src->w * 4);
    image_free(src);
    return out;
}

static double sinc_fn(double x) {
    if (fabs(x) < 1e-12)
        return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}
static double lanczos_kernel(double x) {
    x = fabs(x);
    if (x >= 3.0)
        return 0.0;
    return sinc_fn(x) * sinc_fn(x / 3.0);
}

/* Separable Lanczos-3.  When shrinking, the filter is widened by the inverse
   scale to perform proper low-pass filtering rather than merely sampling. */
static Image *resize_lanczos(Image *src, int nw, int nh) {
    if (nw <= 0 || nh <= 0) {
        set_error("resize dimensions must be positive");
        return NULL;
    }
    if (nw == src->w && nh == src->h) {
        Image *copy = image_new(nw, nh);
        if (copy)
            memcpy(copy->rgba, src->rgba, (size_t)nw * nh * 4);
        return copy;
    }
    size_t tmpn;
    if (!checked_pixels(nw, src->h, 4, &tmpn))
        return NULL;
    double *tmp = xcalloc(tmpn, sizeof(double));
    double sx = (double)nw / src->w;
    double support = sx < 1.0 ? 3.0 / sx : 3.0;
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < nw; x++) {
            double center = ((double)x + 0.5) / sx - 0.5;
            int lo = (int)floor(center - support + 1.0), hi = (int)floor(center + support);
            double sum = 0.0, v[4] = {0, 0, 0, 0};
            for (int q = lo; q <= hi; q++) {
                int xx = clampi(q, 0, src->w - 1);
                double dist = center - q;
                double wt = sx < 1.0 ? lanczos_kernel(dist * sx) * sx : lanczos_kernel(dist);
                sum += wt;
                const uint8_t *p = src->rgba + ((size_t)y * src->w + xx) * 4;
                for (int c = 0; c < 4; c++)
                    v[c] += p[c] * wt;
            }
            if (fabs(sum) < 1e-15)
                sum = 1.0;
            for (int c = 0; c < 4; c++)
                tmp[((size_t)y * nw + x) * 4 + c] = v[c] / sum;
        }
    Image *out = image_new(nw, nh);
    if (!out) {
        free(tmp);
        return NULL;
    }
    double sy = (double)nh / src->h;
    support = sy < 1.0 ? 3.0 / sy : 3.0;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            double center = ((double)y + 0.5) / sy - 0.5;
            int lo = (int)floor(center - support + 1.0), hi = (int)floor(center + support);
            double sum = 0.0, v[4] = {0, 0, 0, 0};
            for (int q = lo; q <= hi; q++) {
                int yy = clampi(q, 0, src->h - 1);
                double dist = center - q;
                double wt = sy < 1.0 ? lanczos_kernel(dist * sy) * sy : lanczos_kernel(dist);
                sum += wt;
                for (int c = 0; c < 4; c++)
                    v[c] += tmp[((size_t)yy * nw + x) * 4 + c] * wt;
            }
            if (fabs(sum) < 1e-15)
                sum = 1.0;
            uint8_t *p = out->rgba + ((size_t)y * nw + x) * 4;
            for (int c = 0; c < 4; c++)
                p[c] = (uint8_t)clampi((int)floor(v[c] / sum + 0.5), 0, 255);
        }
    free(tmp);
    return out;
}

static bool parse_resize(const char *mode, int w, int h, int *nw, int *nh) {
    if (!mode) {
        *nw = w;
        *nh = h;
        return true;
    }
    if (!strcasecmp(mode, "up")) {
        *nw = next_power2(w);
        *nh = next_power2(h);
        return true;
    }
    if (!strcasecmp(mode, "down")) {
        *nw = prev_power2(w);
        *nh = prev_power2(h);
        return true;
    }
    char extra = 0;
    if (sscanf(mode, "%dx%d%c", nw, nh, &extra) == 2 && *nw > 0 && *nh > 0)
        return true;
    set_error("invalid --resize value '%s' (use up | down | WxH)", mode);
    return false;
}

/* ========================================================================== */
/* MEDIAN-CUT PALETTE GENERATION AND FLOYD-STEINBERG INDEX ASSIGNMENT          */
/* ========================================================================== */

/* The histogram stores 5-bit RGB bins.  It avoids pathological memory use on
   photographic sources while preserving the color-space partitioning expected
   from median-cut.  Counts weight both split medians and palette centroids. */
typedef struct {
    uint32_t count;
    uint64_t rsum, gsum, bsum;
    uint8_t r5, g5, b5;
} HistColor;

typedef struct {
    int begin, end;
    uint64_t weight;
    uint8_t rmin, rmax, gmin, gmax, bmin, bmax;
} ColorBox;

static int g_sort_channel = 0;
static int cmp_hist_color(const void *aa, const void *bb) {
    const HistColor *a = aa, *b = bb;
    int av = g_sort_channel == 0 ? a->r5 : g_sort_channel == 1 ? a->g5 : a->b5;
    int bv = g_sort_channel == 0 ? b->r5 : g_sort_channel == 1 ? b->g5 : b->b5;
    if (av != bv)
        return av - bv;
    if (a->r5 != b->r5)
        return a->r5 - b->r5;
    if (a->g5 != b->g5)
        return a->g5 - b->g5;
    return a->b5 - b->b5;
}
static void box_measure(ColorBox *b, HistColor *c) {
    b->weight = 0;
    b->rmin = b->gmin = b->bmin = 31;
    b->rmax = b->gmax = b->bmax = 0;
    for (int i = b->begin; i < b->end; i++) {
        if (c[i].r5 < b->rmin)
            b->rmin = c[i].r5;
        if (c[i].r5 > b->rmax)
            b->rmax = c[i].r5;
        if (c[i].g5 < b->gmin)
            b->gmin = c[i].g5;
        if (c[i].g5 > b->gmax)
            b->gmax = c[i].g5;
        if (c[i].b5 < b->bmin)
            b->bmin = c[i].b5;
        if (c[i].b5 > b->bmax)
            b->bmax = c[i].b5;
        b->weight += c[i].count;
    }
}
static int box_priority(const ColorBox *b) {
    int rr = b->rmax - b->rmin, gg = b->gmax - b->gmin, bb = b->bmax - b->bmin,
        range = rr > gg ? (rr > bb ? rr : bb) : (gg > bb ? gg : bb);
    return range * (int)(b->weight > INT_MAX ? INT_MAX : b->weight);
}

static int build_median_palette(const Image *im, int requested, uint8_t palette[256][3]) {
    typedef struct {
        uint32_t count;
        uint64_t r, g, b;
    } Bin;
    Bin *bins = xcalloc(32768, sizeof(*bins));
    size_t np = (size_t)im->w * im->h;
    for (size_t i = 0; i < np; i++) {
        const uint8_t *p = im->rgba + i * 4;
        unsigned a = p[3];
        /* Match the Python quantization input: RGB composited over black using
           source alpha.  With premultiplication enabled this intentionally
           applies alpha once more, as does the reference pipeline. */
        uint8_t r = (uint8_t)((p[0] * a + 127) / 255), g = (uint8_t)((p[1] * a + 127) / 255),
                b = (uint8_t)((p[2] * a + 127) / 255);
        unsigned key = ((unsigned)(r >> 3) << 10) | ((unsigned)(g >> 3) << 5) | (b >> 3);
        Bin *z = &bins[key];
        z->count++;
        z->r += r;
        z->g += g;
        z->b += b;
    }
    int used = 0;
    for (int i = 0; i < 32768; i++)
        if (bins[i].count)
            used++;
    HistColor *colors = xmalloc((size_t)(used ? used : 1) * sizeof(*colors));
    int ci = 0;
    for (int i = 0; i < 32768; i++)
        if (bins[i].count) {
            colors[ci].count = bins[i].count;
            colors[ci].rsum = bins[i].r;
            colors[ci].gsum = bins[i].g;
            colors[ci].bsum = bins[i].b;
            colors[ci].r5 = (uint8_t)((i >> 10) & 31);
            colors[ci].g5 = (uint8_t)((i >> 5) & 31);
            colors[ci].b5 = (uint8_t)(i & 31);
            ci++;
        }
    free(bins);
    memset(palette, 0, 256 * 3);
    if (!used) {
        free(colors);
        return 1;
    }
    int target = requested < used ? requested : used;
    ColorBox boxes[256];
    boxes[0] = (ColorBox){0, used, 0, 0, 0, 0, 0, 0, 0};
    box_measure(&boxes[0], colors);
    int nb = 1;
    while (nb < target) {
        int pick = -1, best = -1;
        for (int i = 0; i < nb; i++)
            if (boxes[i].end - boxes[i].begin > 1) {
                int p = box_priority(&boxes[i]);
                if (p > best) {
                    best = p;
                    pick = i;
                }
            }
        if (pick < 0)
            break;
        ColorBox old = boxes[pick];
        int rr = old.rmax - old.rmin, gg = old.gmax - old.gmin, bb = old.bmax - old.bmin;
        g_sort_channel = (rr >= gg && rr >= bb) ? 0 : (gg >= bb ? 1 : 2);
        qsort(colors + old.begin, (size_t)(old.end - old.begin), sizeof(*colors), cmp_hist_color);
        uint64_t half = (old.weight + 1) / 2, acc = 0;
        int split = old.begin + 1;
        for (int i = old.begin; i < old.end - 1; i++) {
            acc += colors[i].count;
            if (acc >= half) {
                split = i + 1;
                break;
            }
        }
        boxes[pick] = (ColorBox){old.begin, split, 0, 0, 0, 0, 0, 0, 0};
        boxes[nb] = (ColorBox){split, old.end, 0, 0, 0, 0, 0, 0, 0};
        box_measure(&boxes[pick], colors);
        box_measure(&boxes[nb], colors);
        nb++;
    }
    for (int i = 0; i < nb; i++) {
        uint64_t count = 0, rs = 0, gs = 0, bs = 0;
        for (int j = boxes[i].begin; j < boxes[i].end; j++) {
            count += colors[j].count;
            rs += colors[j].rsum;
            gs += colors[j].gsum;
            bs += colors[j].bsum;
        }
        if (count) {
            palette[i][0] = (uint8_t)((rs + count / 2) / count);
            palette[i][1] = (uint8_t)((gs + count / 2) / count);
            palette[i][2] = (uint8_t)((bs + count / 2) / count);
        }
    }
    free(colors);
    return nb;
}

static int nearest_palette(double r, double g, double b, const uint8_t pal[256][3], int n) {
    int best = 0;
    double bd = 1e100;
    for (int i = 0; i < n; i++) {
        double dr = r - pal[i][0], dg = g - pal[i][1],
               db = b - pal[i][2]; /* Luma-aware metric remains Euclidean enough to preserve
                                      median-cut intent. */
        double d = dr * dr + dg * dg + db * db;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static uint8_t *map_palette(const Image *im, const uint8_t pal[256][3], int n, bool dither,
                            uint64_t asum[256], uint32_t acount[256]) {
    size_t np = (size_t)im->w * im->h;
    uint8_t *idx = xmalloc(np);
    memset(asum, 0, 256 * sizeof(*asum));
    memset(acount, 0, 256 * sizeof(*acount));
    if (!dither) {
        for (size_t i = 0; i < np; i++) {
            const uint8_t *p = im->rgba + i * 4;
            double a = p[3] / 255.0;
            int k = nearest_palette(p[0] * a, p[1] * a, p[2] * a, pal, n);
            idx[i] = (uint8_t)k;
            asum[k] += p[3];
            acount[k]++;
        }
        return idx;
    }
    int w = im->w;
    double *er0 = xcalloc((size_t)(w + 2) * 3, sizeof(double)),
           *er1 = xcalloc((size_t)(w + 2) * 3, sizeof(double));
    for (int y = 0; y < im->h; y++) {
        memset(er1, 0, (size_t)(w + 2) * 3 * sizeof(double));
        bool lr = (y & 1) == 0;
        int start = lr ? 0 : w - 1, end = lr ? w : -1, step = lr ? 1 : -1;
        for (int x = start; x != end; x += step) {
            const uint8_t *p = im->rgba + ((size_t)y * w + x) * 4;
            double a = p[3] / 255.0;
            double r = clampd(p[0] * a + er0[(x + 1) * 3], 0, 255),
                   g = clampd(p[1] * a + er0[(x + 1) * 3 + 1], 0, 255),
                   b = clampd(p[2] * a + er0[(x + 1) * 3 + 2], 0, 255);
            int k = nearest_palette(r, g, b, pal, n);
            idx[(size_t)y * w + x] = (uint8_t)k;
            asum[k] += p[3];
            acount[k]++;
            double e[3] = {r - pal[k][0], g - pal[k][1], b - pal[k][2]};
            int d = lr ? 1 : -1, f = x + 1 + d, back = x + 1 - d;
            for (int c = 0; c < 3; c++) {
                er0[f * 3 + c] += e[c] * 7 / 16;
                er1[back * 3 + c] += e[c] * 3 / 16;
                er1[(x + 1) * 3 + c] += e[c] * 5 / 16;
                er1[f * 3 + c] += e[c] * 1 / 16;
            }
        }
        double *t = er0;
        er0 = er1;
        er1 = t;
    }
    free(er0);
    free(er1);
    return idx;
}

/* Direct 16-bit channel dithering, matching the serpentine diffusion geometry
   of the Python implementation.  Output channels hold 5-bit values. */
static uint8_t *dither_direct_5bit(const Image *im) {
    int w = im->w;
    size_t np = (size_t)w * im->h;
    uint8_t *out = xmalloc(np * 4);
    double *cur = xcalloc((size_t)(w + 2) * 3, sizeof(double)),
           *next = xcalloc((size_t)(w + 2) * 3, sizeof(double));
    const double qstep = 255.0 / 31.0;
    for (int y = 0; y < im->h; y++) {
        memset(next, 0, (size_t)(w + 2) * 3 * sizeof(double));
        bool lr = (y & 1) == 0;
        int first = lr ? 0 : w - 1, last = lr ? w : -1, dx = lr ? 1 : -1;
        for (int x = first; x != last; x += dx) {
            const uint8_t *p = im->rgba + ((size_t)y * w + x) * 4;
            uint8_t *o = out + ((size_t)y * w + x) * 4;
            double adj[3];
            int q[3];
            for (int c = 0; c < 3; c++) {
                adj[c] = p[c] + cur[(x + 1) * 3 + c];
                q[c] = clampi((int)floor(adj[c] / qstep + 0.5), 0, 31);
                o[c] = (uint8_t)q[c];
            }
            o[3] = p[3];
            int f = x + 1 + dx, back = x + 1 - dx;
            for (int c = 0; c < 3; c++) {
                double e = adj[c] - q[c] * qstep;
                cur[f * 3 + c] += e * 7 / 16;
                next[back * 3 + c] += e * 3 / 16;
                next[(x + 1) * 3 + c] += e * 5 / 16;
                next[f * 3 + c] += e / 16;
            }
        }
        double *t = cur;
        cur = next;
        next = t;
    }
    free(cur);
    free(next);
    return out;
}

/* ========================================================================== */
/* PS1 COLOR WORDS AND TIM BUILDING                                            */
/* ========================================================================== */

static uint16_t pack_5551(int r5, int g5, int b5, int a) {
    if (a <= 0)
        return 0;
    uint16_t v =
        (uint16_t)(((a >= 128) ? 0x8000 : 0) | ((b5 & 31) << 10) | ((g5 & 31) << 5) | (r5 & 31));
    return v ? v : 1;
}
static uint16_t color_to_5551(int r, int g, int b, int a) {
    if (a <= 0)
        return 0;
    int r5 = clampi((r * 31 + 127) / 255, 0, 31), g5 = clampi((g * 31 + 127) / 255, 0, 31),
        b5 = clampi((b * 31 + 127) / 255, 0, 31);
    return pack_5551(r5, g5, b5, a);
}
static void color_from_5551(uint16_t v, uint8_t out[4]) {
    if (!v) {
        memset(out, 0, 4);
        return;
    }
    int r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 3) | (g >> 2));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
    out[3] = 255;
}
static unsigned compute_tpage(int x, int y, int pmode, int abr) {
    int tp = pmode == 0 ? 0 : pmode == 1 ? 1 : 2;
    return ((x / 64) & 15) | (((y / 256) & 1) << 4) | ((abr & 3) << 5) | ((tp & 3) << 7);
}
static unsigned compute_clut_id(int x, int y) {
    return ((x / 16) & 63) | ((y & 511) << 6);
}
static void warn_vram(int x, int y, int w, int h, const char *label) {
    if (x < 0 || y < 0 || x + w > 1024 || y + h > 512)
        printf(
            "  WARNING: %s position (%d,%d) with size %dx%d exceeds PS1 VRAM bounds (1024x512).\n",
            label, x, y, w, h);
}
static void warn_dimensions(const char *name, int w, int h) {
    if (!is_power2(w) || !is_power2(h))
        printf("  NOTE: '%s' (%dx%d) is not a power of 2. Not mandatory on PS1, but recommended "
               "(suggestion: %dx%d or %dx%d).\n",
               name, w, h, prev_power2(w), prev_power2(h), next_power2(w), next_power2(h));
}

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buffer;
static void buf_reserve(Buffer *b, size_t add) {
    if (add > SIZE_MAX - b->n) {
        fprintf(stderr, "fatal: buffer overflow\n");
        exit(2);
    }
    size_t need = b->n + add;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        b->p = xrealloc(b->p, cap);
        b->cap = cap;
    }
}
static void buf_put(Buffer *b, const void *p, size_t n) {
    buf_reserve(b, n);
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
static void buf_u16(Buffer *b, uint16_t v) {
    uint8_t z[2];
    wr16(z, v);
    buf_put(b, z, 2);
}
static void buf_u32(Buffer *b, uint32_t v) {
    uint8_t z[4];
    wr32(z, v);
    buf_put(b, z, 4);
}

static bool build_tim(Image *im, int pmode, bool premult, bool dither, int vx, int vy, int cx,
                      int cy, int palettes, Buffer *out) {
    if (premult && pmode != PMODE_24BIT)
        premultiply_alpha(im);
    int mult = pmode == 0 ? 4 : pmode == 1 ? 2 : pmode == 3 ? 2 : 1;
    if (mult > 1) {
        im = pad_canvas(im, mult);
        if (!im)
            return false;
    }
    int w = im->w, h = im->h;
    int width_units = pmode == 0 ? w / 4 : pmode == 1 ? w / 2 : pmode == 2 ? w : w * 3 / 2;
    size_t bytes_per_row = pmode == 0   ? (size_t)w / 2
                           : pmode == 1 ? (size_t)w
                           : pmode == 2 ? (size_t)w * 2
                                        : (size_t)w * 3;
    if (h > UINT16_MAX || width_units > UINT16_MAX || bytes_per_row > UINT32_MAX - 12 ||
        (size_t)h > (UINT32_MAX - 12) / bytes_per_row) {
        set_error("image dimensions %dx%d exceed TIM section/header limits", w, h);
        image_free(im);
        return false;
    }
    if (vx < INT16_MIN || vx > INT16_MAX || vy < INT16_MIN || vy > INT16_MAX || cx < INT16_MIN ||
        cx > INT16_MAX || cy < INT16_MIN || cy > INT16_MAX) {
        set_error("VRAM/CLUT coordinates must fit signed 16-bit TIM fields");
        image_free(im);
        return false;
    }
    if (palettes < 1 || palettes > UINT16_MAX ||
        (pmode < 2 &&
         (uint64_t)(pmode == 0 ? 16 : 256) * (uint64_t)palettes * 2 + 12 > UINT32_MAX)) {
        set_error("palette count %d exceeds TIM header/section limits", palettes);
        image_free(im);
        return false;
    }
    Buffer cl = {0}, pix = {0};
    if (pmode == PMODE_4BIT || pmode == PMODE_8BIT) {
        int nc = pmode == 0 ? 16 : 256;
        uint8_t pal[256][3];
        int actual = build_median_palette(im, nc, pal);
        uint64_t asum[256];
        uint32_t acount[256];
        uint8_t *idx = map_palette(im, pal, actual, dither, asum, acount);
        size_t pn = pmode == 0 ? (size_t)w * h / 2 : (size_t)w * h;
        buf_reserve(&pix, pn);
        pix.n = pn;
        if (pmode == 0)
            for (size_t i = 0; i < (size_t)w * h; i += 2)
                pix.p[i / 2] = (uint8_t)((idx[i] & 15) | ((idx[i + 1] & 15) << 4));
        else
            memcpy(pix.p, idx, pn);
        if (palettes < 1)
            palettes = 1;
        uint32_t clen = 12u + (uint32_t)nc * (uint32_t)palettes * 2u;
        buf_u32(&cl, clen);
        buf_u16(&cl, (uint16_t)cx);
        buf_u16(&cl, (uint16_t)cy);
        buf_u16(&cl, (uint16_t)nc);
        buf_u16(&cl, (uint16_t)palettes);
        for (int row = 0; row < palettes; row++)
            for (int i = 0; i < nc; i++) {
                int a = acount[i] ? (int)llrint((double)asum[i] / acount[i]) : 255;
                buf_u16(&cl, color_to_5551(pal[i][0], pal[i][1], pal[i][2], a));
            }
        free(idx);
        warn_vram(cx, cy, nc, palettes, "CLUT");
        warn_vram(vx, vy, pmode == 0 ? w / 4 : w / 2, h, "Image");
        printf("  TPAGE = 0x%03X   CLUT ID = 0x%04X\n", compute_tpage(vx, vy, pmode, 0),
               compute_clut_id(cx, cy));
    } else if (pmode == PMODE_16BIT) {
        size_t np = (size_t)w * h;
        buf_reserve(&pix, np * 2);
        pix.n = np * 2;
        if (dither) {
            uint8_t *q = dither_direct_5bit(im);
            for (size_t i = 0; i < np; i++)
                wr16(pix.p + i * 2, pack_5551(q[i * 4], q[i * 4 + 1], q[i * 4 + 2], q[i * 4 + 3]));
            free(q);
        } else
            for (size_t i = 0; i < np; i++) {
                uint8_t *p = im->rgba + i * 4;
                wr16(pix.p + i * 2, color_to_5551(p[0], p[1], p[2], p[3]));
            }
        warn_vram(vx, vy, w, h, "Image");
        printf("  TPAGE = 0x%03X   (no CLUT for this format)\n", compute_tpage(vx, vy, pmode, 0));
    } else {
        size_t np = (size_t)w * h;
        buf_reserve(&pix, np * 3);
        pix.n = np * 3;
        for (size_t i = 0; i < np; i++)
            memcpy(pix.p + i * 3, im->rgba + i * 4, 3);
        warn_vram(vx, vy, w * 3 / 2, h, "Image");
        printf("  TPAGE = 0x%03X   (no CLUT for this format)\n", compute_tpage(vx, vy, pmode, 0));
    }
    buf_u32(out, TIM_ID);
    buf_u32(out, (uint32_t)pmode | (cl.n ? CF_CLUT_PRESENT : 0));
    if (cl.n)
        buf_put(out, cl.p, cl.n);
    buf_u32(out, (uint32_t)(12 + pix.n));
    buf_u16(out, (uint16_t)vx);
    buf_u16(out, (uint16_t)vy);
    buf_u16(out, (uint16_t)width_units);
    buf_u16(out, (uint16_t)h);
    buf_put(out, pix.p, pix.n);
    free(cl.p);
    free(pix.p);
    image_free(im);
    return true;
}

/* ========================================================================== */
/* SAFE TIM PARSER, INFO, VERIFY, EXTRACTION                                   */
/* ========================================================================== */

typedef struct {
    int pmode;
    bool has_clut;
    uint32_t clut_len;
    int16_t cx, cy;
    uint16_t cw, ch;
    const uint8_t *colors;
    uint32_t img_len;
    int16_t ix, iy;
    uint16_t iw, ih;
    const uint8_t *pixels;
    size_t pixel_len;
    size_t parsed_end, file_len;
} TimInfo;
static bool parse_tim_mem(const uint8_t *d, size_t n, TimInfo *t) {
    memset(t, 0, sizeof(*t));
    t->file_len = n;
    if (n < 8) {
        set_error("file is too small to be a valid TIM");
        return false;
    }
    if (rd32(d) != TIM_ID) {
        set_error("not a valid TIM (ID 0x%08X)", rd32(d));
        return false;
    }
    uint32_t flag = rd32(d + 4);
    t->pmode = flag & 3;
    t->has_clut = !!(flag & 8);
    size_t off = 8;
    if (t->has_clut) {
        if (n - off < 12) {
            set_error("truncated CLUT header");
            return false;
        }
        t->clut_len = rd32(d + off);
        t->cx = (int16_t)rd16(d + off + 4);
        t->cy = (int16_t)rd16(d + off + 6);
        t->cw = rd16(d + off + 8);
        t->ch = rd16(d + off + 10);
        if (t->clut_len < 12 || t->clut_len > n - off) {
            set_error("invalid CLUT section length %u", t->clut_len);
            return false;
        }
        size_t need = (size_t)t->cw * t->ch * 2;
        if (need > t->clut_len - 12) {
            set_error("CLUT dimensions exceed section");
            return false;
        }
        t->colors = d + off + 12;
        off += t->clut_len;
    }
    if (n - off < 12) {
        set_error("truncated image header");
        return false;
    }
    t->img_len = rd32(d + off);
    t->ix = (int16_t)rd16(d + off + 4);
    t->iy = (int16_t)rd16(d + off + 6);
    t->iw = rd16(d + off + 8);
    t->ih = rd16(d + off + 10);
    if (t->img_len < 12 || t->img_len > n - off) {
        set_error("invalid image section length %u", t->img_len);
        return false;
    }
    t->pixels = d + off + 12;
    t->pixel_len = t->img_len - 12;
    t->parsed_end = off + t->img_len;
    return true;
}
static int pixel_width(const TimInfo *t) {
    return t->pmode == 0   ? t->iw * 4
           : t->pmode == 1 ? t->iw * 2
           : t->pmode == 2 ? t->iw
                           : (t->iw * 2) / 3;
}
static const char *pmode_name(int p) {
    static const char *n[] = {"4-bit Indexed (16 colors, CLUT)", "8-bit Indexed (256 colors, CLUT)",
                              "16-bit Direct (RGBA5551, 1-bit STP)",
                              "24-bit Direct (RGB888, no transparency)"};
    return p >= 0 && p < 4 ? n[p] : "unknown";
}

static bool tim_info_file(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return false;
    TimInfo t;
    if (!parse_tim_mem(d, n, &t)) {
        free(d);
        return false;
    }
    int w = pixel_width(&t);
    printf("\n----------------------------------------------------\n  File        :  %s\n  File "
           "size   :  %.2f KB  (%zu bytes)\n----------------------------------------------------\n",
           path_base(path), n / 1024.0, n);
    printf("  Pixel mode  :  %s\n  Dimensions  :  %d x %u "
           "px\n----------------------------------------------------\n",
           pmode_name(t.pmode), w, t.ih);
    warn_dimensions(path_base(path), w, t.ih);
    if (t.has_clut)
        printf("  CLUT present:  yes\n  CLUT colors :  %u per palette x %u palette(s) = %u total\n "
               " CLUT size   :  %u bytes\n  CLUT X/Y    :  (%d, %d)\n  CLUT ID     :  "
               "0x%04X\n----------------------------------------------------\n",
               t.cw, t.ch, t.cw * t.ch, t.cw * t.ch * 2, t.cx, t.cy, compute_clut_id(t.cx, t.cy));
    else
        printf("  CLUT present:  no\n----------------------------------------------------\n");
    printf("  Image data  :  %zu bytes  (declared %u)\n  Image X/Y   :  (%d, %d)\n  TPAGE       :  "
           "0x%03X\n----------------------------------------------------\n",
           t.pixel_len, t.img_len - 12, t.ix, t.iy, compute_tpage(t.ix, t.iy, t.pmode, 0));
    free(d);
    return true;
}

static bool verify_tim_file(const char *path) {
    printf("Verifying: %s ... ", path_base(path));
    fflush(stdout);
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n)) {
        printf("FAILED (%s)\n", g_error);
        return false;
    }
    TimInfo t;
    if (!parse_tim_mem(d, n, &t)) {
        printf("FAILED (%s)\n", g_error);
        free(d);
        return false;
    }
    char problems[4096] = "";
    size_t plen = 0;
#define PROBLEM(...)                                                                               \
    do {                                                                                           \
        if (plen < sizeof(problems)) {                                                             \
            int z = snprintf(problems + plen, sizeof(problems) - plen, "    - " __VA_ARGS__);      \
            if (z > 0)                                                                             \
                plen += (size_t)z < sizeof(problems) - plen ? (size_t)z : sizeof(problems) - plen; \
        }                                                                                          \
    } while (0)
    if (t.parsed_end != n)
        PROBLEM("Trailing data: parsed %zu of %zu bytes\n", t.parsed_end, n);
    if (t.has_clut) {
        if (t.clut_len != 12u + (uint32_t)t.cw * t.ch * 2u)
            PROBLEM("Declared CLUT section length does not match dimensions\n");
        int want = t.pmode == 0 ? 16 : t.pmode == 1 ? 256 : 0;
        if (want && t.cw != want)
            PROBLEM("Colors per palette (%u) unexpected (expected %d)\n", t.cw, want);
    } else if (t.pmode < 2)
        PROBLEM("Indexed format has no CLUT\n");
    int w = pixel_width(&t), mult = t.pmode == 0 ? 4 : t.pmode == 1 ? 2 : t.pmode == 3 ? 2 : 1;
    if (w % mult)
        PROBLEM("Width is incompatible with required alignment (%d)\n", mult);
    size_t expected = t.pmode == 0   ? (size_t)w * t.ih / 2
                      : t.pmode == 1 ? (size_t)w * t.ih
                      : t.pmode == 2 ? (size_t)w * t.ih * 2
                                     : (size_t)w * t.ih * 3;
    if (t.pixel_len != expected)
        PROBLEM("Image data size (%zu) does not match expected (%zu)\n", t.pixel_len, expected);
#undef PROBLEM
    bool ok = plen == 0;
    printf(ok ? "OK\n" : "FAILED\n%s", problems);
    free(d);
    return ok;
}

static Image *decode_tim_image(const TimInfo *t, int palette_index) {
    int w = pixel_width(t), h = t->ih;
    Image *im = image_new(w, h);
    if (!im)
        return NULL;
    size_t np = (size_t)w * h;
    if (t->pmode < 2) {
        if (!t->has_clut || !t->cw || !t->ch) {
            set_error("indexed TIM has no usable CLUT");
            image_free(im);
            return NULL;
        }
        if (palette_index < 0 || palette_index >= t->ch) {
            printf("  NOTE: this file only contains %u palette(s), palette 0 will be used instead "
                   "of %d\n",
                   t->ch, palette_index);
            palette_index = 0;
        }
        for (size_t i = 0; i < np; i++) {
            unsigned idx = t->pmode == 0 ? ((i & 1) ? t->pixels[i / 2] >> 4 : t->pixels[i / 2] & 15)
                                         : t->pixels[i];
            if (idx >= t->cw)
                idx = 0;
            uint16_t v = rd16(t->colors + ((size_t)palette_index * t->cw + idx) * 2);
            color_from_5551(v, im->rgba + i * 4);
        }
    } else if (t->pmode == 2)
        for (size_t i = 0; i < np; i++)
            color_from_5551(rd16(t->pixels + i * 2), im->rgba + i * 4);
    else
        for (size_t i = 0; i < np; i++) {
            memcpy(im->rgba + i * 4, t->pixels + i * 3, 3);
            im->rgba[i * 4 + 3] = 255;
        }
    return im;
}
static bool extract_tim(const char *path, const char *ext, int pal, char **outpath) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return false;
    TimInfo t;
    if (!parse_tim_mem(d, n, &t)) {
        free(d);
        return false;
    }
    Image *im = decode_tim_image(&t, pal);
    free(d);
    if (!im)
        return false;
    char *dst = replace_ext(path, ext);
    bool ok = save_image(dst, im);
    image_free(im);
    if (ok)
        *outpath = dst;
    else
        free(dst);
    return ok;
}

/* ========================================================================== */
/* STRUCTURAL DIFF                                                            */
/* ========================================================================== */
typedef enum { DIFF_SAFE, DIFF_WARNING, DIFF_UNSAFE } DiffLevel;
static const char *level_name(DiffLevel l) {
    return l == DIFF_SAFE ? "SAFE" : l == DIFF_WARNING ? "WARNING" : "UNSAFE";
}
static DiffLevel diff_files(const char *a, const char *b, bool header) {
    if (header)
        printf("\n--------------------------------------------------------\n  Comparing:\n    "
               "original: %s\n    modified: "
               "%s\n--------------------------------------------------------\n",
               path_base(a), path_base(b));
    uint8_t *d1, *d2;
    size_t n1, n2;
    if (!read_file(a, &d1, &n1)) {
        printf("  UNSAFE - %s\n", g_error);
        return DIFF_UNSAFE;
    }
    if (!read_file(b, &d2, &n2)) {
        printf("  UNSAFE - %s\n", g_error);
        free(d1);
        return DIFF_UNSAFE;
    }
    TimInfo x, y;
    if (!parse_tim_mem(d1, n1, &x)) {
        printf("  UNSAFE - %s\n", g_error);
        free(d1);
        free(d2);
        return DIFF_UNSAFE;
    }
    if (!parse_tim_mem(d2, n2, &y)) {
        printf("  UNSAFE - %s\n", g_error);
        free(d1);
        free(d2);
        return DIFF_UNSAFE;
    }
    DiffLevel l = DIFF_SAFE;
    if (x.pmode != y.pmode) {
        printf("  - Format changed: %s -> %s\n", pmode_name(x.pmode), pmode_name(y.pmode));
        l = DIFF_UNSAFE;
    }
    int wx = pixel_width(&x), wy = pixel_width(&y);
    if (wx != wy || x.ih != y.ih) {
        printf("  - Dimensions changed: %dx%u -> %dx%u\n", wx, x.ih, wy, y.ih);
        if (l == DIFF_SAFE)
            l = DIFF_WARNING;
    }
    if (x.has_clut != y.has_clut) {
        printf("  - CLUT presence changed\n");
        l = DIFF_UNSAFE;
    }
    if (n1 != n2)
        printf("  - File size differs: %zu bytes -> %zu bytes\n", n1, n2);
    size_t m = x.pixel_len < y.pixel_len ? x.pixel_len : y.pixel_len;
    if (m) {
        size_t dif = 0;
        for (size_t i = 0; i < m; i++)
            dif += x.pixels[i] != y.pixels[i];
        double ratio = dif * 100.0 / m;
        printf("  - Pixel data difference: %.2f%%\n", ratio);
        if (l == DIFF_SAFE && ratio > 30)
            l = DIFF_WARNING;
    }
    printf("  => %s\n", level_name(l));
    free(d1);
    free(d2);
    return l;
}

typedef struct {
    char **v;
    size_t n, cap;
} StrVec;
static void sv_add(StrVec *s, const char *x) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = xrealloc(s->v, s->cap * sizeof(*s->v));
    }
    s->v[s->n++] = xstrdup(x);
}
static int cmp_strp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static void sv_free(StrVec *s) {
    for (size_t i = 0; i < s->n; i++)
        free(s->v[i]);
    free(s->v);
}
static StrVec list_tim_names(const char *dir) {
    StrVec s = {0};
    DIR *d = opendir(dir);
    if (!d)
        return s;
    struct dirent *e;
    while ((e = readdir(d)))
        if (!strcasecmp(path_ext(e->d_name), ".tim"))
            sv_add(&s, e->d_name);
    closedir(d);
    qsort(s.v, s.n, sizeof(*s.v), cmp_strp);
    return s;
}
static char *join_path2(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *r = xmalloc(na + 1 + nb + 1);
    memcpy(r, a, na);
    if (na && a[na - 1] != '/')
        r[na++] = '/';
    memcpy(r + na, b, nb + 1);
    return r;
}
static void diff_folders(const char *a, const char *b) {
    StrVec x = list_tim_names(a), y = list_tim_names(b);
    int cnt = 0, safe = 0, warn = 0, bad = 0;
    for (size_t i = 0, j = 0; i < x.n && j < y.n;) {
        int c = strcmp(x.v[i], y.v[j]);
        if (c < 0)
            i++;
        else if (c > 0)
            j++;
        else {
            char *p = join_path2(a, x.v[i]), *q = join_path2(b, y.v[j]);
            DiffLevel l = diff_files(p, q, true);
            cnt++;
            safe += l == DIFF_SAFE;
            warn += l == DIFF_WARNING;
            bad += l == DIFF_UNSAFE;
            free(p);
            free(q);
            i++;
            j++;
        }
    }
    if (!cnt)
        printf("No common .tim files between the two folders\n");
    else
        printf("\nSummary: %d compared | SAFE %d | WARNING %d | UNSAFE %d\n", cnt, safe, warn, bad);
    sv_free(&x);
    sv_free(&y);
}

/* ========================================================================== */
/* CONVERSION DRIVER AND COMPLETE COMMAND-LINE INTERFACE                      */
/* ========================================================================== */

#define PS1_TOOL_VERSION "1.0.0"

typedef struct {
    StrVec images, original, modified;
    const char *format, *output, *output_dir, *resize, *extract, *diff_list, *list;
    bool dither, no_premult, info, verify, diff, list_formats, help, version;
    int vram_x, vram_y, clut_x, clut_y, palette_count, palette_index;
} Options;

static void usage(FILE *f, const char *prog) {
    fprintf(f,
            "PS1 TIM Converter - native C edition\n"
            "Usage:\n"
            "  %s <image(s)> --format <4bit|8bit|16bit|24bit> [options]\n"
            "  %s <file.tim> --info | --verify | --extract png\n"
            "  %s --list convert.txt [options]\n"
            "  %s --diff --original a.tim --modified b.tim\n\n"
            "Options:\n"
            "  --format FMT          conversion format: 4bit, 8bit, 16bit, 24bit\n"
            "  --output FILE         output name for one input\n"
            "  --output-dir DIR      output directory for multiple/batch inputs\n"
            "  --resize MODE         up, down, or WxH (Lanczos-3)\n"
            "  --dither              Floyd-Steinberg dithering\n"
            "  --no-premult          disable alpha premultiplication\n"
            "  --vram-x N            image VRAM X position (default 0)\n"
            "  --vram-y N            image VRAM Y position (default 0)\n"
            "  --clut-x N            CLUT VRAM X position (default 0)\n"
            "  --clut-y N            CLUT VRAM Y position (default 0)\n"
            "  --palette-count N     repeated palettes for indexed TIM (default 1)\n"
            "  --info                 display TIM metadata\n"
            "  --verify               structurally verify TIM files\n"
            "  --extract EXT          extract TIM to png/bmp/tga/tiff/webp/ppm\n"
            "  --palette-index N     palette row selected during extraction\n"
            "  --diff                 compare TIM files or folders\n"
            "  --original PATH...    originals for --diff\n"
            "  --modified PATH...    modified files for --diff\n"
            "  --diff-list FILE      whitespace-separated original/modified pairs\n"
            "  --list FILE           batch list: <filename> <format> per line\n"
            "  --list-formats        show formats and commands\n"
            "  --version             show version\n"
            "  -h, --help            show this help\n",
            prog, prog, prog, prog);
}
static void list_formats(void) {
    puts("============================================================\n"
         "  PS1 TIM Converter - Supported formats\n"
         "============================================================\n"
         "  Input: PNG, JPEG, BMP, TGA, TIFF, WebP, GIF, PPM/PGM/PBM, ICO, DDS\n"
         "  4bit    4-bit CLUT   up to 16 colors\n"
         "  8bit    8-bit CLUT   up to 256 colors\n"
         "  16bit   16-bit Direct RGBA5551\n"
         "  24bit   24-bit Direct RGB888 (no transparency)\n"
         "  Extract: PNG, BMP, TGA, TIFF, WebP, PPM\n"
         "============================================================\n"
         "  Median-cut and Floyd-Steinberg are internal; no external process.\n"
         "============================================================");
}
static bool parse_int_arg(const char *name, const char *s, int *out) {
    char *e;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (errno || *e || v < INT_MIN || v > INT_MAX) {
        fprintf(stderr, "ERROR: %s expects an integer, got '%s'\n", name, s);
        return false;
    }
    *out = (int)v;
    return true;
}
static bool option_takes_value(const char *s) {
    return !strcmp(s, "--format") || !strcmp(s, "--output") || !strcmp(s, "--output-dir") ||
           !strcmp(s, "--resize") || !strcmp(s, "--extract") || !strcmp(s, "--diff-list") ||
           !strcmp(s, "--list") || !strcmp(s, "--vram-x") || !strcmp(s, "--vram-y") ||
           !strcmp(s, "--clut-x") || !strcmp(s, "--clut-y") || !strcmp(s, "--palette-count") ||
           !strcmp(s, "--palette-index");
}
static bool parse_options(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof(*o));
    o->palette_count = 1;
    enum { NORMAL, ORIGINAL, MODIFIED } mode = NORMAL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--original")) {
            mode = ORIGINAL;
            continue;
        }
        if (!strcmp(a, "--modified")) {
            mode = MODIFIED;
            continue;
        }
        if (a[0] == '-') {
            mode = NORMAL;
            if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
                o->help = true;
                continue;
            }
            if (!strcmp(a, "--version")) {
                o->version = true;
                continue;
            }
            if (!strcmp(a, "--dither")) {
                o->dither = true;
                continue;
            }
            if (!strcmp(a, "--no-premult")) {
                o->no_premult = true;
                continue;
            }
            if (!strcmp(a, "--info")) {
                o->info = true;
                continue;
            }
            if (!strcmp(a, "--verify")) {
                o->verify = true;
                continue;
            }
            if (!strcmp(a, "--diff")) {
                o->diff = true;
                continue;
            }
            if (!strcmp(a, "--list-formats")) {
                o->list_formats = true;
                continue;
            }
            if (option_takes_value(a)) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "ERROR: %s requires a value\n", a);
                    return false;
                }
                const char *v = argv[++i];
                if (!strcmp(a, "--format"))
                    o->format = v;
                else if (!strcmp(a, "--output"))
                    o->output = v;
                else if (!strcmp(a, "--output-dir"))
                    o->output_dir = v;
                else if (!strcmp(a, "--resize"))
                    o->resize = v;
                else if (!strcmp(a, "--extract"))
                    o->extract = v;
                else if (!strcmp(a, "--diff-list"))
                    o->diff_list = v;
                else if (!strcmp(a, "--list"))
                    o->list = v;
                else if (!strcmp(a, "--vram-x")) {
                    if (!parse_int_arg(a, v, &o->vram_x))
                        return false;
                } else if (!strcmp(a, "--vram-y")) {
                    if (!parse_int_arg(a, v, &o->vram_y))
                        return false;
                } else if (!strcmp(a, "--clut-x")) {
                    if (!parse_int_arg(a, v, &o->clut_x))
                        return false;
                } else if (!strcmp(a, "--clut-y")) {
                    if (!parse_int_arg(a, v, &o->clut_y))
                        return false;
                } else if (!strcmp(a, "--palette-count")) {
                    if (!parse_int_arg(a, v, &o->palette_count))
                        return false;
                } else if (!strcmp(a, "--palette-index")) {
                    if (!parse_int_arg(a, v, &o->palette_index))
                        return false;
                }
                continue;
            }
            fprintf(stderr, "ERROR: unknown option '%s'\n", a);
            return false;
        }
        if (mode == ORIGINAL)
            sv_add(&o->original, a);
        else if (mode == MODIFIED)
            sv_add(&o->modified, a);
        else
            sv_add(&o->images, a);
    }
    return true;
}
static int fmt_number(const char *s) {
    if (!s)
        return -1;
    if (!strcasecmp(s, "4bit"))
        return 0;
    if (!strcasecmp(s, "8bit"))
        return 1;
    if (!strcasecmp(s, "16bit"))
        return 2;
    if (!strcasecmp(s, "24bit"))
        return 3;
    return -1;
}

static char *convert_one(const char *src, const char *fmt, const Options *o,
                         const char *out_override) {
    int pm = fmt_number(fmt);
    if (pm < 0) {
        set_error("unknown format '%s'", fmt ? fmt : "");
        return NULL;
    }
    Image *im = load_image(src);
    if (!im)
        return NULL;
    int original_w = im->w, original_h = im->h, nw, nh;
    if (!parse_resize(o->resize, im->w, im->h, &nw, &nh)) {
        image_free(im);
        return NULL;
    }
    if (nw != im->w || nh != im->h) {
        Image *r = resize_lanczos(im, nw, nh);
        image_free(im);
        if (!r)
            return NULL;
        im = r;
    }
    printf("  Converting: %s  ->  [%s] ...\n", path_base(src), fmt);
    Buffer tim = {0};
    if (!build_tim(im, pm, !o->no_premult, o->dither, o->vram_x, o->vram_y, o->clut_x, o->clut_y,
                   o->palette_count, &tim))
        return NULL;
    char *dst;
    if (out_override)
        dst = xstrdup(out_override);
    else if (o->output_dir)
        dst = join_output(o->output_dir, src, ".tim");
    else
        dst = replace_ext(src, ".tim");
    if (!write_file(dst, tim.p, tim.n)) {
        free(tim.p);
        free(dst);
        return NULL;
    }
    printf("done  (%.1f KB)  ->  %s\n", tim.n / 1024.0, path_base(dst));
    warn_dimensions(path_base(src), o->resize ? nw : original_w, o->resize ? nh : original_h);
    free(tim.p);
    return dst;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}
static bool split_two(char *line, char **a, char **b) {
    char *p = trim(line);
    if (!*p || *p == '#')
        return false;
    *a = p;
    while (*p && !isspace((unsigned char)*p))
        p++;
    if (!*p) {
        *b = NULL;
        return true;
    }
    *p++ = 0;
    p = trim(p);
    *b = p;
    while (*p && !isspace((unsigned char)*p))
        p++;
    *p = 0;
    return true;
}
static int run_batch(const Options *o) {
    FILE *f = fopen(o->list, "r");
    if (!f) {
        fprintf(stderr, "ERROR reading list: %s\n", strerror(errno));
        return 1;
    }
    if (o->output_dir && !mkdir_p(o->output_dir)) {
        fprintf(stderr, "ERROR: %s\n", g_error);
        fclose(f);
        return 1;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t z;
    int lineno = 0, ok = 0, fail = 0;
    printf("Reading list: %s\n\n", path_base(o->list));
    while ((z = getline(&line, &cap, f)) >= 0) {
        (void)z;
        lineno++;
        char *a, *b;
        if (!split_two(line, &a, &b))
            continue;
        if (!b || fmt_number(b) < 0) {
            printf("  FAILED (line %d): expected <filename> <format>\n", lineno);
            fail++;
            continue;
        }
        char *out = convert_one(a, b, o, NULL);
        if (out) {
            ok++;
            free(out);
        } else {
            printf("  FAILED (line %d): %s (%s)\n", lineno, a, g_error);
            fail++;
        }
    }
    free(line);
    fclose(f);
    printf("\nDone: %d succeeded, %d failed\n", ok, fail);
    return fail ? 1 : 0;
}
static void diff_summary(int n, int safe, int warn, int bad) {
    if (n > 1)
        printf("\nSummary: %d compared | SAFE %d | WARNING %d | UNSAFE %d\n", n, safe, warn, bad);
}
static int run_diff(const Options *o) {
    if (o->original.n == 1 && o->modified.n == 1 && is_dir(o->original.v[0]) &&
        is_dir(o->modified.v[0])) {
        diff_folders(o->original.v[0], o->modified.v[0]);
        return 0;
    }
    if ((o->original.n || o->modified.n) && o->original.n != o->modified.n) {
        fprintf(stderr, "ERROR: --original and --modified must contain the same number of files\n");
        return 1;
    }
    int n = 0, safe = 0, warn = 0, bad = 0;
    for (size_t i = 0; i < o->original.n; i++) {
        DiffLevel l = diff_files(o->original.v[i], o->modified.v[i], true);
        n++;
        safe += l == DIFF_SAFE;
        warn += l == DIFF_WARNING;
        bad += l == DIFF_UNSAFE;
    }
    if (o->diff_list) {
        FILE *f = fopen(o->diff_list, "r");
        if (!f) {
            fprintf(stderr, "ERROR reading diff list: %s\n", strerror(errno));
            return 1;
        }
        char *line = NULL;
        size_t cap = 0;
        int ln = 0;
        while (getline(&line, &cap, f) >= 0) {
            ln++;
            char *a, *b;
            if (!split_two(line, &a, &b))
                continue;
            if (!b) {
                fprintf(stderr, "ERROR: diff list line %d expects two paths\n", ln);
                free(line);
                fclose(f);
                return 1;
            }
            DiffLevel l = diff_files(a, b, true);
            n++;
            safe += l == DIFF_SAFE;
            warn += l == DIFF_WARNING;
            bad += l == DIFF_UNSAFE;
        }
        free(line);
        fclose(f);
    }
    if (!n) {
        fprintf(stderr, "ERROR: no files to compare. Use --original/--modified or --diff-list\n");
        return 1;
    }
    diff_summary(n, safe, warn, bad);
    return 0;
}

int main(int argc, char **argv) {
    Options o;
    if (!parse_options(argc, argv, &o)) {
        usage(stderr, argv[0]);
        return 2;
    }
    int rc = 0;
    if (o.help) {
        usage(stdout, argv[0]);
        goto done;
    }
    if (o.version) {
        printf("ps1_tim_tool %s\n", PS1_TOOL_VERSION);
        goto done;
    }
    if (o.diff) {
        rc = run_diff(&o);
        goto done;
    }
    if (o.extract) {
        char ext[32];
        if (o.extract[0] == '.')
            snprintf(ext, sizeof(ext), "%s", o.extract);
        else
            snprintf(ext, sizeof(ext), ".%s", o.extract);
        if (strcasecmp(ext, ".png") && strcasecmp(ext, ".bmp") && strcasecmp(ext, ".tga") &&
            strcasecmp(ext, ".tif") && strcasecmp(ext, ".tiff") && strcasecmp(ext, ".webp") &&
            strcasecmp(ext, ".ppm")) {
            fprintf(stderr,
                    "ERROR: unsupported extract format '%s'. Supported: png, bmp, tga, tif, tiff, "
                    "webp, ppm\n",
                    o.extract);
            rc = 1;
            goto done;
        }
        if (!o.images.n) {
            fprintf(stderr, "ERROR: provide one or more .tim files with --extract\n");
            rc = 1;
            goto done;
        }
        int ok = 0, fail = 0;
        printf("Extracting %zu file(s) -> [%s]\n\n", o.images.n, ext);
        for (size_t i = 0; i < o.images.n; i++) {
            printf("  Extracting: %s ... ", path_base(o.images.v[i]));
            char *out = NULL;
            if (extract_tim(o.images.v[i], ext, o.palette_index, &out)) {
                struct stat st;
                stat(out, &st);
                printf("done (%.1f KB) -> %s\n", st.st_size / 1024.0, out);
                free(out);
                ok++;
            } else {
                printf("FAILED (%s)\n", g_error);
                fail++;
            }
        }
        printf("\nDone: %d succeeded, %d failed\n", ok, fail);
        rc = fail ? 1 : 0;
        goto done;
    }
    if (o.verify) {
        if (!o.images.n) {
            fprintf(stderr, "ERROR: provide one or more .tim files with --verify\n");
            rc = 1;
            goto done;
        }
        bool all = true;
        for (size_t i = 0; i < o.images.n; i++)
            if (!verify_tim_file(o.images.v[i]))
                all = false;
        puts(all ? "All files verified successfully." : "One or more files failed verification.");
        rc = all ? 0 : 1;
        goto done;
    }
    if (o.info) {
        if (!o.images.n) {
            fprintf(stderr, "ERROR: provide a .tim file with --info\n");
            rc = 1;
            goto done;
        }
        for (size_t i = 0; i < o.images.n; i++)
            if (!tim_info_file(o.images.v[i])) {
                fprintf(stderr, "  FAILED: %s (%s)\n", o.images.v[i], g_error);
                rc = 1;
            }
        goto done;
    }
    if (o.list) {
        rc = run_batch(&o);
        goto done;
    }
    if (o.list_formats || !o.images.n) {
        list_formats();
        goto done;
    }
    if (fmt_number(o.format) < 0) {
        fprintf(stderr, "ERROR: --format is required. Choose: 4bit | 8bit | 16bit | 24bit\n\n");
        list_formats();
        rc = 1;
        goto done;
    }
    if (o.palette_count < 1) {
        fprintf(stderr, "ERROR: --palette-count must be >= 1\n");
        rc = 1;
        goto done;
    }
    if (o.output_dir && !mkdir_p(o.output_dir)) {
        fprintf(stderr, "ERROR: %s\n", g_error);
        rc = 1;
        goto done;
    }
    if (o.output && o.images.n > 1)
        fprintf(stderr, "WARNING: --output is ignored when converting multiple files.\n");
    printf("Converting %zu file(s) -> [%s]\n\n", o.images.n, o.format);
    int ok = 0, fail = 0;
    for (size_t i = 0; i < o.images.n; i++) {
        const char *forced = (o.images.n == 1) ? o.output : NULL;
        char *out = convert_one(o.images.v[i], o.format, &o, forced);
        if (out) {
            ok++;
            free(out);
        } else {
            printf("  FAILED: %s (%s)\n", o.images.v[i], g_error);
            fail++;
        }
    }
    printf("\nDone: %d succeeded, %d failed\n", ok, fail);
    rc = fail ? 1 : 0;
done:
    sv_free(&o.images);
    sv_free(&o.original);
    sv_free(&o.modified);
    return rc;
}
