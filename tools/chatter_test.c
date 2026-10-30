/* chatter_test.c <userdata folder>: the Mii chatter mod (runtime/mii_chatter.c) without the game: its self-test, what it
 * learns from the save in <userdata>\save, and sample lines of both kinds. Point it at a COPY of a userdata folder (it
 * writes chatter_memory.txt there). With a model (.gguf) as the second argument: its sanity check (a one-word answer), the
 * tokenizer, the speed, and lines the AI writes (TOMO_LLM_THREADS, CHATTER_LINES = how many).
 * Build: clang -O2 -w -I runtime -o chatter_test.exe tools/chatter_test.c */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
typedef struct { uint64_t x[32]; } Ctx;
typedef void (*ImportFn)(Ctx *c);
static inline uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static inline uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static inline uint16_t rd16(uint64_t a) { uint16_t v; memcpy(&v, (void *)(uintptr_t)a, 2); return v; }
static inline uint8_t rd8(uint64_t a) { return *(uint8_t *)(uintptr_t)a; }
static inline void wr64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static inline void wr32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static struct { uint64_t text_size; } g_main = {0x256d250};
static int hook_install(uint64_t a, void (*r)(Ctx *, void *), void *u, ImportFn *o) { (void)a; (void)r; (void)u; (void)o; return 1; }
int gfx_mii_chatter(void) { return 1; }
int gfx_mii_chat_english(void) { return 1; }
static char g_root[MAX_PATH];
const char *fs_user_root(void) { return g_root; }
#include "mii_chatter.c"

