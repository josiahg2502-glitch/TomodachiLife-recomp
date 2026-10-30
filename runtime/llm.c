/* LLM: a small local language model for the Mii chatter mod (included by mii_chatter.c). Free and offline: the user puts
 * a GGUF model file in build\ai (Qwen2.5-1.5B-Instruct, Apache 2.0, is the one this was made for). It is memory-mapped,
 * tokenized with its own byte-level BPE vocabulary (GPT-2 style; the qwen2 / llama-bpe pre-tokenizer, written out for
 * ASCII text) and run on the CPU by a few low-priority threads: Qwen2 and Llama layouts; F32, F16, BF16, Q4_0, Q4_1,
 * Q5_0, Q5_1, Q8_0, Q4_K, Q5_K and Q6_K weights; AVX2 + FMA where the CPU has them. One sequence: a fixed prefix stays in
 * the key/value cache and every line rewinds to it (llm_rewind). */
#include <math.h>
#include <ctype.h>
#include <immintrin.h>
#include <intrin.h>
#if defined(_MSC_VER) && !defined(__AVX__)        /* (immintrin.h leaves these out unless the whole file is built for AVX) */
#include <avxintrin.h>
#include <avx2intrin.h>
#include <fmaintrin.h>
#endif

enum { GT_F32 = 0, GT_F16 = 1, GT_Q4_0 = 2, GT_Q4_1 = 3, GT_Q5_0 = 6, GT_Q5_1 = 7, GT_Q8_0 = 8, GT_Q4_K = 12, GT_Q5_K = 13, GT_Q6_K = 14, GT_BF16 = 30 };
typedef struct { const uint8_t *data; int type; int64_t ne[4]; } LlmT;
typedef struct { const char *s; uint32_t n; } LlmStr;
typedef struct { LlmT attn_norm, wq, wk, wv, wo, bq, bk, bv, ffn_norm, w_gate, w_up, w_down; } LlmLayer;
#define LLM_MAXT 16
#define LLM_BATCH 32
typedef struct Llm Llm;
typedef void (*LlmJob)(Llm *m, void *ctx, int t, int nt);
struct Llm {
    HANDLE file, map; const uint8_t *base; uint64_t size;
    int n_embd, n_layer, n_head, n_kv, hd, n_ff, n_vocab, n_ctx, rope_dim, neox;
    float eps, theta;
    LlmT tok_embd, out_norm, output;
    LlmLayer *L;
    LlmStr *vocab, *merges; const uint8_t *ttype; int ttype_kind, n_merges;
    uint32_t *vh, vh_mask, *mh, mh_mask;
    int digits3, eos, im_start, im_end, endoftext;
    float *kc, *vc, *logits, *inv_freq;
    int n_past;
    float *x, *xb, *q, *k, *v, *ao, *g, *u, *att, *rowbuf;
    int rowmax, avx2;
    int nt; HANDLE th[LLM_MAXT], go[LLM_MAXT], done[LLM_MAXT]; volatile LONG stop;
    LlmJob job; void *job_ctx;
};

