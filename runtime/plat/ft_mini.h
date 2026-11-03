/* The FreeType calls nnfontll.c makes, without FreeType (Android has none): FT_New_Memory_Face reads a font (or one face of a
 * collection) from memory, FT_Load_Glyph decodes a glyph's outline in font units (no hinting: what FT_LOAD_NO_SCALE |
 * FT_LOAD_NO_HINTING gives), FT_Outline_Decompose walks it as FreeType does (implied on-curve points between two off-curve
 * ones, the contour closed back to its start). TrueType outlines (glyf: simple and composite glyphs; the console's system
 * fonts, runtime/fonts_open) and CFF ones (the "OTTO" fonts the game ships: Rodin, Seurat, TsukuMin, UD Kakugo; CID-keyed with
 * FDSelect or not; Type 2 charstrings with their subroutines, flex and hint masks; seac accents and the arithmetic operators
 * are not read: no font of the game uses them). Not thread-safe per face (nnfontll.c holds its lock). */
#ifndef PLAT_FT_MINI_H
#define PLAT_FT_MINI_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef long FT_Long;
typedef long FT_Pos;
typedef struct { FT_Pos x, y; } FT_Vector;
typedef struct { int n_contours, n_points; FT_Vector *points; char *tags; int *contours; } FT_Outline;     /* tags bit 0: on the curve */
typedef int (*FT_Outline_MoveToFunc)(const FT_Vector *to, void *user);
typedef int (*FT_Outline_LineToFunc)(const FT_Vector *to, void *user);
typedef int (*FT_Outline_ConicToFunc)(const FT_Vector *control, const FT_Vector *to, void *user);
typedef int (*FT_Outline_CubicToFunc)(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user);
typedef struct { FT_Outline_MoveToFunc move_to; FT_Outline_LineToFunc line_to; FT_Outline_ConicToFunc conic_to; FT_Outline_CubicToFunc cubic_to; int shift; FT_Pos delta; } FT_Outline_Funcs;
#define FT_GLYPH_FORMAT_OUTLINE 0x6f75746c
#define FT_LOAD_NO_SCALE 1
#define FT_LOAD_NO_HINTING 2
#define FT_LOAD_NO_BITMAP 8
typedef struct { int format; FT_Outline outline; int cap_points, cap_contours; } FT_GlyphSlotRec, *FT_GlyphSlot;
typedef struct {
    FT_GlyphSlot glyph; FT_GlyphSlotRec slot; const uint8_t *data; size_t size; uint32_t glyf, glyf_len, loca, loca_len; int long_loca, num_glyphs;
    const uint8_t *cff; uint32_t cff_len, cs, gsubr, lsubr, fdarray, fdselect; int cid;      /* CFF: offsets in the table (INDEXes; lsubr 0: none) */
} FT_FaceRec, *FT_Face;
typedef void *FT_Library;

static uint32_t ftm_u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned ftm_u16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static int ftm_s16(const uint8_t *p) { return (int16_t)ftm_u16(p); }
static int FT_Init_FreeType(FT_Library *lib) { *lib = (FT_Library)(uintptr_t)1; return 0; }