static void show(const char *l8) {
    uint16_t o[320];
    int n = chatter_encode(l8, o, 320), tags = 0;
    for (int k = 0; k < n; k++) if (o[k] == 0x0E) { tags++; k += 3 + o[k + 3] / 2; }
    printf("  [%3d units, %d tags] %s\n", n, tags, l8);
}
int main(int argc, char **argv) {
    snprintf(g_root, sizeof g_root, "%s", argc > 1 ? argv[1] : ".");
    printf("self-test: %s\n", chatter_selftest() ? "ok" : "FAILED");
    cm_learn();
    kb_load_file();
    if (getenv("CHATTER_SARC")) {                       /* a decompressed message archive (romfs/Mals/USen.Product.100.sarc), as if in the game's memory */
        FILE *f = fopen(getenv("CHATTER_SARC"), "rb");
        static uint8_t *sarc; long sz = 0;
        if (f && !fseek(f, 0, SEEK_END) && (sz = ftell(f)) > 0x100 && !fseek(f, 0, SEEK_SET) && (sarc = malloc((size_t)sz)) && fread(sarc, 1, (size_t)sz, f) == (size_t)sz) kb_from_game((uint64_t)(uintptr_t)(sarc + 0x100));
        if (f) fclose(f);
    }
    printf("island \"%s\", %d known Miis, %d words, %d facts about the game\n", g_cm.island, g_cm.nmii, g_cm.nword, g_nkb);
    for (int i = 0; i < g_cm.nmii; i++) {
        char who[400]; ai_person(g_cm.mii[i].name, who, sizeof who);
        printf("  mii %-12s first %d left %d, category %d:%s\n", g_cm.mii[i].name, g_cm.mii[i].first, g_cm.mii[i].left, cm_cat_of(g_cm.mii[i].name), who);
    }
    for (int i = 0; i < g_cm.nmii; i++)
        for (int j = 0; j < g_cm.nmii; j++) {
            int si = cm_slot(g_cm.mii[i].name), sj = cm_slot(g_cm.mii[j].name), c = i != j && si >= 0 && sj >= 0 ? g_cm.rel[cm_pair(si, sj)] : 0;
            if (c & 0x7f) printf("  tie: %s is %s's %s\n", g_cm.mii[j].name, g_cm.mii[i].name, cm_rel_what(c));
        }
    {   static AiFacts f; int c = ai_want_cat(); ai_facts(&f, 2, c); printf("a thought's facts (category %d):\n  %s\n", c, f.text); }
    for (int i = 0; i < g_cm.nword; i++) printf("  word %s\n", g_cm.word[i]);
    for (int kind = 1; kind <= 2; kind++) {
        printf("%s, speaker unknown:\n", kind == 1 ? "touch talk" : "thoughts");
        for (int i = 0; i < 14; i++) { char l[600]; chatter_line(l, sizeof l, -1, kind); show(l); }
    }
    printf("touch talk, speaker = the first resident (lines about the others):\n");
    for (int i = 0; i < 8; i++) { char l[600]; chatter_line(l, sizeof l, 0, 1); show(l); }
    if (argc < 3) return 0;
    char err[160];
    DWORD t0 = GetTickCount();
    Llm *m = llm_open(argv[2], getenv("TOMO_LLM_THREADS") ? atoi(getenv("TOMO_LLM_THREADS")) : 4, 1536, err, sizeof err);
    if (!m) { printf("model: %s\n", err); return 1; }
    printf("model: %d layers, width %d, %d heads (%d for keys), feed-forward %d, vocabulary %d, %s, %d threads%s, opened in %lu ms\n",
           m->n_layer, m->n_embd, m->n_head, m->n_kv, m->n_ff, m->n_vocab, m->neox ? "qwen2" : "llama", m->nt, m->avx2 ? ", AVX2" : "", GetTickCount() - t0);
    int32_t t[256];
    int n = llm_tok(m, "Hello world, I'm a Mii!", t, 256);
    printf("tokens of \"Hello world, I'm a Mii!\":");
    for (int i = 0; i < n; i++) { char pc[64]; llm_piece(m, t[i], pc, sizeof pc); printf(" %d[%s]", t[i], pc); }
    printf("\n");
    n = ai_turn(m, t, 256, "user\nWhat is the capital of France? Answer in one word.", 0);
    n += ai_turn(m, t + n, 256 - n, "assistant\n", 1);
    t0 = GetTickCount();
    llm_rewind(m, 0);
    llm_eval(m, t, n);
    printf("sanity (greedy): ");
    uint32_t rng = 12345;
    for (int i = 0; i < 8; i++) {
        int tok = llm_sample(m, 0, 1, 1, 1, NULL, 0, &rng);
        if (tok == m->im_end || tok == m->endoftext || tok == m->eos) break;
        char pc[64]; llm_piece(m, tok, pc, sizeof pc); printf("%s", pc);
        llm_eval(m, &tok, 1);
    }
    printf("   (%d prompt tokens, %lu ms)\n", n, GetTickCount() - t0);
    t0 = GetTickCount();
    int prefix = ai_prefix(m);
    printf("fixed prompt: %d tokens in %lu ms\n", prefix, GetTickCount() - t0);
    int lines = getenv("CHATTER_LINES") ? atoi(getenv("CHATTER_LINES")) : 12, good = 0;
    DWORD all = GetTickCount();
    for (int i = 0; i < lines; i++) {
        char raw[300], line[300]; const char *why = "";
        DWORD a = GetTickCount();
        char about[100]; int cat = i % 2 ? ai_want_cat() : -1, ok = ai_write(m, prefix, 1 + i % 2, &rng, raw, sizeof raw, line, sizeof line, about, &cat, &why);
        good += ok;
        if (ok) ai_push(1 + i % 2, line, about, cat);           /* (so the next thought goes to another personality) */
        printf("  %s %5lu ms %-6s %-60s -> %s\n", i % 2 ? "think" : "say  ", GetTickCount() - a, cat >= 0 ? k_pcat_key[cat] : "", raw, ok ? line : why);
    }
    printf("%d of %d lines usable, %.1f s a line\n", good, lines, (GetTickCount() - all) / 1000.0 / (lines ? lines : 1));
    static const char *const var[][2] = {{"BringAndJumpWait", "Won't someone do aerobics with me?"}, {"TalkAboutOthersHouse", "This is some prime real estate."},
        {"FeelingWaitGood", "Teehee..."}, {"Stretch_0_0", "Deep breath!"}, {"Knitting_Work", "Is this a plain stitch?"}, {"LookClockTower", "What time is it?"}};
    int nvar = getenv("CHATTER_VARS") ? atoi(getenv("CHATTER_VARS")) : 6, nchat = getenv("CHATTER_CHATS") ? atoi(getenv("CHATTER_CHATS")) : 4;
    for (int i = 0; i < nvar; i++) {
        char raw[300], line[300]; const char *why = "";
        DWORD a = GetTickCount();
        int ok = ai_write_var(m, prefix, var[i % 6][0], var[i % 6][1], &rng, raw, sizeof raw, line, sizeof line, &why);
        printf("  vary  %5lu ms  %-22s %-40s -> %s\n", GetTickCount() - a, var[i % 6][0], raw, ok ? line : why);
    }
    for (int i = 0; i < nchat && g_cm.nmii >= 2; i++) {
        int x = (int)(chatter_rand() % (unsigned)g_cm.nmii), y = (x + 1 + (int)(chatter_rand() % (unsigned)(g_cm.nmii - 1))) % g_cm.nmii;
        char raw[700], lines[4][300]; int nl = 0; const char *why = "";
        DWORD a = GetTickCount();
        int ok = ai_write_chat(m, prefix, g_cm.mii[x].name, g_cm.mii[y].name, &rng, raw, sizeof raw, lines, &nl, &why);
        for (char *q = raw; *q; q++) if (*q == 10) *q = '|';
        printf("  chat  %5lu ms  %s / %s: %s\n", GetTickCount() - a, g_cm.mii[x].name, g_cm.mii[y].name, raw);
        if (ok) for (int k = 0; k < nl; k++) printf("          %s: %s\n", k & 1 ? g_cm.mii[y].name : g_cm.mii[x].name, lines[k]);
        else printf("          -> %s\n", why);
    }
    llm_close(m);
    return 0;
}