/* ---- numbers ---- */
static inline uint16_t le16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, f = h & 0x3ff, u;
    if (!e) { float v = ldexpf((float)f, -24); return s ? -v : v; }
    u = e == 31 ? s | 0x7f800000u | (f << 13) : s | ((e + 112) << 23) | (f << 13);
    float r; memcpy(&r, &u, 4); return r;
}
static int gt_blck(int t) { return t == GT_F32 || t == GT_F16 || t == GT_BF16 ? 1 : t >= GT_Q4_K && t <= GT_Q6_K ? 256 : 32; }
static int gt_bytes(int t) {
    switch (t) {
    case GT_F32: return 4; case GT_F16: case GT_BF16: return 2; case GT_Q4_0: return 18; case GT_Q4_1: return 20; case GT_Q5_0: return 22;
    case GT_Q5_1: return 24; case GT_Q8_0: return 34; case GT_Q4_K: return 144; case GT_Q5_K: return 176; case GT_Q6_K: return 210;
    }
    return 0;
}
static size_t gt_row(int t, int64_t n) { return (size_t)(n / gt_blck(t)) * (size_t)gt_bytes(t); }
static inline void k_scale_min(int j, const uint8_t *q, uint8_t *d, uint8_t *m) {   /* Q4_K / Q5_K: the 6-bit scales */
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else { *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4); *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4); }
}
/* One row of weights to floats (the layouts are ggml's). Instantiated twice: plain and AVX2 + FMA (auto-vectorized). */
#define LLM_DQ_BODY \
    switch (t) { \
    case GT_F32: memcpy(y, s, (size_t)n * 4); break; \
    case GT_F16: for (int i = 0; i < n; i++) y[i] = h2f(le16(s + 2 * i)); break; \
    case GT_BF16: for (int i = 0; i < n; i++) { uint32_t w = (uint32_t)le16(s + 2 * i) << 16; memcpy(y + i, &w, 4); } break; \
    case GT_Q4_0: for (int b = 0; b < n / 32; b++, s += 18, y += 32) { float d = h2f(le16(s)); \
        for (int j = 0; j < 16; j++) { y[j] = (float)((s[2 + j] & 15) - 8) * d; y[j + 16] = (float)((s[2 + j] >> 4) - 8) * d; } } break; \
    case GT_Q4_1: for (int b = 0; b < n / 32; b++, s += 20, y += 32) { float d = h2f(le16(s)), mn = h2f(le16(s + 2)); \
        for (int j = 0; j < 16; j++) { y[j] = (float)(s[4 + j] & 15) * d + mn; y[j + 16] = (float)(s[4 + j] >> 4) * d + mn; } } break; \
    case GT_Q5_0: for (int b = 0; b < n / 32; b++, s += 22, y += 32) { float d = h2f(le16(s)); uint32_t qh; memcpy(&qh, s + 2, 4); \
        for (int j = 0; j < 16; j++) { int h0 = ((qh >> j) << 4) & 0x10, h1 = (qh >> (j + 12)) & 0x10; \
            y[j] = (float)(((s[6 + j] & 15) | h0) - 16) * d; y[j + 16] = (float)(((s[6 + j] >> 4) | h1) - 16) * d; } } break; \
    case GT_Q5_1: for (int b = 0; b < n / 32; b++, s += 24, y += 32) { float d = h2f(le16(s)), mn = h2f(le16(s + 2)); uint32_t qh; memcpy(&qh, s + 4, 4); \
        for (int j = 0; j < 16; j++) { int h0 = ((qh >> j) << 4) & 0x10, h1 = (qh >> (j + 12)) & 0x10; \
            y[j] = (float)((s[8 + j] & 15) | h0) * d + mn; y[j + 16] = (float)((s[8 + j] >> 4) | h1) * d + mn; } } break; \
    case GT_Q8_0: for (int b = 0; b < n / 32; b++, s += 34, y += 32) { float d = h2f(le16(s)); \
        for (int j = 0; j < 32; j++) y[j] = (float)(int8_t)s[2 + j] * d; } break; \
    case GT_Q4_K: for (int b = 0; b < n / 256; b++, s += 144) { float d = h2f(le16(s)), mn = h2f(le16(s + 2)); const uint8_t *sc = s + 4, *q = s + 16; \
        for (int j = 0, is = 0; j < 256; j += 64, q += 32, is += 2) { uint8_t a, c; k_scale_min(is, sc, &a, &c); float d1 = d * a, m1 = mn * c; \
            k_scale_min(is + 1, sc, &a, &c); float d2 = d * a, m2 = mn * c; \
            for (int l = 0; l < 32; l++) { y[l] = d1 * (float)(q[l] & 15) - m1; y[l + 32] = d2 * (float)(q[l] >> 4) - m2; } y += 64; } } break; \
    case GT_Q5_K: for (int b = 0; b < n / 256; b++, s += 176) { float d = h2f(le16(s)), mn = h2f(le16(s + 2)); const uint8_t *sc = s + 4, *qh = s + 16, *q = s + 48; \
        int u1 = 1, u2 = 2; \
        for (int j = 0, is = 0; j < 256; j += 64, q += 32, is += 2, u1 <<= 2, u2 <<= 2) { uint8_t a, c; k_scale_min(is, sc, &a, &c); float d1 = d * a, m1 = mn * c; \
            k_scale_min(is + 1, sc, &a, &c); float d2 = d * a, m2 = mn * c; \
            for (int l = 0; l < 32; l++) { y[l] = d1 * (float)((q[l] & 15) + (qh[l] & u1 ? 16 : 0)) - m1; y[l + 32] = d2 * (float)((q[l] >> 4) + (qh[l] & u2 ? 16 : 0)) - m2; } y += 64; } } break; \
    case GT_Q6_K: for (int b = 0; b < n / 256; b++, s += 210) { const uint8_t *ql = s, *qh = s + 128; const int8_t *sc = (const int8_t *)(s + 192); float d = h2f(le16(s + 208)); \
        for (int h = 0; h < 2; h++, ql += 64, qh += 32, sc += 8, y += 128) for (int l = 0; l < 32; l++) { int is = l / 16; \
            y[l] = d * sc[is] * (float)((int)((ql[l] & 15) | ((qh[l] & 3) << 4)) - 32); \
            y[l + 32] = d * sc[is + 2] * (float)((int)((ql[l + 32] & 15) | (((qh[l] >> 2) & 3) << 4)) - 32); \
            y[l + 64] = d * sc[is + 4] * (float)((int)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32); \
            y[l + 96] = d * sc[is + 6] * (float)((int)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32); } } break; \
    }
static void llm_dq_c(int t, const uint8_t *s, float *y, int n) { LLM_DQ_BODY }
__attribute__((target("avx2,fma"))) static void llm_dq_avx2(int t, const uint8_t *s, float *y, int n) { LLM_DQ_BODY }
static float llm_dot_c(const float *a, const float *b, int n) {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int i = 0;
    for (; i + 4 <= n; i += 4) { s0 += a[i] * b[i]; s1 += a[i + 1] * b[i + 1]; s2 += a[i + 2] * b[i + 2]; s3 += a[i + 3] * b[i + 3]; }
    for (; i < n; i++) s0 += a[i] * b[i];
    return s0 + s1 + s2 + s3;
}
__attribute__((target("avx2,fma"))) static float llm_dot_avx2(const float *a, const float *b, int n) {
    __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps(), s2 = _mm256_setzero_ps(), s3 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
        s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), s1);
        s2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), s2);
        s3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), s3);
    }
    for (; i + 8 <= n; i += 8) s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
    __m256 s = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h)); h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1));
    float r = _mm_cvtss_f32(h);
    for (; i < n; i++) r += a[i] * b[i];
    return r;
}
__attribute__((target("xsave"))) static int llm_has_avx2(void) {
    int r[4];
    __cpuid(r, 1);
    if (!(r[2] & (1 << 12)) || !(r[2] & (1 << 27)) || !(r[2] & (1 << 28))) return 0;          /* FMA, OSXSAVE, AVX */
    if ((_xgetbv(0) & 6) != 6) return 0;                                                         /* the OS saves the AVX registers */
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;                                                                      /* AVX2 */
}