/* ---- CFF: INDEXes and DICTs (Adobe TN 5176), Type 2 charstrings (TN 5177) */
static uint32_t ftm_off(const uint8_t *p, unsigned n) { uint32_t v = 0; while (n--) v = v << 8 | *p++; return v; }
/* element i of the INDEX at off: its bytes [*a, *b) in the table; 0 when out of range */
static int ftm_index_at(const uint8_t *t, uint32_t len, uint32_t off, unsigned i, uint32_t *a, uint32_t *b) {
    if (off + 3 > len) return 0;
    unsigned count = ftm_u16(t + off), os = t[off + 2];
    if (i >= count || os < 1 || os > 4 || off + 3 + (uint64_t)(count + 1) * os > len) return 0;
    uint32_t base = off + 3 + (count + 1) * os - 1;
    *a = base + ftm_off(t + off + 3 + i * os, os); *b = base + ftm_off(t + off + 3 + (i + 1) * os, os);
    return *a <= *b && *b <= len;
}
static unsigned ftm_index_count(const uint8_t *t, uint32_t len, uint32_t off) { return off + 2 <= len ? ftm_u16(t + off) : 0; }
static uint32_t ftm_index_end(const uint8_t *t, uint32_t len, uint32_t off) {      /* the offset after the INDEX, 0: broken */
    uint32_t a, b;
    unsigned n = ftm_index_count(t, len, off);
    if (off + 2 > len) return 0;
    if (!n) return off + 2;
    return ftm_index_at(t, len, off, n - 1, &a, &b) ? b : 0;
}
/* the integer operands of operator op (12 x: 1200 + x) in the DICT [p, end); their number, -1 when it is not there */
static int ftm_dict(const uint8_t *p, const uint8_t *end, int op, long *v, int max) {
    long st[48];
    int n = 0;
    while (p < end) {
        unsigned b = *p++;
        if (b <= 21) {
            int o = b == 12 ? (p < end ? 1200 + *p++ : -1) : (int)b;
            if (o == op) { for (int i = 0; i < n && i < max; i++) v[i] = st[i]; return n; }
            n = 0;
        } else {
            long x;
            if (b >= 32 && b <= 246) x = (long)b - 139;
            else if (b >= 247 && b <= 250) { if (p >= end) return -1; x = ((long)b - 247) * 256 + *p++ + 108; }
            else if (b >= 251 && b <= 254) { if (p >= end) return -1; x = -((long)b - 251) * 256 - *p++ - 108; }
            else if (b == 28) { if (p + 2 > end) return -1; x = (int16_t)ftm_u16(p); p += 2; }
            else if (b == 29) { if (p + 4 > end) return -1; x = (int32_t)ftm_u32(p); p += 4; }
            else if (b == 30) { x = 0; while (p < end) { uint8_t q = *p++; if ((q & 15) == 15 || (q >> 4) == 15) break; } }      /* a real: not needed */
            else return -1;
            if (n < 48) st[n++] = x;
        }
    }
    return -1;
}
static int ftm_cff_face(FT_Face f, const uint8_t *t, uint32_t len) {
    uint32_t at, top_a, top_b;
    long v[4];
    if (len < 4 || t[0] != 1) return 0;
    at = ftm_index_end(t, len, t[2]);                                          /* Name INDEX */
    if (!at || !ftm_index_at(t, len, at, 0, &top_a, &top_b)) return 0;        /* Top DICT INDEX: the first (only) font */
    at = ftm_index_end(t, len, at);
    at = at ? ftm_index_end(t, len, at) : 0;                                   /* String INDEX, then the Global Subr INDEX */
    if (!at) return 0;
    f->gsubr = at;
    if (ftm_dict(t + top_a, t + top_b, 17, v, 1) != 1 || v[0] <= 0 || (uint32_t)v[0] >= len) return 0;
    f->cs = (uint32_t)v[0];
    f->cid = ftm_dict(t + top_a, t + top_b, 1230, v, 3) >= 0;                  /* ROS: CID-keyed, a Private DICT per Font DICT */
    if (f->cid) {
        if (ftm_dict(t + top_a, t + top_b, 1236, v, 1) != 1 || v[0] <= 0) return 0;
        f->fdarray = (uint32_t)v[0];
        if (ftm_dict(t + top_a, t + top_b, 1237, v, 1) != 1 || v[0] <= 0 || (uint32_t)v[0] >= len) return 0;
        f->fdselect = (uint32_t)v[0];
    } else if (ftm_dict(t + top_a, t + top_b, 18, v, 2) == 2 && v[1] > 0 && (uint64_t)v[1] + (uint64_t)v[0] <= len) {
        long s[1];
        if (ftm_dict(t + v[1], t + v[1] + v[0], 19, s, 1) == 1 && s[0] > 0) f->lsubr = (uint32_t)(v[1] + s[0]);
    }
    f->num_glyphs = (int)ftm_index_count(t, len, f->cs);
    return f->num_glyphs > 0;
}
static uint32_t ftm_cff_lsubr(FT_Face f, unsigned g) {      /* the local subroutines of glyph g (CID: its Font DICT's) */
    const uint8_t *t = f->cff;
    uint32_t len = f->cff_len, fs = f->fdselect, a, b;
    unsigned fd = 0;
    long v[2], s[1];
    if (!f->cid) return f->lsubr;
    if (fs >= len) return 0;
    if (t[fs] == 0) { if (fs + 1 + g >= len) return 0; fd = t[fs + 1 + g]; }
    else if (t[fs] == 3) {
        unsigned n = fs + 3 <= len ? ftm_u16(t + fs + 1) : 0;
        for (unsigned i = 0; i < n && fs + 3 + 3 * (i + 1) + 2 <= len; i++)
            if (g >= ftm_u16(t + fs + 3 + 3 * i) && g < ftm_u16(t + fs + 3 + 3 * (i + 1))) { fd = t[fs + 3 + 3 * i + 2]; break; }
    } else return 0;
    if (!ftm_index_at(t, len, f->fdarray, fd, &a, &b) || ftm_dict(t + a, t + b, 18, v, 2) != 2 || v[1] <= 0 || (uint64_t)v[1] + (uint64_t)v[0] > len) return 0;
    return ftm_dict(t + v[1], t + v[1] + v[0], 19, s, 1) == 1 && s[0] > 0 ? (uint32_t)(v[1] + s[0]) : 0;
}
static int ftm_room(FT_GlyphSlot s, int points, int contours);
typedef struct { FT_Face f; double st[64], x, y; int n, stems, width_done, open, first; uint32_t lsubr; } FtmCs;
static int ftm_cs_point(FtmCs *c, double x, double y, char tag) {
    FT_Outline *o = &c->f->slot.outline;
    if (!ftm_room(&c->f->slot, 1, 1)) return 0;
    o->points[o->n_points].x = (FT_Pos)(x < 0 ? x - 0.5 : x + 0.5); o->points[o->n_points].y = (FT_Pos)(y < 0 ? y - 0.5 : y + 0.5);
    o->tags[o->n_points++] = tag;
    return 1;
}
static void ftm_cs_close(FtmCs *c) {
    FT_Outline *o = &c->f->slot.outline;
    if (!c->open) return;
    c->open = 0;
    if (o->n_points - c->first < 2) { o->n_points = c->first; return; }      /* a lone moveto: no contour */
    o->contours[o->n_contours++] = o->n_points - 1;
}
static int ftm_cs_move(FtmCs *c) { ftm_cs_close(c); c->first = c->f->slot.outline.n_points; c->open = 1; return ftm_cs_point(c, c->x, c->y, 1); }
static int ftm_cs_line(FtmCs *c, double dx, double dy) { c->x += dx; c->y += dy; return ftm_cs_point(c, c->x, c->y, 1); }
static int ftm_cs_curve(FtmCs *c, double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
    double x1 = c->x + dx1, y1 = c->y + dy1, x2 = x1 + dx2, y2 = y1 + dy2;
    c->x = x2 + dx3; c->y = y2 + dy3;
    return ftm_cs_point(c, x1, y1, 2) && ftm_cs_point(c, x2, y2, 2) && ftm_cs_point(c, c->x, c->y, 1);
}
static unsigned ftm_bias(unsigned n) { return n < 1240 ? 107 : n < 33900 ? 1131 : 32768; }
static int ftm_cs_run(FtmCs *c, const uint8_t *p, const uint8_t *end, int depth) {      /* 1: go on, 2: endchar, 0: broken */
    double *s = c->st;
    if (depth > 10) return 0;
    while (p < end) {
        unsigned b = *p++;
        if (b >= 32 || b == 28) {                  /* an operand */
            double x;
            if (b == 28) { if (p + 2 > end) return 0; x = (int16_t)ftm_u16(p); p += 2; }
            else if (b <= 246) x = (double)b - 139;
            else if (b <= 250) { if (p >= end) return 0; x = ((double)b - 247) * 256 + *p++ + 108; }
            else if (b <= 254) { if (p >= end) return 0; x = -((double)b - 251) * 256 - *p++ - 108; }
            else { if (p + 4 > end) return 0; x = (int32_t)ftm_u32(p) / 65536.0; p += 4; }
            if (c->n >= 64) return 0;
            s[c->n++] = x;
            continue;
        }
        int n = c->n, i = 0, k;
        switch (b) {
        case 1: case 3: case 18: case 23:          /* hstem vstem hstemhm vstemhm: hints are not used (unhinted outlines) */
            if (!c->width_done && (n & 1)) i = 1;
            c->width_done = 1; c->stems += (n - i) / 2; break;
        case 19: case 20:                          /* hintmask cntrmask: an implied vstem before, then the mask's bytes */
            if (!c->width_done && (n & 1)) i = 1;
            c->width_done = 1; c->stems += (n - i) / 2; p += (c->stems + 7) / 8; break;
        case 21:                                   /* rmoveto */
            if (!c->width_done && n > 2) i = 1;
            c->width_done = 1; if (n - i < 2) return 0;
            c->x += s[i]; c->y += s[i + 1]; if (!ftm_cs_move(c)) return 0; break;
        case 22: case 4:                           /* hmoveto vmoveto */
            if (!c->width_done && n > 1) i = 1;
            c->width_done = 1; if (n - i < 1) return 0;
            if (b == 22) c->x += s[i]; else c->y += s[i];
            if (!ftm_cs_move(c)) return 0; break;
        case 5:                                    /* rlineto */
            for (; i + 1 < n; i += 2) if (!ftm_cs_line(c, s[i], s[i + 1])) return 0;
            break;
        case 6: case 7:                            /* hlineto vlineto: alternating */
            for (k = b == 7; i < n; i++, k ^= 1) if (!ftm_cs_line(c, k ? 0 : s[i], k ? s[i] : 0)) return 0;
            break;
        case 8:                                    /* rrcurveto */
            for (; i + 5 < n; i += 6) if (!ftm_cs_curve(c, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5])) return 0;
            break;
        case 27: {                                 /* hhcurveto: [dy1] {dxa dxb dyb dxc}+ */
            double dy1 = 0;
            if (n & 1) dy1 = s[i++];
            for (; i + 3 < n; i += 4, dy1 = 0) if (!ftm_cs_curve(c, s[i], dy1, s[i + 1], s[i + 2], s[i + 3], 0)) return 0;
            break; }
        case 26: {                                 /* vvcurveto: [dx1] {dya dxb dyb dyc}+ */
            double dx1 = 0;
            if (n & 1) dx1 = s[i++];
            for (; i + 3 < n; i += 4, dx1 = 0) if (!ftm_cs_curve(c, dx1, s[i], s[i + 1], s[i + 2], 0, s[i + 3])) return 0;
            break; }
        case 30: case 31:                          /* vhcurveto hvcurveto: alternating, the last with an optional df */
            for (k = b == 31; i + 3 < n; i += 4, k ^= 1) {
                double last = n - (i + 4) == 1 ? s[i + 4] : 0;
                if (k) { if (!ftm_cs_curve(c, s[i], 0, s[i + 1], s[i + 2], last, s[i + 3])) return 0; }
                else if (!ftm_cs_curve(c, 0, s[i], s[i + 1], s[i + 2], s[i + 3], last)) return 0;
            }
            break;
        case 24:                                   /* rcurveline: curves, then a line */
            for (; i + 7 < n; i += 6) if (!ftm_cs_curve(c, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5])) return 0;
            if (i + 1 < n && !ftm_cs_line(c, s[i], s[i + 1])) return 0;
            break;
        case 25:                                   /* rlinecurve: lines, then a curve */
            for (; i + 7 < n; i += 2) if (!ftm_cs_line(c, s[i], s[i + 1])) return 0;
            if (i + 5 < n && !ftm_cs_curve(c, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5])) return 0;
            break;
        case 10: case 29: {                        /* callsubr callgsubr: the operands stay on the stack */
            uint32_t idx = b == 10 ? c->lsubr : c->f->gsubr, a2, b2;
            if (!n || (b == 10 && !idx)) return 0;
            long sub = (long)s[--c->n] + (long)ftm_bias(ftm_index_count(c->f->cff, c->f->cff_len, idx));
            if (sub < 0 || !ftm_index_at(c->f->cff, c->f->cff_len, idx, (unsigned)sub, &a2, &b2)) return 0;
            int r = ftm_cs_run(c, c->f->cff + a2, c->f->cff + b2, depth + 1);
            if (r != 1) return r;
            continue; }
        case 11: return 1;                         /* return */
        case 14:                                   /* endchar (with 4 more operands: seac, an accented glyph: not read) */
            ftm_cs_close(c); return 2;
        case 12: {
            if (p >= end) return 0;
            unsigned e = *p++;
            if (e == 35 && n >= 12) {              /* flex: two curves */
                if (!ftm_cs_curve(c, s[0], s[1], s[2], s[3], s[4], s[5]) || !ftm_cs_curve(c, s[6], s[7], s[8], s[9], s[10], s[11])) return 0;
            } else if (e == 34 && n >= 7) {        /* hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6, back to the starting y */
                if (!ftm_cs_curve(c, s[0], 0, s[1], s[2], s[3], 0) || !ftm_cs_curve(c, s[4], 0, s[5], -s[2], s[6], 0)) return 0;
            } else if (e == 36 && n >= 9) {        /* hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6, back to the starting y */
                if (!ftm_cs_curve(c, s[0], s[1], s[2], s[3], s[4], 0) || !ftm_cs_curve(c, s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]))) return 0;
            } else if (e == 37 && n >= 11) {       /* flex1: the last point's other coordinate returns to the start's */
                double dx = s[0] + s[2] + s[4] + s[6] + s[8], dy = s[1] + s[3] + s[5] + s[7] + s[9];
                if (!ftm_cs_curve(c, s[0], s[1], s[2], s[3], s[4], s[5])) return 0;
                if (fabs(dx) > fabs(dy)) { if (!ftm_cs_curve(c, s[6], s[7], s[8], s[9], s[10], -dy)) return 0; }
                else if (!ftm_cs_curve(c, s[6], s[7], s[8], s[9], -dx, s[10])) return 0;
            } else return 0;                       /* the arithmetic operators: no font of the game has them */
            break; }
        default: return 0;
        }
        c->n = 0;
    }
    return 1;
}
static int ftm_cff_glyph(FT_Face f, unsigned g) {
    FtmCs c;
    uint32_t a, b;
    if (!ftm_index_at(f->cff, f->cff_len, f->cs, g, &a, &b)) return 0;
    memset(&c, 0, sizeof c);
    c.f = f; c.lsubr = ftm_cff_lsubr(f, g);
    int r = ftm_cs_run(&c, f->cff + a, f->cff + b, 0);
    ftm_cs_close(&c);
    return r != 0;
}

