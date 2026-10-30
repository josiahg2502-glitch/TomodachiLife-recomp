/* chatter_test.c <userdata folder>: the Mii chatter mod (runtime/mii_chatter.c) without the game: its self-test, what it
 * learns from the save in <userdata>\save, and sample lines of both kinds. Point it at a COPY of a userdata folder (it
 * writes chatter_memory.txt there). Build: clang -O1 -w -I runtime -o chatter_test.exe tools/chatter_test.c */
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
    printf("island \"%s\", %d known Miis, %d words\n", g_cm.island, g_cm.nmii, g_cm.nword);
    for (int i = 0; i < g_cm.nmii; i++) printf("  mii %-12s first %d left %d\n", g_cm.mii[i].name, g_cm.mii[i].first, g_cm.mii[i].left);
    for (int i = 0; i < g_cm.nword; i++) printf("  word %s\n", g_cm.word[i]);
    for (int kind = 1; kind <= 2; kind++) {
        printf("%s, speaker unknown:\n", kind == 1 ? "touch talk" : "thoughts");
        for (int i = 0; i < 14; i++) { char l[600]; chatter_line(l, sizeof l, -1, kind); show(l); }
    }
    printf("touch talk, speaker = the first resident (lines about the others):\n");
    for (int i = 0; i < 8; i++) { char l[600]; chatter_line(l, sizeof l, 0, 1); show(l); }
    return 0;
}