/* ---- threads: worker t runs job(t, nt); the caller is worker 0 ---- */
typedef struct { Llm *m; int t; } LlmW;
static DWORD WINAPI llm_worker(void *p) {
    LlmW w = *(LlmW *)p; free(p);
    for (;;) {
        WaitForSingleObject(w.m->go[w.t], INFINITE);
        if (w.m->stop) return 0;
        w.m->job(w.m, w.m->job_ctx, w.t, w.m->nt);
        SetEvent(w.m->done[w.t]);
    }
}
static void llm_run(Llm *m, LlmJob fn, void *ctx) {
    m->job = fn; m->job_ctx = ctx;
    for (int t = 1; t < m->nt; t++) SetEvent(m->go[t]);
    fn(m, ctx, 0, m->nt);
    if (m->nt > 1) WaitForMultipleObjects((DWORD)(m->nt - 1), m->done + 1, TRUE, INFINITE);
}
/* out[b][r] = W[r] . x[b] for B inputs; rows split across the workers. */
typedef struct { float *out; const LlmT *w; const float *x; int B, n_in, n_out; } LlmMm;
static void llm_mm_job(Llm *m, void *ctx, int t, int nt) {
    LlmMm *j = ctx;
    int r0 = (int)((int64_t)j->n_out * t / nt), r1 = (int)((int64_t)j->n_out * (t + 1) / nt);
    float *buf = m->rowbuf + (size_t)t * (size_t)m->rowmax;
    size_t rs = gt_row(j->w->type, j->n_in);
    for (int r = r0; r < r1; r++) {
        const uint8_t *src = j->w->data + rs * (size_t)r;
        if (m->avx2) llm_dq_avx2(j->w->type, src, buf, j->n_in); else llm_dq_c(j->w->type, src, buf, j->n_in);
        for (int b = 0; b < j->B; b++)
            j->out[(size_t)b * (size_t)j->n_out + (size_t)r] = m->avx2 ? llm_dot_avx2(buf, j->x + (size_t)b * (size_t)j->n_in, j->n_in) : llm_dot_c(buf, j->x + (size_t)b * (size_t)j->n_in, j->n_in);
    }
}
static void llm_mm(Llm *m, float *out, const LlmT *w, const float *x, int B) {
    LlmMm j = {out, w, x, B, (int)w->ne[0], (int)w->ne[1]};
    llm_run(m, llm_mm_job, &j);
}
static void llm_add_bias(float *y, const LlmT *bias, int B, int n) {
    if (!bias->data) return;
    for (int b = 0; b < B; b++) for (int i = 0; i < n; i++) { float v; memcpy(&v, bias->data + 4 * (size_t)i, 4); y[(size_t)b * n + i] += v; }
}
static void llm_rmsnorm(float *o, const float *x, const LlmT *w, int n, float eps) {
    double ss = 0;
    for (int i = 0; i < n; i++) ss += (double)x[i] * x[i];
    float r = 1.0f / sqrtf((float)(ss / n) + eps);
    for (int i = 0; i < n; i++) { float wi; memcpy(&wi, w->data + 4 * (size_t)i, 4); o[i] = x[i] * r * wi; }
}
/* Attention for B new tokens at n_past.. over the cache, causal; (token, head) pairs split across the workers. */
typedef struct { int layer, B; } LlmAtt;
static void llm_att_job(Llm *m, void *ctx, int t, int nt) {
    LlmAtt *a = ctx;
    int hd = m->hd, kvd = m->n_kv * hd, grp = m->n_head / m->n_kv, total = a->B * m->n_head;
    float *sc = m->att + (size_t)t * (size_t)m->n_ctx, scale = 1.0f / sqrtf((float)hd);
    const float *K = m->kc + (size_t)a->layer * (size_t)m->n_ctx * (size_t)kvd, *V = m->vc + (size_t)a->layer * (size_t)m->n_ctx * (size_t)kvd;
    for (int w = total * t / nt; w < total * (t + 1) / nt; w++) {
        int b = w / m->n_head, h = w % m->n_head, kh = h / grp, n = m->n_past + b + 1;
        const float *q = m->q + (size_t)b * (size_t)(m->n_head * hd) + (size_t)h * hd;
        float mx = -1e30f, sum = 0;
        for (int p = 0; p < n; p++) { sc[p] = (m->avx2 ? llm_dot_avx2(q, K + (size_t)p * kvd + (size_t)kh * hd, hd) : llm_dot_c(q, K + (size_t)p * kvd + (size_t)kh * hd, hd)) * scale; if (sc[p] > mx) mx = sc[p]; }
        for (int p = 0; p < n; p++) { sc[p] = expf(sc[p] - mx); sum += sc[p]; }
        float *o = m->ao + (size_t)b * (size_t)(m->n_head * hd) + (size_t)h * hd;
        memset(o, 0, sizeof(float) * (size_t)hd);
        for (int p = 0; p < n; p++) { float wgt = sc[p] / sum; const float *vv = V + (size_t)p * kvd + (size_t)kh * hd; for (int i = 0; i < hd; i++) o[i] += wgt * vv[i]; }
    }
}
static void llm_rope(Llm *m, float *v, int heads, int pos) {
    int half = m->rope_dim / 2;
    for (int i = 0; i < half; i++) {
        float a = (float)pos * m->inv_freq[i], c = cosf(a), s = sinf(a);
        for (int h = 0; h < heads; h++) {
            float *x = v + (size_t)h * m->hd;
            int i0 = m->neox ? i : 2 * i, i1 = m->neox ? i + half : 2 * i + 1;
            float x0 = x[i0], x1 = x[i1];
            x[i0] = x0 * c - x1 * s; x[i1] = x0 * s + x1 * c;
        }
    }
}
/* Feed n tokens (batches of LLM_BATCH); the logits of the last one are left in m->logits. 0 = done, -1 = no room. */
static int llm_eval(Llm *m, const int32_t *tok, int n) {
    int E = m->n_embd, kvd = m->n_kv * m->hd, qd = m->n_head * m->hd;
    while (n > 0) {
        int B = n < LLM_BATCH ? n : LLM_BATCH;
        if (m->n_past + B > m->n_ctx) return -1;
        for (int b = 0; b < B; b++) {
            int id = tok[b] >= 0 && tok[b] < m->n_vocab ? tok[b] : 0;
            const uint8_t *row = m->tok_embd.data + gt_row(m->tok_embd.type, E) * (size_t)id;
            if (m->avx2) llm_dq_avx2(m->tok_embd.type, row, m->x + (size_t)b * E, E); else llm_dq_c(m->tok_embd.type, row, m->x + (size_t)b * E, E);
        }
        for (int l = 0; l < m->n_layer; l++) {
            LlmLayer *L = &m->L[l];
            for (int b = 0; b < B; b++) llm_rmsnorm(m->xb + (size_t)b * E, m->x + (size_t)b * E, &L->attn_norm, E, m->eps);
            llm_mm(m, m->q, &L->wq, m->xb, B); llm_add_bias(m->q, &L->bq, B, qd);
            llm_mm(m, m->k, &L->wk, m->xb, B); llm_add_bias(m->k, &L->bk, B, kvd);
            llm_mm(m, m->v, &L->wv, m->xb, B); llm_add_bias(m->v, &L->bv, B, kvd);
            for (int b = 0; b < B; b++) {
                llm_rope(m, m->q + (size_t)b * qd, m->n_head, m->n_past + b);
                llm_rope(m, m->k + (size_t)b * kvd, m->n_kv, m->n_past + b);
                size_t at = ((size_t)l * (size_t)m->n_ctx + (size_t)(m->n_past + b)) * (size_t)kvd;
                memcpy(m->kc + at, m->k + (size_t)b * kvd, sizeof(float) * (size_t)kvd);
                memcpy(m->vc + at, m->v + (size_t)b * kvd, sizeof(float) * (size_t)kvd);
            }
            LlmAtt aj = {l, B};
            llm_run(m, llm_att_job, &aj);
            llm_mm(m, m->xb, &L->wo, m->ao, B);
            for (size_t i = 0; i < (size_t)B * E; i++) m->x[i] += m->xb[i];
            for (int b = 0; b < B; b++) llm_rmsnorm(m->xb + (size_t)b * E, m->x + (size_t)b * E, &L->ffn_norm, E, m->eps);
            llm_mm(m, m->g, &L->w_gate, m->xb, B);
            llm_mm(m, m->u, &L->w_up, m->xb, B);
            for (size_t i = 0; i < (size_t)B * m->n_ff; i++) m->g[i] = m->g[i] / (1.0f + expf(-m->g[i])) * m->u[i];
            llm_mm(m, m->xb, &L->w_down, m->g, B);
            for (size_t i = 0; i < (size_t)B * E; i++) m->x[i] += m->xb[i];
        }
        m->n_past += B; tok += B; n -= B;
        if (n == 0) {
            llm_rmsnorm(m->xb, m->x + (size_t)(B - 1) * E, &m->out_norm, E, m->eps);
            llm_mm(m, m->logits, &m->output, m->xb, 1);
        }
    }
    return 0;
}
static void llm_rewind(Llm *m, int n_past) { if (n_past >= 0 && n_past <= m->n_past) m->n_past = n_past; }
/* The next token: repetition penalty on the recent ones, then temperature, top-k and top-p. */
static int llm_sample(Llm *m, float temp, int top_k, float top_p, float rep, const int32_t *recent, int nrec, uint32_t *rng) {
    enum { K = 64 };
    int idx[K], n = 0;
    float val[K];
    if (top_k > K) top_k = K;
    for (int i = 0; i < nrec; i++) { int t = recent[i]; if (t >= 0 && t < m->n_vocab) m->logits[t] = m->logits[t] > 0 ? m->logits[t] / rep : m->logits[t] * rep; }
    for (int i = 0; i < m->n_vocab; i++) {                     /* the top_k largest, sorted */
        float l = m->logits[i];
        if (n == top_k && l <= val[n - 1]) continue;
        int j = n < top_k ? n++ : n - 1;
        while (j > 0 && val[j - 1] < l) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; j--; }
        val[j] = l; idx[j] = i;
    }
    if (temp <= 0 || n == 1) return idx[0];
    float p[K], sum = 0, acc = 0;
    for (int i = 0; i < n; i++) { p[i] = expf((val[i] - val[0]) / temp); sum += p[i]; }
    int keep = n;
    for (int i = 0; i < n; i++) { acc += p[i] / sum; if (acc >= top_p) { keep = i + 1; break; } }
    sum = 0;
    for (int i = 0; i < keep; i++) sum += p[i];
    *rng ^= *rng << 13; *rng ^= *rng >> 17; *rng ^= *rng << 5;
    float r = (float)(*rng >> 8) / 16777216.0f * sum;
    for (int i = 0; i < keep; i++) { r -= p[i]; if (r <= 0) return idx[i]; }
    return idx[keep - 1];
}