static int FT_New_Memory_Face(FT_Library lib, const unsigned char *data, FT_Long size, FT_Long index, FT_Face *out) {
    const uint8_t *d = data;
    uint32_t off = 0, head = 0, maxp = 0, cff = 0, cff_len = 0;
    FT_Face f;
    (void)lib;
    if (size < 12) return 1;
    if (!memcmp(d, "ttcf", 4)) {                   /* a collection: the face's own table directory */
        if (size < 16 || (uint32_t)index >= ftm_u32(d + 8) || 12 + 4 * (size_t)index + 4 > (size_t)size) return 1;
        off = ftm_u32(d + 12 + 4 * index);
        if ((size_t)off + 12 > (size_t)size) return 1;
    }
    int otto = !memcmp(d + off, "OTTO", 4);      /* CFF outlines */
    if (ftm_u32(d + off) != 0x00010000 && memcmp(d + off, "true", 4) && !otto) return 1;
    if (!(f = calloc(1, sizeof *f))) return 1;
    f->data = d; f->size = (size_t)size;
    for (unsigned i = 0, n = ftm_u16(d + off + 4); i < n && off + 12 + 16 * (size_t)i + 16 <= (size_t)size; i++) {
        const uint8_t *e = d + off + 12 + 16 * i;
        uint32_t at = ftm_u32(e + 8), len = ftm_u32(e + 12);
        if ((size_t)at + len > (size_t)size) continue;
        if (!memcmp(e, "glyf", 4)) { f->glyf = at; f->glyf_len = len; }
        else if (!memcmp(e, "loca", 4)) { f->loca = at; f->loca_len = len; }
        else if (!memcmp(e, "head", 4) && len >= 54) head = at;
        else if (!memcmp(e, "maxp", 4) && len >= 6) maxp = at;
        else if (!memcmp(e, "CFF ", 4)) { cff = at; cff_len = len; }
    }
    if (otto) {
        f->cff = d + cff; f->cff_len = cff_len;
        if (!cff || !ftm_cff_face(f, f->cff, cff_len)) { free(f); return 1; }
    } else {
        if (!f->glyf || !f->loca || !head || !maxp) { free(f); return 1; }
        f->long_loca = ftm_s16(d + head + 50) != 0;
        f->num_glyphs = (int)ftm_u16(d + maxp + 4);
    }
    f->glyph = &f->slot;
    *out = f;
    return 0;
}
static int ftm_room(FT_GlyphSlot s, int points, int contours) {
    if (s->outline.n_points + points > s->cap_points) {
        int cap = (s->outline.n_points + points) * 2;
        FT_Vector *p = realloc(s->outline.points, (size_t)cap * sizeof *p);
        char *t = realloc(s->outline.tags, (size_t)cap);
        if (p) s->outline.points = p;
        if (t) s->outline.tags = t;
        if (!p || !t) return 0;
        s->cap_points = cap;
    }
    if (s->outline.n_contours + contours > s->cap_contours) {
        int cap = (s->outline.n_contours + contours) * 2, *c = realloc(s->outline.contours, (size_t)cap * sizeof *c);
        if (!c) return 0;
        s->outline.contours = c; s->cap_contours = cap;
    }
    return 1;
}
/* One glyph's points appended to the outline, through the 2x2 matrix m (16.16) and the offset (dx, dy). 0: unreadable. */
static int ftm_glyph(FT_Face f, unsigned g, const long *m, long dx, long dy, int depth) {
    FT_Outline *o = &f->slot.outline;
    const uint8_t *d = f->data;
    uint32_t a, b;
    if ((int)g >= f->num_glyphs || depth > 8) return 0;
    if (f->long_loca) { if (4 * (size_t)g + 8 > f->loca_len) return 0; a = ftm_u32(d + f->loca + 4 * g); b = ftm_u32(d + f->loca + 4 * g + 4); }
    else { if (2 * (size_t)g + 4 > f->loca_len) return 0; a = 2 * ftm_u16(d + f->loca + 2 * g); b = 2 * ftm_u16(d + f->loca + 2 * g + 2); }
    if (b <= a) return 1;                          /* an empty glyph (a space) */
    if (b > f->glyf_len || b - a < 10) return 0;
    const uint8_t *p = d + f->glyf + a, *end = d + f->glyf + b;
    int nc = ftm_s16(p);
    p += 10;
    if (nc >= 0) {                                 /* a simple glyph: end points, instructions, flags, x, y */
        if (p + 2 * nc + 2 > end) return 0;
        int np = nc ? (int)ftm_u16(p + 2 * (nc - 1)) + 1 : 0, base = o->n_points;
        if (!ftm_room(&f->slot, np, nc)) return 0;
        for (int i = 0; i < nc; i++) o->contours[o->n_contours + i] = base + (int)ftm_u16(p + 2 * i);
        p += 2 * nc;
        p += 2 + ftm_u16(p);
        uint8_t *fl = (uint8_t *)o->tags + base;
        for (int i = 0; i < np;) {
            if (p >= end) return 0;
            uint8_t c = *p++;
            int rep = 1;
            if (c & 8) { if (p >= end) return 0; rep += *p++; }
            while (rep-- && i < np) fl[i++] = c;
        }
        long v = 0;
        for (int k = 0; k < 2; k++) {              /* x, then y: short (1 byte, bit 1/2 = sign) or long, or "same" */
            uint8_t sh = k ? 4 : 2, same = k ? 32 : 16;
            v = 0;
            for (int i = 0; i < np; i++) {
                if (fl[i] & sh) { if (p >= end) return 0; v += (fl[i] & same) ? *p : -(long)*p; p++; }
                else if (!(fl[i] & same)) { if (p + 2 > end) return 0; v += ftm_s16(p); p += 2; }
                if (k) o->points[base + i].y = v; else o->points[base + i].x = v;
            }
        }
        for (int i = 0; i < np; i++) {             /* the transform of a composite's component, the on-curve tag */
            long x = o->points[base + i].x, y = o->points[base + i].y;
            o->points[base + i].x = (long)(((int64_t)x * m[0] + (int64_t)y * m[2]) / 65536) + dx;
            o->points[base + i].y = (long)(((int64_t)x * m[1] + (int64_t)y * m[3]) / 65536) + dy;
            fl[i] &= 1;
        }
        o->n_points += np; o->n_contours += nc;
        return 1;
    }
    for (unsigned flags = 0x20; flags & 0x20;) {   /* a composite: its components, each with its own placement */
        if (p + 4 > end) return 0;
        flags = ftm_u16(p);
        unsigned cg = ftm_u16(p + 2);
        long a1, a2, cm[4] = {65536, 0, 0, 65536};
        p += 4;
        if (flags & 1) { if (p + 4 > end) return 0; a1 = ftm_s16(p); a2 = ftm_s16(p + 2); p += 4; }
        else { if (p + 2 > end) return 0; a1 = (flags & 2) ? (int8_t)p[0] : p[0]; a2 = (flags & 2) ? (int8_t)p[1] : p[1]; p += 2; }
        if (flags & 8) { if (p + 2 > end) return 0; cm[0] = cm[3] = (long)ftm_s16(p) * 4; p += 2; }                   /* F2Dot14 -> 16.16 */
        else if (flags & 0x40) { if (p + 4 > end) return 0; cm[0] = (long)ftm_s16(p) * 4; cm[3] = (long)ftm_s16(p + 2) * 4; p += 4; }
        else if (flags & 0x80) { if (p + 8 > end) return 0; for (int i = 0; i < 4; i++) cm[i] = (long)ftm_s16(p + 2 * i) * 4; p += 8; }
        long nm[4] = {(long)(((int64_t)cm[0] * m[0] + (int64_t)cm[1] * m[2]) / 65536), (long)(((int64_t)cm[0] * m[1] + (int64_t)cm[1] * m[3]) / 65536),
                      (long)(((int64_t)cm[2] * m[0] + (int64_t)cm[3] * m[2]) / 65536), (long)(((int64_t)cm[2] * m[1] + (int64_t)cm[3] * m[3]) / 65536)};
        int first = o->n_points;
        if (flags & 2) {                           /* the offset, in the parent's units (TrueType's unscaled offset) */
            long ox = (long)(((int64_t)a1 * m[0] + (int64_t)a2 * m[2]) / 65536), oy = (long)(((int64_t)a1 * m[1] + (int64_t)a2 * m[3]) / 65536);
            if (!ftm_glyph(f, cg, nm, dx + ox, dy + oy, depth + 1)) return 0;
        } else {                                   /* point matching: point a2 of the component onto point a1 so far */
            if (!ftm_glyph(f, cg, nm, 0, 0, depth + 1)) return 0;
            if (a1 < first && first + a2 < o->n_points) {
                long mx = o->points[a1].x - o->points[first + a2].x, my = o->points[a1].y - o->points[first + a2].y;
                for (int i = first; i < o->n_points; i++) { o->points[i].x += mx; o->points[i].y += my; }
            }
        }
    }
    return 1;
}
static int FT_Load_Glyph(FT_Face f, unsigned glyph, int flags) {
    static const long id[4] = {65536, 0, 0, 65536};
    (void)flags;
    f->slot.outline.n_points = f->slot.outline.n_contours = 0;
    f->slot.format = FT_GLYPH_FORMAT_OUTLINE;
    if (f->cff ? ftm_cff_glyph(f, glyph) : ftm_glyph(f, glyph, id, 0, 0, 0)) return 0;
    f->slot.outline.n_points = f->slot.outline.n_contours = 0;
    return 1;
}
static int FT_Outline_Decompose(const FT_Outline *o, const FT_Outline_Funcs *fn, void *user) {
    for (int c = 0, first = 0; c < o->n_contours; first = o->contours[c++] + 1) {
        int last = o->contours[c];
        if (last < first || last >= o->n_points) continue;
        FT_Vector start = o->points[first], ctrl;
        int i = first, have_ctrl = 0;
        if (!(o->tags[first] & 1)) {               /* starts off the curve: at the last point if it is on it, else midway */
            if (o->tags[last] & 1) { start = o->points[last]; last--; }
            else { start.x = (o->points[first].x + o->points[last].x) / 2; start.y = (o->points[first].y + o->points[last].y) / 2; }
            ctrl = o->points[first]; have_ctrl = 1; i = first + 1;
        } else i = first + 1;
        if (fn->move_to(&start, user)) return 1;
        for (; i <= last; i++) {
            FT_Vector pt = o->points[i];
            if (o->tags[i] & 2) {                  /* CFF: two cubic control points, then a point on the curve (or the start) */
                FT_Vector c2 = i + 1 <= last ? o->points[i + 1] : start, to = i + 2 <= last ? o->points[i + 2] : start;
                if (fn->cubic_to(&pt, &c2, &to, user)) return 1;
                if (i + 2 > last) { have_ctrl = -1; break; }
                i += 2;
                continue;
            }
            if (o->tags[i] & 1) {
                if (have_ctrl) { if (fn->conic_to(&ctrl, &pt, user)) return 1; have_ctrl = 0; }
                else if (fn->line_to(&pt, user)) return 1;
            } else {
                if (have_ctrl) {
                    FT_Vector mid = {(ctrl.x + pt.x) / 2, (ctrl.y + pt.y) / 2};
                    if (fn->conic_to(&ctrl, &mid, user)) return 1;
                }
                ctrl = pt; have_ctrl = 1;
            }
        }
        if (have_ctrl > 0) { if (fn->conic_to(&ctrl, &start, user)) return 1; }
        else if (!have_ctrl && fn->line_to(&start, user)) return 1;      /* (-1: a curve ended at the start) */
    }
    return 0;
}
#endif