/* ---- tokenizer: GPT-2 byte-level BPE ---- */
static int g_b2u[256], g_u2b[512];
static void llm_bytes_init(void) {
    static int done;
    if (done++) return;
    int n = 0;
    for (int b = 0; b < 256; b++) { int keep = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174); g_b2u[b] = keep ? b : 256 + n++; }
    for (int i = 0; i < 512; i++) g_u2b[i] = -1;
    for (int b = 0; b < 256; b++) g_u2b[g_b2u[b]] = b;
}
static uint32_t llm_fnv(const char *s, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; } return h; }
static uint32_t *llm_hash_build(const LlmStr *keys, int n, uint32_t *mask) {
    uint32_t size = 1;
    while (size < (uint32_t)n * 2u) size <<= 1;
    uint32_t *h = calloc(size, sizeof *h);
    if (!h) return NULL;
    for (int i = 0; i < n; i++) {
        uint32_t at = llm_fnv(keys[i].s, keys[i].n) & (size - 1);
        while (h[at]) at = (at + 1) & (size - 1);
        h[at] = (uint32_t)i + 1;
    }
    *mask = size - 1;
    return h;
}
static int llm_hash_find(const uint32_t *h, uint32_t mask, const LlmStr *keys, const char *s, uint32_t n) {
    for (uint32_t at = llm_fnv(s, n) & mask; h[at]; at = (at + 1) & mask) {
        const LlmStr *k = &keys[h[at] - 1];
        if (k->n == n && !memcmp(k->s, s, n)) return (int)h[at] - 1;
    }
    return -1;
}
static int llm_isl(int c) { return isalpha(c) || c >= 0x80; }
static int llm_isn(int c) { return c >= '0' && c <= '9'; }
static int llm_iss(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
/* How long the pre-tokenizer's piece at s[i] is: (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}(or {1,3})|
 *  ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+ (bytes >= 0x80 count as letters). */
static int llm_piece_len(const char *s, int n, int i, int digits3) {
    int c = (uint8_t)s[i];
    if (c == '\'' && i + 1 < n) {
        int a = tolower((uint8_t)s[i + 1]), b = i + 2 < n ? tolower((uint8_t)s[i + 2]) : 0;
        if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) return 3;
        if (a == 's' || a == 't' || a == 'm' || a == 'd') return 2;
    }
    {   int j = i;
        if (!llm_isl(c) && !llm_isn(c) && c != '\r' && c != '\n') j++;
        if (j < n && llm_isl((uint8_t)s[j])) { while (j < n && llm_isl((uint8_t)s[j])) j++; return j - i; } }
    if (llm_isn(c)) { int j = i + 1; while (digits3 && j < n && j - i < 3 && llm_isn((uint8_t)s[j])) j++; return j - i; }
    {   int j = i + (c == ' '), k = j;
        while (k < n && !llm_iss((uint8_t)s[k]) && !llm_isl((uint8_t)s[k]) && !llm_isn((uint8_t)s[k])) k++;
        if (k > j) { while (k < n && (s[k] == '\r' || s[k] == '\n')) k++; return k - i; } }
    int j = i, last = -1;
    while (j < n && llm_iss((uint8_t)s[j])) { if (s[j] == '\r' || s[j] == '\n') last = j; j++; }
    if (last >= 0) return last + 1 - i;
    if (j > i) return j == n ? j - i : j - i >= 2 ? j - 1 - i : 1;
    return 1;
}
/* One piece by BPE: each byte as its mapped character, then the lowest-ranked merge of neighbours until none is left. */
static int llm_bpe(Llm *m, const char *s, int n, int32_t *out, int max) {
    char buf[512], key[520];
    int st[256], ln[256], ns = 0, o = 0, bl = 0;
    if (n > 128) n = 128;
    for (int i = 0; i < n; i++) {
        int u = g_b2u[(uint8_t)s[i]];
        st[ns] = bl;
        if (u < 0x80) buf[bl++] = (char)u; else { buf[bl++] = (char)(0xC0 | (u >> 6)); buf[bl++] = (char)(0x80 | (u & 0x3F)); }
        ln[ns] = bl - st[ns]; ns++;
    }
    while (ns > 1) {
        int best = -1, rank = 0x7fffffff;
        for (int i = 0; i + 1 < ns; i++) {
            int kl = ln[i] + 1 + ln[i + 1];
            memcpy(key, buf + st[i], (size_t)ln[i]); key[ln[i]] = ' '; memcpy(key + ln[i] + 1, buf + st[i + 1], (size_t)ln[i + 1]);
            int r = llm_hash_find(m->mh, m->mh_mask, m->merges, key, (uint32_t)kl);
            if (r >= 0 && r < rank) { rank = r; best = i; }
        }
        if (best < 0) break;
        ln[best] += ln[best + 1];
        memmove(st + best + 1, st + best + 2, sizeof(int) * (size_t)(ns - best - 2)); memmove(ln + best + 1, ln + best + 2, sizeof(int) * (size_t)(ns - best - 2));
        ns--;
    }
    for (int i = 0; i < ns && o < max; i++) {
        int id = llm_hash_find(m->vh, m->vh_mask, m->vocab, buf + st[i], (uint32_t)ln[i]);
        if (id >= 0) out[o++] = id;
    }
    return o;
}
static int llm_tok(Llm *m, const char *text, int32_t *out, int max) {
    int n = (int)strlen(text), o = 0;
    for (int i = 0; i < n && o < max;) {
        int l = llm_piece_len(text, n, i, m->digits3);
        o += llm_bpe(m, text + i, l, out + o, max - o);
        i += l;
    }
    return o;
}
/* A token as text (UTF-8); control tokens give nothing. */
static int llm_piece(Llm *m, int id, char *out, int n) {
    if (id < 0 || id >= m->n_vocab || n < 1) return 0;
    if (m->ttype && m->ttype_kind == 5) { int32_t t; memcpy(&t, m->ttype + 4 * (size_t)id, 4); if (t == 3) { out[0] = 0; return 0; } }
    const uint8_t *s = (const uint8_t *)m->vocab[id].s;
    int o = 0;
    for (uint32_t i = 0; i < m->vocab[id].n && o < n - 1;) {
        uint32_t cp = s[i++];
        if (cp >= 0xC0 && i < m->vocab[id].n) { cp = ((cp & 0x1F) << 6) | (s[i++] & 0x3F); }
        out[o++] = cp < 512 && g_u2b[cp] >= 0 ? (char)g_u2b[cp] : '?';
    }
    out[o] = 0;
    return o;
}

/* ---- the file (GGUF 2 / 3) ---- */
typedef struct { const uint8_t *p, *e; int bad; } LlmRd;
static uint64_t llm_u(LlmRd *r, int n) { uint64_t v = 0; if (r->p + n > r->e) { r->bad = 1; return 0; } memcpy(&v, r->p, (size_t)n); r->p += n; return v; }
static LlmStr llm_s(LlmRd *r) {
    LlmStr s = {0};
    uint64_t n = llm_u(r, 8);
    if (r->bad || n > (uint64_t)(r->e - r->p)) { r->bad = 1; return s; }
    s.s = (const char *)r->p; s.n = (uint32_t)n; r->p += n;
    return s;
}
static int llm_vsize(uint32_t t) { static const int z[13] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8}; return t < 13 ? z[t] : 0; }
static void llm_skip(LlmRd *r, uint32_t t) {
    if (t == 8) { llm_s(r); return; }
    if (t == 9) {
        uint32_t et = (uint32_t)llm_u(r, 4); uint64_t n = llm_u(r, 8);
        if (et != 8 && et != 9 && llm_vsize(et)) { uint64_t b = n * (uint64_t)llm_vsize(et); if (b > (uint64_t)(r->e - r->p)) r->bad = 1; else r->p += b; return; }
        for (uint64_t i = 0; i < n && !r->bad; i++) llm_skip(r, et);
        return;
    }
    int z = llm_vsize(t);
    if (!z || r->p + z > r->e) r->bad = 1; else r->p += z;
}
static double llm_num(LlmRd *r, uint32_t t) {
    uint64_t v = llm_u(r, llm_vsize(t) ? llm_vsize(t) : 1);
    switch (t) {
    case 1: return (int8_t)v; case 3: return (int16_t)v; case 5: return (int32_t)v; case 11: return (double)(int64_t)v;
    case 6: { float f; uint32_t w = (uint32_t)v; memcpy(&f, &w, 4); return f; }
    case 12: { double d; memcpy(&d, &v, 8); return d; }
    }
    return (double)v;
}
static int llm_key(LlmStr k, const char *s) { size_t n = strlen(s); return k.n == n && !memcmp(k.s, s, n); }
static int llm_keyend(LlmStr k, const char *s) { size_t n = strlen(s); return k.n > n && !memcmp(k.s + k.n - n, s, n); }
static LlmStr *llm_strs(LlmRd *r, int *count) {     /* an array of strings (after its type) */
    uint32_t et = (uint32_t)llm_u(r, 4); uint64_t n = llm_u(r, 8);
    if (et != 8 || n > 10000000) { r->bad = 1; return NULL; }
    LlmStr *a = malloc(sizeof *a * (size_t)(n ? n : 1));
    if (!a) { r->bad = 1; return NULL; }
    for (uint64_t i = 0; i < n && !r->bad; i++) a[i] = llm_s(r);
    *count = (int)n;
    return a;
}
static void llm_close(Llm *m) {
    if (!m) return;
    if (m->nt > 1) {
        m->stop = 1;
        for (int t = 1; t < m->nt; t++) SetEvent(m->go[t]);
        WaitForMultipleObjects((DWORD)(m->nt - 1), m->th + 1, TRUE, 5000);
    }
    for (int t = 1; t < m->nt; t++) { CloseHandle(m->th[t]); CloseHandle(m->go[t]); CloseHandle(m->done[t]); }
    free(m->L); free(m->vocab); free(m->merges); free(m->vh); free(m->mh);
    free(m->kc); free(m->vc); free(m->logits); free(m->inv_freq);
    free(m->x); free(m->xb); free(m->q); free(m->k); free(m->v); free(m->ao); free(m->g); free(m->u); free(m->att); free(m->rowbuf);
    if (m->base) UnmapViewOfFile(m->base);
    if (m->map) CloseHandle(m->map);
    if (m->file && m->file != INVALID_HANDLE_VALUE) CloseHandle(m->file);
    free(m);
}
static Llm *llm_fail(Llm *m, char *err, size_t en, const char *why) { snprintf(err, en, "%s", why); llm_close(m); return NULL; }
static Llm *llm_open(const char *path, int nt, int n_ctx, char *err, size_t en) {
    Llm *m = calloc(1, sizeof *m);
    if (!m) return llm_fail(m, err, en, "out of memory");
    llm_bytes_init();
    m->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m->file == INVALID_HANDLE_VALUE) return llm_fail(m, err, en, "cannot open the model file");
    LARGE_INTEGER fs; GetFileSizeEx(m->file, &fs); m->size = (uint64_t)fs.QuadPart;
    m->map = CreateFileMappingA(m->file, NULL, PAGE_READONLY, 0, 0, NULL);
    m->base = m->map ? MapViewOfFile(m->map, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!m->base || m->size < 32) return llm_fail(m, err, en, "cannot map the model file");
    LlmRd r = {m->base, m->base + m->size, 0};
    if (llm_u(&r, 4) != 0x46554747u) return llm_fail(m, err, en, "not a GGUF file");
    uint32_t ver = (uint32_t)llm_u(&r, 4);
    if (ver < 2 || ver > 3) return llm_fail(m, err, en, "unsupported GGUF version");
    uint64_t nten = llm_u(&r, 8), nkv = llm_u(&r, 8);
    uint32_t align = 32;
    char arch[32] = "", tmodel[32] = "", pre[32] = "";
    m->eps = 1e-6f; m->theta = 10000.0f; m->eos = -1;
    int nvoc = 0;
    for (uint64_t i = 0; i < nkv && !r.bad; i++) {
        LlmStr k = llm_s(&r);
        uint32_t t = (uint32_t)llm_u(&r, 4);
        if (r.bad) break;
        if (t == 8 && (llm_key(k, "general.architecture") || llm_key(k, "tokenizer.ggml.model") || llm_key(k, "tokenizer.ggml.pre"))) {
            LlmStr v = llm_s(&r);
            char *dst = llm_key(k, "general.architecture") ? arch : llm_key(k, "tokenizer.ggml.model") ? tmodel : pre;
            snprintf(dst, 32, "%.*s", (int)(v.n < 31 ? v.n : 31), v.s);
        } else if (t == 9 && llm_key(k, "tokenizer.ggml.tokens")) m->vocab = llm_strs(&r, &nvoc);
        else if (t == 9 && llm_key(k, "tokenizer.ggml.merges")) m->merges = llm_strs(&r, &m->n_merges);
        else if (t == 9 && llm_key(k, "tokenizer.ggml.token_type")) {
            uint32_t et = (uint32_t)llm_u(&r, 4); uint64_t n = llm_u(&r, 8);
            m->ttype = r.p; m->ttype_kind = (int)et;
            if (!llm_vsize(et) || n * (uint64_t)llm_vsize(et) > (uint64_t)(r.e - r.p)) r.bad = 1; else r.p += n * (uint64_t)llm_vsize(et);
        } else if (t != 8 && t != 9) {
            double v = llm_num(&r, t);
            if (llm_key(k, "general.alignment")) align = (uint32_t)v;
            else if (llm_key(k, "tokenizer.ggml.eos_token_id")) m->eos = (int)v;
            else if (llm_keyend(k, ".context_length")) m->n_ctx = (int)v;
            else if (llm_keyend(k, ".embedding_length")) m->n_embd = (int)v;
            else if (llm_keyend(k, ".block_count")) m->n_layer = (int)v;
            else if (llm_keyend(k, ".feed_forward_length")) m->n_ff = (int)v;
            else if (llm_keyend(k, ".attention.head_count")) m->n_head = (int)v;
            else if (llm_keyend(k, ".attention.head_count_kv")) m->n_kv = (int)v;
            else if (llm_keyend(k, ".rope.freq_base")) m->theta = (float)v;
            else if (llm_keyend(k, ".attention.layer_norm_rms_epsilon")) m->eps = (float)v;
            else if (llm_keyend(k, ".rope.dimension_count")) m->rope_dim = (int)v;
        } else llm_skip(&r, t);
    }
    if (r.bad) return llm_fail(m, err, en, "the model file's settings are damaged");
    if (strcmp(arch, "qwen2") && strcmp(arch, "llama")) return llm_fail(m, err, en, "only Qwen2 and Llama models are supported");
    if (strcmp(tmodel, "gpt2") || !m->vocab || !m->merges) return llm_fail(m, err, en, "only byte-level BPE (gpt2) tokenizers are supported");
    m->neox = !strcmp(arch, "qwen2");
    m->digits3 = !strcmp(pre, "llama-bpe") || !strcmp(pre, "smaug-bpe");
    if (!m->n_kv) m->n_kv = m->n_head;
    if (m->n_embd <= 0 || m->n_layer <= 0 || m->n_head <= 0 || m->n_ff <= 0 || m->n_embd % m->n_head || m->n_head % m->n_kv) return llm_fail(m, err, en, "the model's shape is missing");
    m->hd = m->n_embd / m->n_head;
    if (!m->rope_dim) m->rope_dim = m->hd;
    LlmStr *tn = malloc(sizeof *tn * (size_t)(nten ? nten : 1));
    LlmT *tt = malloc(sizeof *tt * (size_t)(nten ? nten : 1));
    uint64_t *toff = malloc(sizeof *toff * (size_t)(nten ? nten : 1));
    if (!tn || !tt || !toff) { free(tn); free(tt); free(toff); return llm_fail(m, err, en, "out of memory"); }
    for (uint64_t i = 0; i < nten && !r.bad; i++) {
        tn[i] = llm_s(&r);
        uint32_t nd = (uint32_t)llm_u(&r, 4);
        tt[i].ne[0] = tt[i].ne[1] = tt[i].ne[2] = tt[i].ne[3] = 1;
        for (uint32_t d = 0; d < nd; d++) { uint64_t v = llm_u(&r, 8); if (d < 4) tt[i].ne[d] = (int64_t)v; }
        tt[i].type = (int)llm_u(&r, 4);
        toff[i] = llm_u(&r, 8);
    }
    uint64_t data = ((uint64_t)(r.p - m->base) + align - 1) / align * align;
    int ok = !r.bad;
    for (uint64_t i = 0; i < nten && ok; i++) {
        int bs = gt_bytes(tt[i].type);
        uint64_t sz = bs ? (uint64_t)gt_row(tt[i].type, tt[i].ne[0]) * (uint64_t)(tt[i].ne[1] * tt[i].ne[2] * tt[i].ne[3]) : 0;
        if (!bs || tt[i].ne[0] % gt_blck(tt[i].type)) tt[i].data = NULL;                   /* (a type this does not read: fine unless it is used) */
        else if (data + toff[i] + sz > m->size) ok = 0;
        else tt[i].data = m->base + data + toff[i];
    }
    free(toff);
    #define LLM_FIND(dst, nm) do { (dst).data = NULL; for (uint64_t ii = 0; ii < nten; ii++) if (llm_key(tn[ii], nm)) { (dst) = tt[ii]; break; } } while (0)
    char nm[64];
    if (ok) {
        LLM_FIND(m->tok_embd, "token_embd.weight"); LLM_FIND(m->out_norm, "output_norm.weight"); LLM_FIND(m->output, "output.weight");
        if (!m->output.data) m->output = m->tok_embd;                                         /* tied embeddings */
        m->L = calloc((size_t)m->n_layer, sizeof *m->L);
        for (int l = 0; l < m->n_layer && m->L; l++) {
            LlmLayer *L = &m->L[l];
            #define LLM_LAYER(f, s) do { snprintf(nm, sizeof nm, "blk.%d.%s", l, s); LLM_FIND(L->f, nm); } while (0)
            LLM_LAYER(attn_norm, "attn_norm.weight"); LLM_LAYER(wq, "attn_q.weight"); LLM_LAYER(wk, "attn_k.weight"); LLM_LAYER(wv, "attn_v.weight");
            LLM_LAYER(wo, "attn_output.weight"); LLM_LAYER(bq, "attn_q.bias"); LLM_LAYER(bk, "attn_k.bias"); LLM_LAYER(bv, "attn_v.bias");
            LLM_LAYER(ffn_norm, "ffn_norm.weight"); LLM_LAYER(w_gate, "ffn_gate.weight"); LLM_LAYER(w_up, "ffn_up.weight"); LLM_LAYER(w_down, "ffn_down.weight");
            int kvd = m->n_kv * m->hd;
            if (!L->attn_norm.data || !L->wq.data || !L->wk.data || !L->wv.data || !L->wo.data || !L->ffn_norm.data || !L->w_gate.data || !L->w_up.data || !L->w_down.data
                || L->attn_norm.type != GT_F32 || L->ffn_norm.type != GT_F32 || (L->bq.data && L->bq.type != GT_F32) || (L->bk.data && L->bk.type != GT_F32) || (L->bv.data && L->bv.type != GT_F32)
                || L->wq.ne[0] != m->n_embd || L->wq.ne[1] != m->n_embd || L->wk.ne[0] != m->n_embd || L->wk.ne[1] != kvd || L->wv.ne[1] != kvd
                || L->wo.ne[0] != m->n_embd || L->w_gate.ne[1] != m->n_ff || L->w_up.ne[1] != m->n_ff || L->w_down.ne[0] != m->n_ff || L->w_down.ne[1] != m->n_embd) ok = 0;
        }
        if (!m->L || !m->tok_embd.data || !m->out_norm.data || m->out_norm.type != GT_F32 || m->tok_embd.ne[0] != m->n_embd || m->output.ne[0] != m->n_embd) ok = 0;
    }
    free(tn); free(tt);
    if (!ok) return llm_fail(m, err, en, "the model's weights are missing, damaged or of a kind this cannot read");
    m->n_vocab = (int)m->output.ne[1];
    if (nvoc < m->n_vocab) m->n_vocab = nvoc;
    m->vh = llm_hash_build(m->vocab, nvoc, &m->vh_mask);
    m->mh = llm_hash_build(m->merges, m->n_merges, &m->mh_mask);
    if (!m->vh || !m->mh) return llm_fail(m, err, en, "out of memory");
    m->im_start = llm_hash_find(m->vh, m->vh_mask, m->vocab, "<|im_start|>", 12);
    m->im_end = llm_hash_find(m->vh, m->vh_mask, m->vocab, "<|im_end|>", 10);
    m->endoftext = llm_hash_find(m->vh, m->vh_mask, m->vocab, "<|endoftext|>", 13);
    m->n_ctx = n_ctx < m->n_ctx || !m->n_ctx ? n_ctx : m->n_ctx;
    int kvd = m->n_kv * m->hd, qd = m->n_head * m->hd;
    m->avx2 = llm_has_avx2();
    m->nt = nt < 1 ? 1 : nt > LLM_MAXT ? LLM_MAXT : nt;
    m->rowmax = m->n_ff > m->n_embd ? m->n_ff : m->n_embd;
    size_t B = LLM_BATCH;
    m->kc = calloc((size_t)m->n_layer * (size_t)m->n_ctx * (size_t)kvd, sizeof(float));
    m->vc = calloc((size_t)m->n_layer * (size_t)m->n_ctx * (size_t)kvd, sizeof(float));
    m->logits = malloc(sizeof(float) * (size_t)m->output.ne[1]);
    m->inv_freq = malloc(sizeof(float) * (size_t)(m->rope_dim / 2 + 1));
    m->x = malloc(sizeof(float) * B * (size_t)m->n_embd); m->xb = malloc(sizeof(float) * B * (size_t)m->n_embd);
    m->q = malloc(sizeof(float) * B * (size_t)qd); m->k = malloc(sizeof(float) * B * (size_t)kvd); m->v = malloc(sizeof(float) * B * (size_t)kvd);
    m->ao = malloc(sizeof(float) * B * (size_t)qd); m->g = malloc(sizeof(float) * B * (size_t)m->n_ff); m->u = malloc(sizeof(float) * B * (size_t)m->n_ff);
    m->att = malloc(sizeof(float) * (size_t)m->nt * (size_t)m->n_ctx); m->rowbuf = malloc(sizeof(float) * (size_t)m->nt * (size_t)m->rowmax);
    if (!m->kc || !m->vc || !m->logits || !m->inv_freq || !m->x || !m->xb || !m->q || !m->k || !m->v || !m->ao || !m->g || !m->u || !m->att || !m->rowbuf)
        return llm_fail(m, err, en, "out of memory");
    for (int i = 0; i < m->rope_dim / 2; i++) m->inv_freq[i] = powf(m->theta, -2.0f * (float)i / (float)m->rope_dim);
    for (int t = 1; t < m->nt; t++) {
        LlmW *w = malloc(sizeof *w);
        m->go[t] = CreateEventA(NULL, FALSE, FALSE, NULL); m->done[t] = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!w || !m->go[t] || !m->done[t]) { free(w); m->nt = t; break; }
        w->m = m; w->t = t;
        m->th[t] = CreateThread(NULL, 0, llm_worker, w, 0, NULL);
        if (!m->th[t]) { free(w); CloseHandle(m->go[t]); CloseHandle(m->done[t]); m->nt = t; break; }
        SetThreadPriority(m->th[t], THREAD_PRIORITY_BELOW_NORMAL);
    }
    return m;
}
