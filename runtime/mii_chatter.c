/* MII CHATTER (2026-10-30, the user's "AI mod": an everlasting expansion of what Miis say; included by tomo.c).
 * The game's line pick (1.0.4 0x71004bbb20: message file, candidate labels, ...; returns {UTF-16 text, length, index |
 * file id << 32} through x8) serves every balloon on the island. The mod lets it pick, then sometimes adds or swaps a line
 * written in the game's own message language (the tags the game fills in: {200.8} the speaker's opening, {200.9} their
 * sentence ending, {2.27} the island, {5.0} you, {6.5} a pause, {0.4} a new balloon, {300.2} the closing), so the game
 * shows and voices it like its own. Which line it is: its label (LBL1 by the returned index). Kinds:
 *   1 touch talk (BalloonTalkOneDirection): the game's line is always said, ours follows in a second balloon;
 *   2 walk-around thoughts (Ai/Ain.msbt InsertActionShort_*), 3 the Mii AI's other lines (activities: Ain.msbt): a share is
 *     swapped (for 3, the AI's variations of that very line from the library);
 *   4 chats between two Miis (ChatMiiLanguage, Mii-speak): a share of conversations in English, written by the AI.
 * The game's own lines stay the most of it (the user: "original game text should still be included"); story, greeting,
 * notice and problem lines are never touched. Who is speaking: chatter_speaker (the request's owner object -> the Mii's
 * data -> the save's per-Mii values); what each Mii is like comes from the save too: the personality (the game's own
 * formula over the sliders, its own names and descriptions), pronouns, birthdays, who they are to each other (as their
 * profiles show it). ADAPTIVE: <userdata>\chatter_memory.txt (residents, newcomers, who moved away; a
 * new Mii is learned when the game saves), chatter_lines.txt (the library of variations and chats: grows as you play;
 * worn lines make way), chatter_knowledge.txt (the game's catalogues: foods, places, things, quirks, trips, tips, read
 * from its own message archive in memory). The AI (llm.c, a free local model in build\ai) also gets the real calendar
 * (weekday, holidays) and real-world topics. Without a model: template lines. graphics.ini mii_chatter = off | some |
 * lots, mii_chat_english = on | off (PC Settings). English only. TESTS: TOMO_CHATTER=<percent> (100 = every line, as touch
 * talk; with a pick log, and for an unknown speaker or a chat the residents found near the pick), TOMO_LLM_MODEL / _THREADS. */
int gfx_mii_chatter(void);                         /* gfx_options.c: 0 off, 1 some, 2 lots */
int gfx_mii_chat_english(void);                    /* gfx_options.c: chats between Miis in English (on by default) */
const char *fs_user_root(void);                    /* nnfs.c: %SSBU_USERDATA% or ..\userdata */
static ImportFn g_chatter_pick;
static int g_chatter_pct = -2;                     /* TOMO_CHATTER: -1 = the setting decides */
static LONG64 g_chatter_lines, g_chatter_picks;

/* ---- the line, in the game's message format ---- */
/* The tags, as the game's files encode them: 0x000E, group, type, size of the parameters, the parameters. */
static int chatter_tag(uint16_t *o, int n, int at, uint16_t group, uint16_t type, const uint8_t *prm, uint16_t size) {
    if (at + 4 + size / 2 >= n) return at;
    o[at++] = 0x0E; o[at++] = group; o[at++] = type; o[at++] = size;
    memcpy(o + at, prm, size);
    return at + size / 2;
}
/* A line (UTF-8) in a small markup: <o> opening, <e> sentence ending, <i> island, <p> the player, <w> pause, <n> new
 * balloon, <h> the closing {300.2}. Parameters as the game's own lines have them most often. */
static int chatter_encode(const char *s, uint16_t *o, int n) {
    static const uint8_t p_open[2] = {0, 0}, p_end[2] = {0, 0xcd}, p_isl[2] = {0, 0xcd}, p_you[2] = {0, 0xcd}, p_wait[4] = {0x2c, 0x01, 0x01, 0xcd}, p_close[2] = {1, 0xcd};
    int at = 0;
    while (*s && at < n - 8) {
        if (*s == '<' && s[1] && s[2] == '>') {
            switch (s[1]) {
            case 'o': at = chatter_tag(o, n, at, 200, 8, p_open, 2); break;
            case 'e': at = chatter_tag(o, n, at, 200, 9, p_end, 2); break;
            case 'i': at = chatter_tag(o, n, at, 2, 27, p_isl, 2); break;
            case 'p': at = chatter_tag(o, n, at, 5, 0, p_you, 2); break;
            case 'w': at = chatter_tag(o, n, at, 6, 5, p_wait, 4); break;
            case 'n': at = chatter_tag(o, n, at, 0, 4, NULL, 0); break;
            case 'h': at = chatter_tag(o, n, at, 300, 2, p_close, 2); break;
            case 's': { static const uint8_t p_self[4] = {0x00, 0x02, 0x00, 0x0d}; at = chatter_tag(o, n, at, 200, 0, p_self, 4); } break;   /* the speaker's own name */
            }
            s += 3;
            continue;
        }
        const uint8_t *u = (const uint8_t *)s;          /* UTF-8 (names and taught words may have accents) */
        uint32_t ch = *u++;
        if (ch >= 0xC0) { int k = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1; ch &= 0x3Fu >> k; while (k-- && (*u & 0xC0) == 0x80) ch = ch << 6 | (*u++ & 0x3F); }
        s = (const char *)u;
        if (ch > 0xFFFF) { ch -= 0x10000; o[at++] = (uint16_t)(0xD800 | ch >> 10); ch = 0xDC00 | (ch & 0x3FF); }
        o[at++] = (uint16_t)ch;
    }
    o[at] = 0;
    return at;
}
static uint32_t chatter_rand(void) {
    static uint32_t x;
    if (!x) { LARGE_INTEGER q; QueryPerformanceCounter(&q); x = (uint32_t)q.QuadPart | 1; }
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}
#define PICK(a) ((a)[chatter_rand() % (sizeof (a) / sizeof (a)[0])])

/* ---- what the mod has learned about the island ---- */
#define CM_MAX 160
typedef struct { char name[100]; int first, left; } CmMii;     /* days since 1601 (left 0 = lives here; first 0 = there before the memory) */
static struct {
    CmMii mii[CM_MAX]; int nmii;
    char island[100];
    char word[300][100]; int nword;                         /* the words you taught the Miis (not kept: the save has them) */
    char heard[8][160]; int nheard, heardi;                 /* what Miis said lately (the game's own lines; for the AI) */
    char slotname[70][100]; uint32_t ids[70][6];            /* by save place: the name, and per-Mii values that tell Miis apart */
    uint8_t ptype[70], pron[70], bmon[70], bday[70];        /* by save place: personality 1..16 (0 unknown), pronoun (1 he, 2 she, 3 they), birthday */
    uint8_t rel[4830];                                      /* per pair of places, both ways: what one is to the other (k_rel + 1; 0 nothing) */
    int loaded, dirty;
    LONG64 sav_t;                                           /* the save files' write times last read */
    DWORD checked;
} g_cm;
static SRWLOCK g_cm_lk = SRWLOCK_INIT;
static int cm_today(void) { FILETIME f; GetSystemTimeAsFileTime(&f); return (int)((((ULONG64)f.dwHighDateTime << 32) | f.dwLowDateTime) / 864000000000ull); }
static void cm_path(char *p, size_t n, const char *name) { snprintf(p, n, "%s\\%s", fs_user_root(), name); }
/* A save file, read without ever standing in the way of the game's save commit (it moves files over this one). */
static uint8_t *cm_read(const char *name, DWORD *n) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\save\\%s", fs_user_root(), name);
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    uint8_t *d = sz != INVALID_FILE_SIZE && sz < (64u << 20) ? malloc(sz) : NULL;
    if (d && (!ReadFile(h, d, sz, &got, NULL) || got != sz)) { free(d); d = NULL; }
    CloseHandle(h);
    *n = sz;
    return d;
}
/* The save's layout (as Zelda: Tears of the Kingdom's): magic 0x01020304, the size of a table of {field hash, value}
 * pairs at +8, the table at +0x20; a string or an array's value is the offset of its data. */
static const uint8_t *sav_field(const uint8_t *d, DWORD n, uint32_t hash) {
    if (!d || n < 0x20 || *(const uint32_t *)d != 0x01020304) return NULL;
    uint32_t tn = *(const uint32_t *)(d + 8) / 8;
    for (uint32_t i = 0; i < tn && 0x28 + i * 8 <= n; i++)
        if (*(const uint32_t *)(d + 0x20 + i * 8) == hash) { uint32_t v = *(const uint32_t *)(d + 0x24 + i * 8); return v + 64 <= n ? d + v : NULL; }
    return NULL;
}
static int sav_utf8(const uint8_t *w16, char *out) {      /* a char16[32] field */
    wchar_t t[33]; memcpy(t, w16, 64); t[32] = 0;
    return t[0] && WideCharToMultiByte(CP_UTF8, 0, t, -1, out, 100, NULL, NULL) > 1;
}
static int sav_strings(const uint8_t *d, DWORD n, uint32_t hash, char (*out)[100], int max) {   /* {count, count x char16[32]} */
    const uint8_t *a = sav_field(d, n, hash);
    int k = 0;
    if (a) for (uint32_t i = 0, cnt = *(const uint32_t *)a; i < cnt && k < max && (size_t)(a - d) + 4 + (i + 1) * 64 <= n; i++) k += sav_utf8(a + 4 + i * 64, out[k]);
    return k;
}
static int sav_ints(const uint8_t *d, DWORD n, uint32_t hash, int32_t *out) {   /* a per-Mii array {70, 70 x 32 bits} */
    const uint8_t *a = sav_field(d, n, hash);
    if (!a || *(const uint32_t *)a != 70 || (size_t)(a - d) + 4 + 70 * 4 > n) return 0;
    memcpy(out, a + 4, 70 * 4);
    return 1;
}
/* A Mii's personality as the game works it out (1.0.4 0x71016972b4, from the Mii's five sliders, 1..8 each): weights of
 * Movement + Speech and of Energy + Thinking (Overall plays no part), each sum in four bands of 50; the type is 1 + the
 * first band + 4 x the second (the game's order: 1 Sin .. 16 Gangan), its category the quarter of that 4 x 4 chart. */
static int cm_band(int w) { return w >= 51 && w <= 100 ? 1 : w >= 101 && w <= 150 ? 2 : w >= 151 && w <= 200 ? 3 : 0; }
static int cm_ptype(int move, int speech, int energy, int thinking) {
    static const uint8_t wa[9] = {50, 10, 20, 30, 40, 60, 70, 80, 90}, wb[9] = {50, 5, 15, 25, 35, 65, 75, 85, 95};
    if ((unsigned)move > 8 || (unsigned)speech > 8 || (unsigned)energy > 8 || (unsigned)thinking > 8) return 0;
    return 1 + cm_band(wa[move] + wb[speech]) + 4 * cm_band(wa[energy] + wb[thinking]);
}
static int cm_pcat(int type) { int i = type - 1; return type ? ((i >> 2) >= 2) * 2 + ((i & 3) >= 2) : -1; }   /* 0 Cool, 1 Dry, 2 Nagomi, 3 Nori; -1 unknown */
static const char *const k_ptype_key[16] = {"Sin", "Jikkuri", "Sabasaba", "Bassari", "Mojimoji", "Kicchiri", "Tekipaki", "Baribari",
    "Honwaka", "Mattari", "Waiwai", "Meramera", "Fuwafuwa", "Nikoniko", "Kirakira", "Gangan"};   /* the game's own keys (Character.msbt labels) */
static const char *const k_pcat_key[4] = {"Cool", "Dry", "Nagomi", "Nori"};
/* Relationships (Mii.sav, per pair of places i < j in order (0,1) (0,2) .. (0,69) (1,2) ..: first what j is to i, then
 * what i is to j): what the profile shows (0x8b41897e; enum values are the hashes of their names) and how close they are
 * (0x42c2fc2f: 100 a start, 140 shown as "Great friends"). Not used: 0x77d6afce, a family enum whose "Partner" the
 * profile shows as plain "Friend". */
static const struct { uint32_t h; const char *what; } k_rel[] = {
    {0x354a0515, "acquaintance"}, {0xba939a42, "friend"}, {0x535e93a8, "former friend"}, {0xb7ce0c18, "sweetheart"}, {0x7783d4c3, "ex"},
    {0xc2d067a7, "spouse"}, {0xfe59f825, "ex-spouse"}, {0xdcfc7603, "parent"}, {0xe193c5a2, "child"},
    {0x1918f808, "older sibling"}, {0x3b1d200a, "younger sibling"}, {0x2fd9785b, "grandparent"}, {0x7e3cd550, "grandchild"}, {0x804172f3, "relative"}};
static int cm_rel_code(uint32_t h) { for (int k = 0; k < (int)(sizeof k_rel / sizeof k_rel[0]); k++) if (k_rel[k].h == h) return k + 1; return 0; }
#define CM_CLOSE 0x80                                       /* (in g_cm.rel: close, 140 or more) */
static const char *cm_rel_what(int c) { int k = c & 0x7f; return !k ? NULL : k == 2 && (c & CM_CLOSE) ? "great friend" : k_rel[k - 1].what; }
static int cm_pair(int i, int j) { int a = i < j ? i : j, b = i < j ? j : i; return 2 * (a * 69 - a * (a - 1) / 2 + (b - a - 1)) + (i > j); }   /* what j is to i */
static int cm_slot(const char *name) { for (int i = 0; i < 70; i++) if (g_cm.slotname[i][0] && !strcmp(g_cm.slotname[i], name)) return i; return -1; }
static int cm_cat_of(const char *name) { int s = name ? cm_slot(name) : -1; return s >= 0 ? cm_pcat(g_cm.ptype[s]) : -1; }
static void cm_load(void) {
    char p[MAX_PATH], l[256];
    cm_path(p, sizeof p, "chatter_memory.txt");
    FILE *f = fopen(p, "r");
    g_cm.loaded = 1;
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        int a, b, at = 0;
        if (!strncmp(l, "island ", 7)) snprintf(g_cm.island, sizeof g_cm.island, "%s", l + 7);
        else if (sscanf(l, "mii %d %d %n", &a, &b, &at) == 2 && at && l[at] && g_cm.nmii < CM_MAX) {
            CmMii *m = &g_cm.mii[g_cm.nmii++];
            snprintf(m->name, sizeof m->name, "%s", l + at); m->first = a; m->left = b;
        }
    }
    fclose(f);
}
static void cm_save(void) {
    char p[MAX_PATH], t[MAX_PATH];
    cm_path(p, sizeof p, "chatter_memory.txt"); cm_path(t, sizeof t, "chatter_memory.tmp");
    FILE *f = fopen(t, "w");
    if (!f) return;
    fprintf(f, "# Mii chatter (mod): what it has learned about this island. Delete this file to make it forget.%c", 10);
    fprintf(f, "# mii <first seen> <moved away, 0 = lives here> <name>: days since 1601 (0 = before the memory began)%c", 10);
    fprintf(f, "island %s%c", g_cm.island, 10);
    for (int i = 0; i < g_cm.nmii; i++) fprintf(f, "mii %d %d %s%c", g_cm.mii[i].first, g_cm.mii[i].left, g_cm.mii[i].name, 10);
    if (fclose(f) == 0 && MoveFileExA(t, p, MOVEFILE_REPLACE_EXISTING)) g_cm.dirty = 0;
}
/* Learn from the save: newcomers, who left, who came back; a different island starts a new memory. */
static void cm_learn(void) {
    DWORD now = GetTickCount();
    if (g_cm.loaded && now - g_cm.checked < 20000) return;
    g_cm.checked = now;
    if (!g_cm.loaded) cm_load();
    char p[MAX_PATH]; WIN32_FILE_ATTRIBUTE_DATA a1, a2;
    snprintf(p, sizeof p, "%s\\save\\Mii.sav", fs_user_root());
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a1)) return;
    snprintf(p, sizeof p, "%s\\save\\Player.sav", fs_user_root());
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a2)) return;
    LONG64 t = (LONG64)a1.ftLastWriteTime.dwLowDateTime ^ ((LONG64)a1.ftLastWriteTime.dwHighDateTime << 32) ^ ((LONG64)a2.ftLastWriteTime.dwLowDateTime << 1) ^ a2.ftLastWriteTime.dwHighDateTime;
    if (t == g_cm.sav_t) return;
    DWORD mn = 0, pn = 0;
    uint8_t *md = cm_read("Mii.sav", &mn), *pd = cm_read("Player.sav", &pn);
    static char names[70][100];
    int nn = md ? sav_strings(md, mn, 0x2499bfda, names, 70) : -1;                /* the residents' names (70 places) */
    if (md) {                                       /* by place: names and the per-Mii values (4 random-looking, 2 times) */
        static const uint32_t idh[6] = {0x5409455d, 0x21eea734, 0x5c7cd87a, 0x400e8774, 0x34190cdf, 0x2b881856};
        const uint8_t *na = sav_field(md, mn, 0x2499bfda);
        memset(g_cm.slotname, 0, sizeof g_cm.slotname); memset(g_cm.ids, 0, sizeof g_cm.ids);
        for (int i = 0; na && i < 70 && (size_t)(na - md) + 4 + (size_t)(i + 1) * 64 <= mn; i++) sav_utf8(na + 4 + i * 64, g_cm.slotname[i]);
        for (int k = 0; k < 6; k++) {
            const uint8_t *a = sav_field(md, mn, idh[k]);
            int es = k < 4 ? 4 : 8;
            if (a && *(const uint32_t *)a == 70 && (size_t)(a - md) + 4 + 70 * (size_t)es <= mn)
                for (int i = 0; i < 70; i++) memcpy(&g_cm.ids[i][k], a + 4 + i * es, 4);
        }
        /* The sliders Movement, Speech, Energy, Thinking; the pronoun (enum: the hash of He / She / They); birth month, day. */
        static int32_t sl[4][70], pr[70], mo[70], dy[70];
        static const uint32_t slh[4] = {0x68e1134e, 0x25b48224, 0xcd8dbaf8, 0x43cd364f};
        int ok = 1;
        for (int k = 0; k < 4; k++) ok &= sav_ints(md, mn, slh[k], sl[k]);
        int okp = sav_ints(md, mn, 0xf3c3de59, pr), okb = sav_ints(md, mn, 0x6cf484f4, mo) && sav_ints(md, mn, 0xab8ae08b, dy);
        for (int i = 0; i < 70; i++) {
            g_cm.ptype[i] = (uint8_t)(ok ? cm_ptype(sl[0][i], sl[1][i], sl[2][i], sl[3][i]) : 0);
            g_cm.pron[i] = (uint8_t)(!okp ? 0 : (uint32_t)pr[i] == 0x3be5d8d4u ? 1 : (uint32_t)pr[i] == 0x25af6ee5u ? 2 : (uint32_t)pr[i] == 0x5677305fu ? 3 : 0);
            g_cm.bmon[i] = (uint8_t)(okb && mo[i] >= 1 && mo[i] <= 12 && dy[i] >= 1 && dy[i] <= 31 ? mo[i] : 0);
            g_cm.bday[i] = (uint8_t)(g_cm.bmon[i] ? dy[i] : 0);
        }
        const uint8_t *ra = sav_field(md, mn, 0x8b41897e), *ca = sav_field(md, mn, 0x42c2fc2f);
        if (ra && ca && *(const uint32_t *)ra == 4830 && *(const uint32_t *)ca == 4830 && (size_t)(ra - md) + 4 + 4830 * 4 <= mn && (size_t)(ca - md) + 4 + 4830 * 4 <= mn)
            for (int k = 0; k < 4830; k++)
                g_cm.rel[k] = (uint8_t)(cm_rel_code(*(const uint32_t *)(ra + 4 + k * 4)) | (*(const int32_t *)(ca + 4 + k * 4) >= 140 ? CM_CLOSE : 0));
    }
    if (pd) g_cm.nword = sav_strings(pd, pn, 0xaa7a1eac, g_cm.word, 300);           /* the words you taught */
    char island[100] = "";
    const uint8_t *is = sav_field(pd, pn, 0xd46df986);                              /* the island's name */
    if (is) sav_utf8(is, island);
    free(md); free(pd);
    if (nn < 0) return;                                                             /* (unreadable now: next time) */
    g_cm.sav_t = t;
    int today = cm_today(), fresh = !g_cm.nmii && !g_cm.island[0];
    if (island[0] && g_cm.island[0] && strcmp(island, g_cm.island)) { g_cm.nmii = 0; fresh = 1; }
    if (island[0] && strcmp(island, g_cm.island)) { snprintf(g_cm.island, sizeof g_cm.island, "%s", island); g_cm.dirty = 1; }
    for (int k = 0; k < nn; k++) {
        int i = 0;
        while (i < g_cm.nmii && strcmp(g_cm.mii[i].name, names[k])) i++;
        if (i == g_cm.nmii) {
            if (i == CM_MAX) {                                   /* full: forget the one who left longest ago */
                int o = -1;
                for (int j = 0; j < g_cm.nmii; j++) if (g_cm.mii[j].left && (o < 0 || g_cm.mii[j].left < g_cm.mii[o].left)) o = j;
                if (o < 0) continue;
                g_cm.mii[o] = g_cm.mii[--g_cm.nmii]; i = g_cm.nmii;
            }
            snprintf(g_cm.mii[i].name, sizeof g_cm.mii[i].name, "%s", names[k]);
            g_cm.mii[i].first = fresh ? 0 : today; g_cm.mii[i].left = 0;
            g_cm.nmii++; g_cm.dirty = 1;
            if (!fresh) printf("chatter: learned a new resident: %s%c", names[k], 10);
        } else if (g_cm.mii[i].left) { g_cm.mii[i].left = 0; g_cm.dirty = 1; printf("chatter: %s is back%c", names[k], 10); }
    }
    for (int i = 0; i < g_cm.nmii; i++) {
        int k = 0;
        while (k < nn && strcmp(g_cm.mii[i].name, names[k])) k++;
        if (k == nn && !g_cm.mii[i].left) { g_cm.mii[i].left = today; g_cm.dirty = 1; printf("chatter: %s moved away%c", g_cm.mii[i].name, 10); }
    }
    if (g_cm.dirty) cm_save();
    {   static int once;
        if (!once++) {
            char who[1200]; int o = 0;
            static const char *const pn[4] = {"", ", he", ", she", ", they"};
            for (int i = 0; i < 70 && o < 1000; i++)
                if (g_cm.slotname[i][0]) o += snprintf(who + o, sizeof who - (size_t)o, " %s (%s/%s%s, born %d/%d)", g_cm.slotname[i], g_cm.ptype[i] ? k_pcat_key[cm_pcat(g_cm.ptype[i])] : "?",
                                                       g_cm.ptype[i] ? k_ptype_key[g_cm.ptype[i] - 1] : "?", pn[g_cm.pron[i] & 3], g_cm.bmon[i], g_cm.bday[i]);
            printf("chatter: knows %d residents, %d words, island \"%s\":%s%c", nn, g_cm.nword, g_cm.island, who, 10);
        }
    }
}

/* ---- the lines ---- */
/* Templates. %F food %D doing %A go-activity %M mood %T thing %S sound %N wish %W when %G greeting; learned: %R another
 * resident, %Y a newcomer, %L someone who moved away, %K a word you taught, %C how many live here. Like the game's own
 * lines (BalloonTalkOneDirection): the opening first, the speaker's sentence ending ONCE, at the end of the last
 * sentence, and {300.2} last. */
static const char *const k_general[] = {
    "<o>I'm feeling %M today. No idea why<e>!",
    "<o>I've been craving %F all day<e>.",
    "<o>Someday I want to try %D<e>.",
    "<o>Hey, <p>, have you ever tried %D<e>?",
    "<o>Do you think %T can tell when I'm happy<e>?",
    "<o>I could listen to %S on <i> forever<e>.",
    "<o>%G <w>I hope you're having fun on <i><e>!",
    "<o>I had a dream about %F last night.<n>It was delicious<e>!",
    "<o>If I were a cloud, I'd float right over <i><e>.",
    "<o>I tried %D yesterday.<n>Let's just say I need more practice<e>...",
    "<o>Life on <i> is never boring<e>!",
    "<o>Quick question, <p>: %F or %F<e>?",
    "<o>Sometimes I just stop and watch %T. <w>It's relaxing<e>.",
    "<o>I've decided %W is for %D<e>!",
    "<o>Have you seen %T %W? So pretty<e>!",
    "<o>I'm %M. <w>Must be the %F I had<e>.",
    "<o>You know what <i> needs? More %N<e>!",
    "<o>I wrote a song about %T.<n>It goes la la la... <w>I'm still working on it<e>.",
    "<o>If you ever want to go %A, count me in, <p><e>!",
    "<o>I tried to have a staring contest with a seagull. <w>I lost<e>.",
    "<o>Do you ever want to eat %F for every meal<e>?",
    "<o>I had the weirdest dream. <w>I was %D on the moon<e>!",
    "<o>Thanks for stopping by, <p>. <w>You always make my day better<e>.",
    "<o>I'm thinking about %D %W. <w>Wanna join<e>?",
    "<o>I practiced my dance moves in the mirror. <w>The mirror looked impressed<e>.",
    "<o>If I had a superpower, I'd make %F appear out of thin air<e>!",
};
static const char *const k_learned[] = {
    "<o>Have you talked to %R lately? <w>I think they're up to something<e>.",
    "<o>%R told me the funniest joke today. <w>I already forgot it<e>...",
    "<o>I want to have a picnic with %R someday<e>.",
    "<o>I bet %R would love some %F<e>.",
    "<o>If %R and I started a band, we'd call it %K<e>!",
    "<o>Have you met %Y yet? <w>I heard they just moved to <i><e>!",
    "<o>I should bring %Y something nice. <w>Moving to a new island is a big deal<e>.",
    "<o>I still think about %L sometimes. <w>I hope they're doing well<e>.",
    "<o>It's not the same around here without %L<e>...",
    "<o>Everyone keeps talking about %K. <w>What's the big deal<e>?",
    "<o>I can't stop thinking about %K<e>!",
    "<o>%K... <w>Just saying it makes me smile<e>.",
    "<o>Someone mentioned %K earlier. <w>Now I'm curious<e>.",
    "<o>There are %C of us on <i> now. <w>It's getting lively<e>!",
};
/* Walk-around thoughts (the game's InsertActionShort lines in Ai/Ain.msbt): short, to themselves, no opening, no closing. */
static const char *const k_think[] = {
    "Mmm... some %F would hit the spot<e>.",
    "Maybe I'll try %D %W<e>.",
    "What a nice day<e>...",
    "Is it %F o'clock yet<e>?",
    "Note to self: <w>try %D<e>.",
    "I should get some %F later<e>.",
    "Feeling %M today<e>.",
    "I hope <p> visits soon<e>.",
    "<i> sure is lively<e>!",
    "Where did I put my %F<e>?",
    "La la la... <w>I love <i><e>.",
    "Hmm... <w>%D or %D<e>?",
};
static const char *const k_think_learned[] = {
    "I can't stop thinking about %K<e>...",
    "Hmm... <w>what would %K do<e>?",
    "%K... <w>Hmm<e>.",
    "Should I tell everyone about %K<e>?",
    "I miss %L<e>...",
    "I wonder how %L is doing<e>.",
    "%C of us here now... <w>Time flies<e>.",
    "I wonder what %R is up to<e>.",
    "I should go say hi to %Y<e>.",
};
static int g_recent[6] = {-1, -1, -1, -1, -1, -1};          /* the last templates used (1000 + i: learned) */
static int cm_other(int speaker, int want) {               /* a random Mii: want 0 resident (not the speaker), 1 newcomer, 2 moved away */
    int pick[CM_MAX], n = 0, today = cm_today();
    for (int i = 0; i < g_cm.nmii; i++) {
        const CmMii *m = &g_cm.mii[i];
        int ok = want == 2 ? m->left != 0 : !m->left && i != speaker && (want == 0 || (m->first && today - m->first <= 3));
        if (ok) pick[n++] = i;
    }
    return n ? pick[chatter_rand() % (unsigned)n] : -1;
}
static int cm_usable(const char *t, int speaker) {
    for (const char *s = t; (s = strchr(s, '%')); s++) {
        if ((s[1] == 'R' || s[1] == 'Y') && (speaker < 0 || cm_other(speaker, s[1] == 'Y') < 0)) return 0;   /* (never about the speaker themself) */
        if (s[1] == 'L' && cm_other(-1, 2) < 0) return 0;
        if (s[1] == 'K' && !g_cm.nword) return 0;
        if (s[1] == 'C') { int n = 0; for (int i = 0; i < g_cm.nmii; i++) n += !g_cm.mii[i].left; if (n < 3) return 0; }
    }
    return 1;
}
/* A new line about the island now and then (learned ones favoured when there is something to say); speaker = the
 * talking Mii's place in the memory, -1 unknown; kind 1 = touch talk, 2 = a walk-around thought. */
#define NUMOF(a) ((int)(sizeof (a) / sizeof (a)[0]))
static void chatter_line(char *out, size_t n, int speaker, int kind) {
    static const char *const food[] = {"pancakes", "curry", "ramen", "strawberry shortcake", "grilled corn", "cheese sandwiches", "lemonade",
        "spaghetti", "fruit salad", "hot cocoa", "fried rice", "apple pie", "popcorn", "dumplings", "tacos", "melon bread"};
    static const char *const doing[] = {"stargazing", "learning the ukulele", "building a giant sandcastle", "writing poetry", "juggling",
        "flying a kite", "baking bread", "dancing in the rain", "collecting seashells", "painting the sunset", "learning magic tricks",
        "starting a band", "planting a garden", "knitting a scarf", "skipping stones", "making a comic book"};
    static const char *const go[] = {"stargazing", "fishing", "swimming", "hiking", "dancing", "jogging", "camping", "bird-watching", "shell hunting"};
    static const char *const mood[] = {"bouncy", "peckish", "a little sleepy", "extra cheerful", "kind of thoughtful", "full of energy", "oddly calm",
        "ready for anything", "a bit mischievous"};
    static const char *const thing[] = {"the clouds", "the waves", "the palm trees", "the seagulls", "the fountain", "the flowers"};
    static const char *const sound[] = {"the waves", "the breeze", "the seagulls", "the birds", "the wind chimes"};
    static const char *const more[] = {"picnics", "dance parties", "ice cream stands", "hammocks", "fireworks", "karaoke nights", "flower gardens", "pillow forts"};
    SYSTEMTIME st; GetLocalTime(&st);
    int part = st.wHour < 5 ? 3 : st.wHour < 12 ? 0 : st.wHour < 17 ? 1 : st.wHour < 21 ? 2 : 3;
    static const char *const when[] = {"this morning", "this afternoon", "this evening", "tonight"};
    static const char *const hello[] = {"Good morning!", "Good afternoon!", "Good evening!", "Hey there, night owl!"};
    const char *shape = NULL;
    const char *const *gen = kind == 2 ? k_think : k_general, *const *lrn = kind == 2 ? k_think_learned : k_learned;
    int base = kind == 2 ? 2000 : 0, ng = kind == 2 ? NUMOF(k_think) : NUMOF(k_general), nl = kind == 2 ? NUMOF(k_think_learned) : NUMOF(k_learned);
    int id = -1;
    for (int k = 0; k < 40 && !shape; k++) {
        int learned = k < 20 && chatter_rand() % 2;
        id = base + (learned ? 1000 + (int)(chatter_rand() % (unsigned)nl) : (int)(chatter_rand() % (unsigned)ng));
        const char *t = id - base >= 1000 ? lrn[id - base - 1000] : gen[id - base];
        int seen = 0;
        for (int j = 0; j < 6; j++) seen |= g_recent[j] == id;
        if (!seen && cm_usable(t, speaker)) shape = t;
    }
    if (!shape) { id = base; shape = gen[0]; }
    memmove(g_recent + 1, g_recent, sizeof g_recent - sizeof g_recent[0]); g_recent[0] = id;
    const char *lastf = NULL, *lastd = NULL;
    char num[16];
    size_t o = 0;
    for (const char *s = shape; *s && o + 120 < n; s++) {
        const char *f = NULL;
        if (*s == '%') {
            int i;
            switch (*++s) {
            case 'F': for (int k = 0; k < 4 && (!f || f == lastf); k++) f = PICK(food); lastf = f; break;  /* "curry or curry?" no */
            case 'D': for (int k = 0; k < 4 && (!f || f == lastd); k++) f = PICK(doing); lastd = f; break;
            case 'A': f = PICK(go); break;
            case 'M': f = PICK(mood); break;
            case 'T': f = PICK(thing); break;
            case 'S': f = PICK(sound); break;
            case 'N': f = PICK(more); break;
            case 'W': f = when[part]; break;
            case 'G': f = hello[part]; break;
            case 'R': case 'Y': case 'L': i = cm_other(speaker, *s == 'R' ? 0 : *s == 'Y' ? 1 : 2); f = i >= 0 ? g_cm.mii[i].name : "someone"; break;
            case 'K': f = g_cm.nword ? g_cm.word[chatter_rand() % (unsigned)g_cm.nword] : "that"; break;
            case 'C': { int c = 0; for (int j = 0; j < g_cm.nmii; j++) c += !g_cm.mii[j].left; snprintf(num, sizeof num, "%d", c); f = num; } break;
            }
            if (f) { size_t l = strlen(f); if (l > 100) l = 100; memcpy(out + o, f, l); o += l; }
            continue;
        }
        out[o++] = *s;
    }
    memcpy(out + o, kind == 2 ? "" : "<h>", kind == 2 ? 1 : 4);   /* {300.2}, as the game ends most of its spoken lines */
}

/* ---- what the AI knows about Tomodachi Life: the game's own catalogues (foods, facilities, goods, quirks, trips) and
 * tips, read from its message archive (the game keeps the whole SARC in memory; found from any message file the pick
 * reads) and kept as <userdata>\chatter_knowledge.txt ("<kind>\t<name>\t<caption>") for the next start ---- */
typedef struct { char kind[8]; char name[80]; char text[260]; } KbEntry;
#define KB_MAX 1200
static KbEntry *g_kb;
static int g_nkb, g_kb_state;                           /* 0 not yet, 1 known, -1 the archive was not found */
static int kb_plain(const uint16_t *w, int n, char *out, size_t on) {   /* a catalogue text as plain ASCII (articles, rubies dropped) */
    size_t o = 0;
    for (int k = 0; k < n && o + 2 < on; k++) {
        uint16_t ch = w[k];
        if (!ch) break;
        if (ch == 0x0E) { if (k + 3 < n) k += 3 + w[k + 3] / 2; continue; }
        if (ch == 0x0F) { k += 2; continue; }
        if (ch == '\n') { if (o && out[o - 1] != ' ') out[o++] = ' '; continue; }
        if (ch == 0x2014 || ch == 0x2013) { out[o++] = '-'; continue; }
        if (ch == 0x2019 || ch == 0x2018) { out[o++] = '\''; continue; }
        if (ch == 0x2026) { if (o + 4 < on) { memcpy(out + o, "...", 3); o += 3; } continue; }
        if (ch < 0x20 || ch >= 0x7F) return 0;
        out[o++] = (char)ch;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return o > 1;
}
/* One MSBT inside the archive: its texts by label. cb(label, text, n) for each. */
typedef void (*KbEach)(const char *label, const uint16_t *text, int n, void *u);
static void kb_msbt(const uint8_t *d, uint32_t size, KbEach cb, void *u) {
    if (size < 0x20 || memcmp(d, "MsgStdBn", 8) || d[8] != 0xFF || d[9] != 0xFE) return;
    uint16_t nsec; memcpy(&nsec, d + 0xE, 2);
    const uint8_t *lbl = NULL, *txt = NULL; uint32_t lsz = 0, tsz = 0, p = 0x20;
    for (int i = 0; i < nsec && p + 0x10 <= size; i++) {
        uint32_t sz; memcpy(&sz, d + p + 4, 4);
        if (p + 0x10 + sz > size) return;
        if (!memcmp(d + p, "LBL1", 4)) { lbl = d + p + 0x10; lsz = sz; }
        if (!memcmp(d + p, "TXT2", 4)) { txt = d + p + 0x10; tsz = sz; }
        p = (p + 0x10 + sz + 15) & ~15u;
    }
    if (!lbl || !txt || tsz < 4 || lsz < 4) return;
    uint32_t nt, nb; memcpy(&nt, txt, 4); memcpy(&nb, lbl, 4);
    for (uint32_t b = 0; b < nb && 4 + b * 8 + 8 <= lsz; b++) {
        uint32_t cnt, off; memcpy(&cnt, lbl + 4 + b * 8, 4); memcpy(&off, lbl + 8 + b * 8, 4);
        for (uint32_t k = 0, q = off; k < cnt && q < lsz; k++) {
            uint8_t ln = lbl[q];
            if (q + 1 + ln + 4 > lsz) break;
            char name[80]; uint32_t idx;
            snprintf(name, sizeof name, "%.*s", (int)ln, (const char *)lbl + q + 1);
            memcpy(&idx, lbl + q + 1 + ln, 4);
            q += 1 + ln + 4;
            if (idx >= nt || 4 + idx * 4 + 4 > tsz) continue;
            uint32_t a, e; memcpy(&a, txt + 4 + idx * 4, 4); e = idx + 1 < nt ? 0 : tsz;
            if (!e) memcpy(&e, txt + 4 + (idx + 1) * 4, 4);
            if (a >= tsz || e > tsz || e <= a) continue;
            cb(name, (const uint16_t *)(const void *)(txt + a), (int)((e - a) / 2), u);
        }
    }
}
typedef struct { const char *kind; char names[600][80]; char keys[600][64]; int n; const char *strip; } KbPair;
static void kb_take_name(const char *label, const uint16_t *t, int n, void *u) {
    KbPair *k = u;
    char plain[96];
    if (k->n < 600 && !strstr(label, "_Plural") && kb_plain(t, n, plain, sizeof plain) && strlen(plain) < 70) {
        snprintf(k->keys[k->n], 64, "%s", label);
        snprintf(k->names[k->n], 80, "%s", plain);
        k->n++;
    }
}
static void kb_take_caption(const char *label, const uint16_t *t, int n, void *u) {
    KbPair *k = u;
    const char *key = !strncmp(label, "Caption_", 8) ? label + 8 : label;
    char plain[300];
    if (!kb_plain(t, n, plain, sizeof plain)) return;
    for (int i = 0; i < k->n && g_nkb < KB_MAX; i++) {
        const char *nk = k->keys[i];
        if (k->strip && !strncmp(nk, k->strip, strlen(k->strip))) nk += strlen(k->strip);     /* (Title_X names, Caption_X captions) */
        const char *ck = k->strip && !strncmp(key, k->strip, strlen(k->strip)) ? key + strlen(k->strip) : key;
        if (strcmp(nk, ck)) continue;
        KbEntry *e = &g_kb[g_nkb++];
        snprintf(e->kind, sizeof e->kind, "%s", k->kind); snprintf(e->name, sizeof e->name, "%s", k->names[i]); snprintf(e->text, sizeof e->text, "%s", plain);
        break;
    }
}
static void kb_take_tip(const char *label, const uint16_t *t, int n, void *u) {
    (void)label; (void)u;
    char plain[300];
    if (g_nkb < KB_MAX && kb_plain(t, n, plain, sizeof plain)) { KbEntry *e = &g_kb[g_nkb++]; snprintf(e->kind, sizeof e->kind, "tip"); e->name[0] = 0; snprintf(e->text, sizeof e->text, "%s", plain); }
}
/* The game's words for the personalities (ReplaceMsg/Character.msbt): "pname" and "pdesc" per type, "cname" and "cdesc"
 * per category, by the game's key (Sabasaba: Rogue, ...). */
static void kb_take_char(const char *label, const uint16_t *t, int n, void *u) {
    static const char *const pre[4] = {"Type16_", "CharacterText16_", "Category4_", "Feature4_"}, *const kind[4] = {"pname", "pdesc", "cname", "cdesc"};
    char plain[260];
    (void)u;
    for (int p = 0; p < 4; p++) {
        size_t pl = strlen(pre[p]);
        if (strncmp(label, pre[p], pl)) continue;
        int known = 0;                                  /* (the key alone: not its _00 and _Plural forms) */
        for (int i = 0; i < (p < 2 ? 16 : 4); i++) known |= !strcmp(label + pl, p < 2 ? k_ptype_key[i] : k_pcat_key[i]);
        if (known && g_nkb < KB_MAX && kb_plain(t, n, plain, sizeof plain)) {
            KbEntry *e = &g_kb[g_nkb++];
            snprintf(e->kind, sizeof e->kind, "%s", kind[p]); snprintf(e->name, sizeof e->name, "%s", label + pl); snprintf(e->text, sizeof e->text, "%s", plain);
        }
        return;
    }
}
static const char *kb_find(const char *kind, const char *name) {
    for (int i = 0; g_kb && i < g_nkb; i++) if (!strcmp(g_kb[i].kind, kind) && !strcmp(g_kb[i].name, name)) return g_kb[i].text;
    return NULL;
}
static int kb_catalogue(const char *k) { return !strcmp(k, "food") || !strcmp(k, "place") || !strcmp(k, "thing") || !strcmp(k, "quirk") || !strcmp(k, "trip"); }
static void kb_save(void) {
    char p[MAX_PATH], t[MAX_PATH];
    cm_path(p, sizeof p, "chatter_knowledge.txt"); cm_path(t, sizeof t, "chatter_knowledge.tmp");
    FILE *f = fopen(t, "w");
    if (!f) return;
    for (int i = 0; i < g_nkb; i++) fprintf(f, "%s\t%s\t%s%c", g_kb[i].kind, g_kb[i].name, g_kb[i].text, 10);
    if (!fclose(f)) MoveFileExA(t, p, MOVEFILE_REPLACE_EXISTING);
}
static void kb_load_file(void) {                        /* the copy from an earlier start (the AI may write before a pick) */
    char p[MAX_PATH], l[420];
    cm_path(p, sizeof p, "chatter_knowledge.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (!g_kb) g_kb = calloc(KB_MAX, sizeof *g_kb);
    while (g_kb && g_nkb < KB_MAX && fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        char *a = strchr(l, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        if (!b) continue;
        *a = 0; *b = 0;
        KbEntry *e = &g_kb[g_nkb++];
        snprintf(e->kind, sizeof e->kind, "%s", l); snprintf(e->name, sizeof e->name, "%s", a + 1); snprintf(e->text, sizeof e->text, "%s", b + 1);
    }
    fclose(f);
    if (g_nkb) g_kb_state = 1;
}
static int kb_readable(uint64_t p, size_t n) {          /* (the AI thread keeps its own region: the pick keeps another) */
    static uint64_t rb, re; static int rok;
    if (p >= rb && p < re && re - p >= n) return rok;
    MEMORY_BASIC_INFORMATION mb;
    if (p < 0x10000 || !VirtualQuery((const void *)(uintptr_t)p, &mb, sizeof mb)) return 0;
    rb = (uint64_t)(uintptr_t)mb.BaseAddress; re = rb + mb.RegionSize;
    rok = mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mb.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY));
    return rok && re - p >= n;
}
/* From a message the game uses (data = one of its sections, noted by the pick): its archive (the SARC that holds it),
 * then the catalogues. On the AI thread (the archive stays put once loaded). */
static volatile uint64_t g_kb_hint;
static void kb_from_game(uint64_t data) {
    if (!g_kb && !(g_kb = calloc(KB_MAX, sizeof *g_kb))) return;
    uint64_t sarc = 0;
    for (uint64_t k = 0; data && k < (96u << 20); k += 4) {         /* back to the archive's header */
        uint64_t q = (data & ~3ull) - k;
        if (!kb_readable(q, 0x20)) break;
        if (rd32(q) == 0x43524153u && rd16(q + 4) == 0x14 && rd16(q + 6) == 0xFEFF && data < q + rd32(q + 8)) { sarc = q; break; }
    }
    uint32_t size = sarc ? rd32(sarc + 8) : 0;
    if (!sarc || !kb_readable(sarc, size)) { printf("chatter: the game's message archive was not found in memory%c", 10); if (!g_nkb) g_kb_state = -1; return; }
    const uint8_t *s = (const uint8_t *)(uintptr_t)sarc;
    uint32_t doff; memcpy(&doff, s + 0xC, 4);
    uint16_t nn; memcpy(&nn, s + 0x14 + 6, 2);
    const uint8_t *nodes = s + 0x14 + 0xC, *names = nodes + nn * 16 + 8;
    static const struct { const char *name, *caption, *kind, *strip; } src[] = {
        {"ReplaceMsg/Food_Name.msbt", "ReplaceMsg/Food_Caption.msbt", "food", NULL},
        {"ReplaceMsg/MapObjectFacility_Name.msbt", "ReplaceMsg/MapObjectFacility_Caption.msbt", "place", NULL},
        {"ReplaceMsg/Goods_Name.msbt", "ReplaceMsg/Goods_Caption.msbt", "thing", NULL},
        {"ReplaceMsg/Habit_Name.msbt", "ReplaceMsg/Habit_Caption.msbt", "quirk", NULL},
        {"ReplaceMsg/Travel_Name.msbt", "ReplaceMsg/Travel_Caption.msbt", "trip", "Title_"},
    };
    static KbPair pair;
    int before = g_nkb;
    g_nkb = 0;                                          /* (the game's own copy wins over the file) */
    for (int si = 0; si < (int)(sizeof src / sizeof src[0]) + 2; si++)
        for (int pass = 0; pass < 2; pass++) {
            const char *want = si < 5 ? (pass ? src[si].caption : src[si].name) : pass ? NULL : si == 5 ? "ProgramMsg/Tips/Tips.msbt" : "ReplaceMsg/Character.msbt";
            if (!want) continue;
            if (!pass && si < 5) { memset(&pair, 0, sizeof pair); pair.kind = src[si].kind; pair.strip = src[si].strip; }
            for (int i = 0; i < nn; i++) {
                uint32_t attr, a, e; memcpy(&attr, nodes + i * 16 + 4, 4); memcpy(&a, nodes + i * 16 + 8, 4); memcpy(&e, nodes + i * 16 + 12, 4);
                if (!(attr & 0x01000000)) continue;
                const char *nm = (const char *)names + (attr & 0xFFFF) * 4;
                if (strcmp(nm, want) || doff + e > size || e < a) continue;
                kb_msbt(s + doff + a, e - a, si == 5 ? kb_take_tip : si == 6 ? kb_take_char : pass ? kb_take_caption : kb_take_name, &pair);
                break;
            }
        }
    if (!g_nkb) { g_nkb = before; printf("chatter: no catalogues in the game's message archive%c", 10); return; }
    g_kb_state = 1;
    int per[7] = {0};
    for (int i = 0; i < g_nkb; i++) per[!strcmp(g_kb[i].kind, "food") ? 0 : !strcmp(g_kb[i].kind, "place") ? 1 : !strcmp(g_kb[i].kind, "thing") ? 2 : !strcmp(g_kb[i].kind, "quirk") ? 3 : !strcmp(g_kb[i].kind, "trip") ? 4 : !strcmp(g_kb[i].kind, "tip") ? 5 : 6]++;
    printf("chatter: knows the game: %d foods, %d places, %d things, %d quirks, %d trips, %d tips, %d personality words (archive %llx, %u bytes)%c", per[0], per[1], per[2], per[3], per[4], per[5], per[6], (unsigned long long)sarc, size, 10);
    kb_save();
}
static const KbEntry *kb_pick(const char *kind) {      /* a random entry of a kind (NULL: any catalogue's) */
    int n = 0;
    for (int i = 0; i < g_nkb; i++) n += kind ? !strcmp(g_kb[i].kind, kind) : kb_catalogue(g_kb[i].kind);
    if (!n) return NULL;
    int r = (int)(chatter_rand() % (unsigned)n);
    for (int i = 0; i < g_nkb; i++) if (kind ? !strcmp(g_kb[i].kind, kind) : kb_catalogue(g_kb[i].kind)) { if (!r--) return &g_kb[i]; }
    return NULL;
}

/* ---- the AI writer: a local language model (llm.c) keeps a few lines ready ----
 * Free and offline: the first build\ai\*.gguf (TOMO_LLM_MODEL=<file> for tests). A thread at the lowest priority loads
 * it, keeps a fixed prompt (what Miis are like, a few examples) in the model's cache and writes lines from what the mod
 * learned (the time and season, how many live here, who moved away, the words you taught, what Miis said lately, a
 * topic), rewinding to the fixed part each time. Each line is checked (ai_clean) before it may be used; the pick takes
 * one when there is one, otherwise a template line. Lines naming a resident are dropped while the speaker is unknown. */
#include "llm.c"
static struct {
    volatile LONG state;                         /* 0 not started, 1 loading, 2 writing, -1 none / failed */
    HANDLE thread, wake;
    char pool[3][8][260]; int n[3];              /* ready lines per kind (1 talk, 2 thought), in the markup */
    char about[3][8][100];                       /* the resident a line is about ("" = nobody) */
    int8_t pcat[3][8];                           /* the personality category a thought was written for (-1 any) */
    char model[MAX_PATH];
    LONG64 made, dropped;
} g_ai;
static SRWLOCK g_ai_lk = SRWLOCK_INIT;
static int ai_find_model(char *out, size_t n) {
    const char *e = getenv("TOMO_LLM_MODEL");
    if (e && *e) { snprintf(out, n, "%s", e); return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES; }
    char dir[MAX_PATH], pat[MAX_PATH];
    if (!GetModuleFileNameA(NULL, dir, MAX_PATH) || !strrchr(dir, '\\')) return 0;
    *strrchr(dir, '\\') = 0;                                         /* build\work */
    snprintf(pat, sizeof pat, "%s\\..\\ai\\*.gguf", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    snprintf(out, n, "%s\\..\\ai\\%s", dir, fd.cFileName);
    FindClose(h);
    return 1;
}
/* Does the line copy one of the prompt's examples (four words in a row)? */
static int ai_words(const char *s, char w[][24], int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s && !isalpha((uint8_t)*s)) s++;
        int k = 0;
        while (isalpha((uint8_t)*s) || *s == '\'') { if (k < 23) w[n][k++] = (char)tolower((uint8_t)*s); s++; }
        if (k) { w[n][k] = 0; n++; }
    }
    return n;
}
static int ai_copies_example(const char *line);
/* Does the line hold this word (whole word, any case; stem = any ending)? */
static int ai_word(const char *s, const char *w, int stem) {
    size_t n = strlen(w);
    for (const char *p = s; *p; p++) {
        if (_strnicmp(p, w, n) || (p > s && isalpha((uint8_t)p[-1]))) continue;
        if (stem || !isalpha((uint8_t)p[n])) return 1;
    }
    return 0;
}
static int ai_personlike(const char *w) {                 /* a person's name, perhaps: two or more words, all capitalized */
    int caps = 0, nw = 0;
    for (const char *q = w; *q; q++) if (q == w || q[-1] == ' ') { nw++; caps += isupper((uint8_t)*q) != 0; }
    return nw >= 2 && caps == nw;
}
/* A model's line made safe for the game: one line of plain ASCII (curly quotes, dashes and ellipses straightened), no
 * markup, stage directions or other names, nothing rude (your taught words excepted); then the game's markup:
 * <o>..<e>.<h> said to you, ..<e>. thought. 0 and why when it cannot be used. */
static int ai_clean(const char *in, int kind, const char *const *names, int nnames, const char *const *ok_names, int nok,
                    const char (*words)[100], int nwords, char *out, size_t n, const char **why) {
    static const char *const banned[] = {"assistant", "language model", "as an ai", "nintendo", "tomodachi", "video game", "chatgpt", "qwen", "player", "Pip", "Captain Zoom", "Bo"};
    static const char *const rude_stem[] = {"fuck", "shit", "bitch", "bastard", "nigg", "fag", "retard", "slut", "whore", "cunt", "pussy", "porn"};
    static const char *const rude_word[] = {"hell", "damn", "crap", "sex", "sexy", "kill", "die", "dead", "death", "stupid", "idiot", "hate", "dick", "drunk", "drugs", "beer", "gun", "guns", "blood"};
    char a[400]; size_t o = 0;
    while (*in == 10 || *in == 13 || *in == 32) in++;               /* (a blank first line) */
    for (const uint8_t *u = (const uint8_t *)in; *u && o < sizeof a - 4; ) {      /* straighten typographic punctuation */
        if (*u == '\n' || *u == '\r') break;
        if (u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0x98 || u[2] == 0x99)) { a[o++] = '\''; u += 3; continue; }
        if (u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0x9C || u[2] == 0x9D)) { a[o++] = '"'; u += 3; continue; }
        if (u[0] == 0xE2 && u[1] == 0x80 && u[2] == 0xA6) { memcpy(a + o, "...", 3); o += 3; u += 3; continue; }
        if (u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0x93 || u[2] == 0x94)) { memcpy(a + o, " - ", 3); o += 3; u += 3; continue; }
        if (*u >= 0x80 || *u < 0x20) { *why = "not plain English"; return 0; }
        a[o++] = (char)*u++;
    }
    a[o] = 0;
    char *s = a;
    while (*s == ' ') s++;
    for (size_t l = strlen(s); l && s[l - 1] == ' '; l--) s[l - 1] = 0;
    static const char *const lead[] = {"Mii:", "Line:", "Thought:", "Assistant:"};
    for (int k = 0; k < 4; k++) if (!_strnicmp(s, lead[k], strlen(lead[k]))) { s += strlen(lead[k]); while (*s == ' ') s++; }
    for (char *p = s, *q = s; ; p++) { if (*p != '"') *q++ = *p; if (!*p) break; }      /* quotes add nothing in a balloon */
    size_t l = strlen(s), lim = kind == 2 || kind == 3 ? 70 : kind == 4 ? 80 : 90;
    if (l > lim) {                                         /* too long: its first sentence(s) that fit */
        size_t cut = 0;
        for (size_t i = 12; i < lim; i++) if (strchr(".!?", s[i]) && s[i + 1] == ' ') cut = i + 1;
        if (cut) { s[cut] = 0; l = cut; }
    }
    if (l < 6 || l > lim) { *why = "too short or too long"; return 0; }
    if (strpbrk(s, "<>[]*#_\\|~^@`()=+/%\"")) { *why = "markup or odd characters"; return 0; }
    for (const char *p = s; (p = strchr(p, '{')); p++) if (strncmp(p, "{player}", 8) && strncmp(p, "{island}", 8)) { *why = "an unknown {placeholder}"; return 0; }
    for (const char *p = s; (p = strchr(p, '}')); p++) if (p < s + 7 || (strncmp(p - 7, "{player}", 8) && strncmp(p - 7, "{island}", 8))) { *why = "an unknown {placeholder}"; return 0; }
    char plain[400];                                       /* the line without its placeholders, for the word checks */
    snprintf(plain, sizeof plain, "%s", s);
    for (char *p; (p = strstr(plain, "{player}")) || (p = strstr(plain, "{island}")); ) memset(p, ' ', 8);
    for (int k = 0; k < (int)(sizeof banned / sizeof banned[0]); k++) {
        int allowed = 0;
        for (int j = 0; j < nok; j++) allowed |= !_stricmp(ok_names[j], banned[k]);
        if (!allowed && ai_word(plain, banned[k], 0)) { *why = "talks about the game or itself"; return 0; }
    }
    for (int k = 0; k < nnames; k++) {                    /* residents: only the one the line was written about */
        int allowed = 0;
        for (int j = 0; j < nok; j++) allowed |= !strcmp(ok_names[j], names[k]);
        if (!allowed && ai_word(plain, names[k], 0)) { *why = "names a resident it was not about"; return 0; }
    }
    if (kind == 2 && (ai_word(plain, "you", 0) || ai_word(plain, "your", 0) || ai_word(plain, "yours", 0) || ai_word(plain, "yourself", 0))) { *why = "a thought that talks to someone"; return 0; }
    if (kind == 2 || kind == 3) for (const char *p = s; (p = strstr(p, "{player}")); p += 8)        /* "..., {player}!" calls to you: not a thought */
        if ((p == s && p[8] == ',') || (p >= s + 2 && p[-2] == ',' && (!p[8] || strchr(".!?,", p[8])))) { *why = "a thought that talks to someone"; return 0; }
    for (int k = 0; k < nwords; k++) {                     /* nor what they are: a small model makes up facts about real people */
        static const char *const claim[] = {"he's", "she's", "he is", "she is", "he was", "she was", "is a", "was a", "is an", "was an"};
        if (!words[k][0] || !strstr(plain, words[k])) continue;
        int person = ai_personlike(words[k]);
        for (int j = 0; j < (int)(sizeof claim / sizeof claim[0]); j++) if ((j < 6 || person) && ai_word(plain, claim[j], 0)) { *why = "states facts about a taught word"; return 0; }
        static const char *const did[] = {"is", "was", "has", "had", "sing", "sings", "sang", "said", "says", "did", "does", "went", "got", "married", "lives", "lived",
            "works", "worked", "made", "makes", "wrote", "writes", "won", "plays", "played", "stars", "starred", "acts", "acted", "loves", "likes", "hates"};
        for (const char *q = plain; person && (q = strstr(q, words[k])); q++) {   /* "<name> sings ...", "<name>'s new ...": a made-up fact */
            const char *after = q + strlen(words[k]);
            if (after[0] == '\'' && after[1] == 's') { *why = "states facts about a taught word"; return 0; }
            for (int j = 0; *after == ' ' && j < (int)(sizeof did / sizeof did[0]); j++)
                if (!strncmp(after + 1, did[j], strlen(did[j])) && !isalpha((uint8_t)after[1 + strlen(did[j])])) { *why = "states facts about a taught word"; return 0; }
        }
    }
    if (ai_copies_example(plain)) { *why = "copies an example"; return 0; }
    for (int k = 0; k < nwords; k++) {                     /* the taught words are things Miis heard of, not who they are */
        char me[2][120];
        snprintf(me[0], sizeof me[0], "I'm %s", words[k]); snprintf(me[1], sizeof me[1], "I am %s", words[k]);
        if (words[k][0] && (ai_word(plain, me[0], 0) || ai_word(plain, me[1], 0))) { *why = "pretends to be a taught word"; return 0; }
    }
    char kind_words[400];                                  /* rude words, but your taught words are yours */
    snprintf(kind_words, sizeof kind_words, "%s", plain);
    for (int k = 0; k < nwords; k++) {
        size_t wl = strlen(words[k]);
        for (char *p = kind_words; wl && (p = strstr(p, words[k])); p += wl) memset(p, ' ', wl);
    }
    for (int k = 0; k < (int)(sizeof rude_stem / sizeof rude_stem[0]); k++) if (ai_word(kind_words, rude_stem[k], 1)) { *why = "rude"; return 0; }
    for (int k = 0; k < (int)(sizeof rude_word / sizeof rude_word[0]); k++) if (ai_word(kind_words, rude_word[k], 0)) { *why = "rude"; return 0; }
    size_t e = strlen(s);                                  /* the sentence ending goes before the last punctuation */
    while (e && strchr(".!?", s[e - 1])) e--;
    if (e == 0) { *why = "no words"; return 0; }
    char body[300];
    size_t bo = 0;
    for (size_t i = 0; i < e && bo < sizeof body - 4; ) {
        if (!strncmp(s + i, "{player}", 8)) { memcpy(body + bo, "<p>", 3); bo += 3; i += 8; }
        else if (!strncmp(s + i, "{island}", 8)) { memcpy(body + bo, "<i>", 3); bo += 3; i += 8; }
        else body[bo++] = s[i++];
    }
    body[bo] = 0;
    snprintf(out, n, "%s%s<e>%s%s", kind == 1 || kind == 4 ? "<o>" : "", body, s[e] ? s + e : ".", kind == 1 ? "<h>" : "");
    return 1;
}
static void ai_push(int kind, const char *line, const char *about, int cat) {
    AcquireSRWLockExclusive(&g_ai_lk);
    if (g_ai.n[kind] < 8) {
        g_ai.pcat[kind][g_ai.n[kind]] = (int8_t)cat;
        snprintf(g_ai.about[kind][g_ai.n[kind]], sizeof g_ai.about[0][0], "%s", about ? about : "");
        snprintf(g_ai.pool[kind][g_ai.n[kind]++], sizeof g_ai.pool[0][0], "%s", line);
    }
    ReleaseSRWLockExclusive(&g_ai_lk);
}
/* The oldest ready line this speaker may say: one about nobody, or (speaker known) about someone else; one written for
 * the speaker's personality category first (cat -1: unknown). */
static int ai_take(int kind, const char *speaker, int cat, char *out, size_t n) {
    int got = 0;
    AcquireSRWLockExclusive(&g_ai_lk);
    for (int pass = cat >= 0 ? 0 : 1; pass < 2 && !got; pass++)
        for (int i = 0; kind >= 1 && kind <= 2 && i < g_ai.n[kind] && !got; i++) {
            const char *a = g_ai.about[kind][i];
            int mine = 0;                                   /* (about: one name, or two with a tab between) */
            for (const char *q = a; speaker && *q; q += strcspn(q, "\t"), q += *q == '\t') mine |= !strncmp(q, speaker, strcspn(q, "\t")) && strlen(speaker) == strcspn(q, "\t");
            if ((a[0] && (!speaker || mine)) || (!pass && g_ai.pcat[kind][i] != cat)) continue;
            snprintf(out, n, "%s", g_ai.pool[kind][i]);
            memmove(g_ai.pool[kind][i], g_ai.pool[kind][i + 1], sizeof g_ai.pool[0][0] * (size_t)(g_ai.n[kind] - i - 1));
            memmove(g_ai.about[kind][i], g_ai.about[kind][i + 1], sizeof g_ai.about[0][0] * (size_t)(g_ai.n[kind] - i - 1));
            memmove(&g_ai.pcat[kind][i], &g_ai.pcat[kind][i + 1], (size_t)(g_ai.n[kind] - i - 1));
            g_ai.n[kind]--;
            got = 1;
        }
    ReleaseSRWLockExclusive(&g_ai_lk);
    if (got && g_ai.wake) SetEvent(g_ai.wake);
    return got;
}
static int ai_room(int kind) { AcquireSRWLockShared(&g_ai_lk); int r = g_ai.n[kind] < 6; ReleaseSRWLockShared(&g_ai_lk); return r; }
/* The real world's calendar: the weekday and a holiday within a week (Easter and the US Thanksgiving, Mother's and
 * Father's Day computed). */
static int ai_days_from_civil(int y, int m, int d) { y -= m <= 2; int era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + doe; }
static int ai_nth_weekday(int y, int m, int wd, int nth) { int first = ai_days_from_civil(y, m, 1), w = (first + 4) % 7; return 1 + (wd - w + 7) % 7 + (nth - 1) * 7; }   /* (wd: 0 Sunday) */
static int ai_calendar(char *out, size_t n) {          /* 1: a holiday is near */
    static const char *const wday[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    SYSTEMTIME st; GetLocalTime(&st);
    int today = ai_days_from_civil(st.wYear, st.wMonth, st.wDay), best = 99;
    const char *hname = NULL;
    for (int yy = st.wYear; yy <= st.wYear + 1; yy++) {
        int a = yy % 19, b = yy / 100, c = yy % 100, d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3, h = (19 * a + b - d - g + 15) % 30, i = c / 4, k = c % 4,
            l = (32 + 2 * e + 2 * i - h - k) % 7, m = (a + 11 * h + 22 * l) / 451, em = (h + l - 7 * m + 114) / 31, ed = (h + l - 7 * m + 114) % 31 + 1;
        struct { int m, d; const char *name; } hol[] = {
            {1, 1, "New Year's Day"}, {2, 14, "Valentine's Day"}, {3, 17, "St. Patrick's Day"}, {4, 1, "April Fools' Day"}, {em, ed, "Easter"},
            {5, ai_nth_weekday(yy, 5, 0, 2), "Mother's Day"}, {6, ai_nth_weekday(yy, 6, 0, 3), "Father's Day"}, {7, 4, "the Fourth of July"},
            {10, 31, "Halloween"}, {11, ai_nth_weekday(yy, 11, 4, 4), "Thanksgiving"}, {12, 24, "Christmas Eve"}, {12, 25, "Christmas"}, {12, 31, "New Year's Eve"}};
        for (int j = 0; j < (int)(sizeof hol / sizeof hol[0]); j++) {
            int dd = ai_days_from_civil(yy, hol[j].m, hol[j].d) - today;
            if (dd >= 0 && dd < best) { best = dd; hname = hol[j].name; }
        }
    }
    int o = snprintf(out, n, " Today is %s.", wday[(today + 4) % 7 < 0 ? 0 : (today + 4) % 7]);
    if (hname && best <= 7) { snprintf(out + o, n - (size_t)o, best == 0 ? " It is %s today!" : best == 1 ? " %s is tomorrow!" : " %s is in %d days.", hname, best); return 1; }
    return 0;
}
/* A resident in the game's own words, for the model: " Melissa (she/her) is an Outgoing Go-Getter: Bold and ..." (the
 * personality and its description from the game's Character texts, once read; "" when nothing is known). 1 when the
 * personality is in it. Under g_cm_lk. */
static const char *cm_pron(const char *name) { static const char *const p[4] = {"", " (he/him)", " (she/her)", " (they/them)"}; int s = cm_slot(name); return s >= 0 ? p[g_cm.pron[s] & 3] : ""; }
static int ai_person(const char *name, char *out, size_t n) {
    int s = cm_slot(name), t = s >= 0 ? g_cm.ptype[s] : 0;
    const char *tn = t ? kb_find("pname", k_ptype_key[t - 1]) : NULL, *cn = t ? kb_find("cname", k_pcat_key[cm_pcat(t)]) : NULL, *d = t ? kb_find("pdesc", k_ptype_key[t - 1]) : NULL;
    out[0] = 0;
    if (tn && cn) { snprintf(out, n, " %s%s is %s %s %s%s%s", name, cm_pron(name), strchr("AEIOU", cn[0]) ? "an" : "a", cn, tn, d ? ": " : ".", d ? d : ""); return 1; }
    if (s >= 0 && g_cm.pron[s]) snprintf(out, n, " %s%s lives on {island}.", name, cm_pron(name));
    return 0;
}
/* What the model is told this time (the user turn), from the memory; names it may use go to ok[]. cat: a thought for a
 * Mii of this personality category (-1 any; f->cat says whether it was used). */
typedef struct { char text[1400]; char names[CM_MAX][100]; int nnames; char ok[8][100]; int nok; char words[300][100]; int nwords; char about[100]; int cat; } AiFacts;
static void ai_facts(AiFacts *f, int kind, int cat) {
    static const char *const season[] = {"a winter", "a winter", "a spring", "a spring", "a spring", "a summer", "a summer", "a summer", "an autumn", "an autumn", "an autumn", "a winter"};
    static const char *const topics[] = {"food", "a dream", "a hobby", "the weather", "the island", "the player", "music", "the sea", "a silly worry",
        "a plan for later", "a snack", "a nap", "clothes", "friendship", "a funny thought", "the stars", "a game they made up"};
    SYSTEMTIME st; GetLocalTime(&st);
    const char *part = st.wHour < 5 ? "night" : st.wHour < 12 ? "morning" : st.wHour < 17 ? "afternoon" : st.wHour < 21 ? "evening" : "night";
    char topic[160];
    int o = 0, live = 0;
    memset(f, 0, sizeof *f);
    f->cat = -1;
    AcquireSRWLockShared(&g_cm_lk);
    const char *gone = NULL, *who = NULL, *bname = NULL, *ra = NULL, *rb = NULL, *rwhat = NULL;
    int nrel = 0;
    int fresh = 0, today = cm_today(), bdays = 99, civil = ai_days_from_civil(st.wYear, st.wMonth, st.wDay);
    for (int i = 0; i < g_cm.nmii; i++) {
        if (!g_cm.mii[i].left) {
            live++;
            if (f->nnames < CM_MAX) snprintf(f->names[f->nnames++], 100, "%s", g_cm.mii[i].name);
            int is_new = g_cm.mii[i].first && today - g_cm.mii[i].first <= 3;
            if (!who || (is_new && !fresh) || (is_new == fresh && chatter_rand() % (unsigned)live == 0)) { who = g_cm.mii[i].name; fresh = is_new; }   /* (a newcomer first) */
            int s = cm_slot(g_cm.mii[i].name);             /* a birthday in the next three days (the game celebrates them) */
            if (s >= 0 && g_cm.bmon[s]) {
                int d = ai_days_from_civil(st.wYear, g_cm.bmon[s], g_cm.bday[s]) - civil;
                if (d < 0) d = ai_days_from_civil(st.wYear + 1, g_cm.bmon[s], g_cm.bday[s]) - civil;
                if (d <= 3 && d < bdays) { bdays = d; bname = g_cm.mii[i].name; }
            }
        }
        else if (!gone || chatter_rand() % 2) gone = g_cm.mii[i].name;
    }
    for (int i = 0; i < g_cm.nmii; i++)                    /* a tie between two residents (family, sweethearts, friends): one at random */
        for (int j = 0, si = g_cm.mii[i].left ? -1 : cm_slot(g_cm.mii[i].name); si >= 0 && j < g_cm.nmii; j++) {
            int sj = j == i || g_cm.mii[j].left ? -1 : cm_slot(g_cm.mii[j].name), c = sj >= 0 ? g_cm.rel[cm_pair(si, sj)] : 0, k = c & 0x7f;
            if (k > 1 && (k != 2 || si < sj) && chatter_rand() % (unsigned)++nrel == 0) { ra = g_cm.mii[j].name; rb = g_cm.mii[i].name; rwhat = cm_rel_what(c); }   /* (friends once, not twice) */
        }
    if (gone) snprintf(f->ok[f->nok++], 100, "%s", gone);
    f->nwords = g_cm.nword;
    memcpy(f->words, g_cm.word, sizeof f->words[0] * (size_t)g_cm.nword);
    const char *heard = g_cm.nheard ? g_cm.heard[chatter_rand() % (unsigned)g_cm.nheard] : NULL;
    const char *w1 = f->nwords ? f->words[chatter_rand() % (unsigned)f->nwords] : NULL;
    if (kind == 4 && w1 && ai_personlike(w1)) w1 = NULL;   /* (a chat's later lines go on about a name unchecked: made-up facts about real people) */
    /* One thing at a time (the model works every fact it is given into the line): a taught word 3 in 10, someone who moved
     * away 2 in 10, something a Mii said 2 in 10, another resident 2 in 10 (thoughts only: there the speaker is known, and
     * a line about a resident is never given to that resident), else only a topic. */
    uint32_t r = chatter_rand() % 20;
    const KbEntry *kbe = r >= 11 && r < 16 ? kb_pick(NULL) : NULL;                    /* the game's own catalogues, 1 in 4 */
    char cal[160] = "";
    int use_bday = bname && r >= 16 && r < 18, holiday = r >= 16 && r < 18 && !use_bday ? ai_calendar(cal, sizeof cal) : 0;   /* a birthday or the real calendar, 1 in 10 */
    int use_word = w1 && r < 5, use_gone = gone && r >= 5 && r < 7, use_heard = heard && r >= 7 && r < 9, use_rel = kind == 2 && ra && r == 10, use_who = kind == 2 && who && live >= 2 && r >= 9 && r < 11 && !use_rel;
    int use_kb = kbe != NULL, use_cal = cal[0] != 0, use_world = r >= 18;            /* something from the real world, 1 in 10 */
    o += snprintf(f->text + o, sizeof f->text - (size_t)o, "Facts: It is %s %s.", season[st.wMonth >= 1 && st.wMonth <= 12 ? st.wMonth - 1 : 0], part);
    if (live >= 2) o += snprintf(f->text + o, sizeof f->text - (size_t)o, " %d Miis live on {island}.", live);
    if (use_gone) o += snprintf(f->text + o, sizeof f->text - (size_t)o, " A Mii named %s moved away from {island}.", gone);
    if (use_word) o += snprintf(f->text + o, sizeof f->text - (size_t)o, " The player taught the Miis the word \"%s\".", w1);
    if (use_heard) o += snprintf(f->text + o, sizeof f->text - (size_t)o, " A Mii said lately: \"%s\"", heard);
    if (use_kb) {
        const char *k = kbe->kind;
        o += snprintf(f->text + o, sizeof f->text - (size_t)o,
                      !strcmp(k, "food") ? " Miis on {island} can eat %s: %s" : !strcmp(k, "place") ? " {island} can have %s: %s" : !strcmp(k, "thing") ? " Miis can get %s to play with: %s"
                      : !strcmp(k, "quirk") ? " Some Miis have this quirk: they %s. %s" : " Miis can go on %s: %s", kbe->name, kbe->text);
    }
    if (use_cal) o += snprintf(f->text + o, sizeof f->text - (size_t)o, "%s", cal);
    if (use_bday) {
        o += snprintf(f->text + o, sizeof f->text - (size_t)o, bdays == 0 ? " It is %s's birthday today!" : bdays == 1 ? " It is %s's birthday tomorrow!" : " %s's birthday is in %d days.",
                      bname, bdays);
        snprintf(f->about, sizeof f->about, "%s", bname);
        if (f->nok < 8) snprintf(f->ok[f->nok++], 100, "%s", bname);
    }
    if (use_rel) {                                         /* (about both: never said by either of them) */
        o += snprintf(f->text + o, sizeof f->text - (size_t)o, " %s is %s's %s.", ra, rb, rwhat);
        snprintf(f->about, sizeof f->about, "%s\t%s", ra, rb);
        if (f->nok < 7) { snprintf(f->ok[f->nok++], 100, "%s", ra); snprintf(f->ok[f->nok++], 100, "%s", rb); }
    }
    if (use_who) {
        o += snprintf(f->text + o, sizeof f->text - (size_t)o, fresh ? " A Mii named %s%s moved to {island} recently." : " A Mii named %s%s also lives on {island}.", who, cm_pron(who));
        snprintf(f->about, sizeof f->about, "%s", who);
        if (f->nok < 8) snprintf(f->ok[f->nok++], 100, "%s", who);
    }
    ReleaseSRWLockShared(&g_cm_lk);
    if (use_word) snprintf(topic, sizeof topic, "the word \"%s\"", w1);
    else if (use_gone) snprintf(topic, sizeof topic, "missing %s", gone);
    else if (use_heard) snprintf(topic, sizeof topic, "what that Mii said");
    else if (use_bday) snprintf(topic, sizeof topic, "%s's birthday", f->about);
    else if (use_who) snprintf(topic, sizeof topic, "%s", f->about);
    else if (use_rel) snprintf(topic, sizeof topic, "%s and %s", rb, ra);
    else if (use_kb) snprintf(topic, sizeof topic, "%s", kbe->name);
    else if (use_cal) snprintf(topic, sizeof topic, "%s", holiday ? "the holiday" : "today");
    else if (use_world) {
        static const char *const world[] = {"a fun fact about animals", "outer space", "the ocean", "dinosaurs", "a favorite song", "sports",
            "a famous invention", "a faraway country", "the weather around the world", "a famous painting", "a science fact", "a world record"};
        snprintf(topic, sizeof topic, "%s", world[chatter_rand() % (sizeof world / sizeof world[0])]);
    }
    else snprintf(topic, sizeof topic, "%s", topics[chatter_rand() % (sizeof topics / sizeof topics[0])]);
    o += snprintf(f->text + o, sizeof f->text - (size_t)o, " Topic: %s.\n%s", topic, kind == 1 ? "A Mii says a line to the player." : "A Mii thinks to itself while walking around.");
    const char *cn = kind == 2 && cat >= 0 ? kb_find("cname", k_pcat_key[cat]) : NULL, *cd = cn ? kb_find("cdesc", k_pcat_key[cat]) : NULL;
    if (cn && cd && o < (int)sizeof f->text) { snprintf(f->text + o, sizeof f->text - (size_t)o, " Its personality is %s: %s Show it without naming it.", cn, cd); f->cat = cat; }   /* (the game's words) */
}
/* The fixed part of the prompt (ChatML): what Miis are like and three examples (with a made-up name, Pip, that lines may
 * not copy). */
static const char *const k_ai_turns[] = {
    "system\nYou write lines for Miis in Tomodachi Life, a life game where Miis live on an island looked after by the player. "
    "Each Mii has a house, a personality, quirks and favorite foods; they shop at the food mart and the clothing shop, eat out, "
    "make friends, fall in love, marry, have kids, go on trips, have dreams and little problems the player helps with, and "
    "the island follows the real clock and calendar. Miis also know the real world: holidays, places, animals, food, music, "
    "sports, science and history, and they talk about it in their own silly, cheerful way; they never claim facts about real "
    "people's lives. A line is one or two short sentences in simple English, under 80 characters. Miis are curious, kind and "
    "a little odd. The words the player taught are things Miis have heard about: they talk about them, but are never them. "
    "A thought is to itself, never to anyone. Never be mean, rude, scary or sad for long. Write {player} for the player's name "
    "and {island} for the island's name. Use no other names unless the facts give them. Write only the line.",
    "user\nFacts: It is a summer morning. 7 Miis live on {island}. Topic: a dream.\nA Mii says a line to the player.",
    "assistant\nGood morning, {player}! I dreamt I was a pancake. A really fluffy one!",
    "user\nFacts: It is an autumn evening. The player taught the Miis the word \"kites\". Topic: the word \"kites\".\nA Mii thinks to itself while walking around.",
    "assistant\nKites... I wonder if they ever get lonely up there.",
    "user\nFacts: It is a spring afternoon. The player taught the Miis the words \"bubble tea\" and \"Captain Zoom\". Topic: the word \"Captain Zoom\".\nA Mii says a line to the player.",
    "assistant\nHave you heard of Captain Zoom, {player}? I want an autograph!",
    "user\nFacts: It is a winter night. A Mii named Pip moved away from {island}. Topic: missing Pip.\nA Mii says a line to the player.",
    "assistant\nI still save a seat for Pip at lunch. Is that silly, {player}?",
    "user\nFacts: It is a summer night. 5 Miis live on {island}. Topic: a snack.\nA Mii thinks to itself while walking around.",
    "assistant\nJust one cookie before bed. Or five. Five is fine.",
    "user\nFacts: It is a spring morning. A Mii is busy with something, and in the game it thinks: \"Left, right, left, right!\"\n"
    "Write a different thought this Mii could have at the same moment, in the same mood.",
    "assistant\nFaster, legs! The ice cream truck won't wait!",
    "user\nFacts: It is a summer afternoon. Two Miis, Pip and Bo, meet on {island} and chat.\nWrite their chat: 4 short lines, "
    "Pip first, each line starting with the name and a colon. Pip never says Bo's name.",
    "assistant\nPip: Guess what? I found a shell shaped like a sandwich!\nBo: No way! Did you eat it, Pip?\nPip: Almost. It looked "
    "so tasty.\nBo: Next time, bring me a real sandwich.",
};
static int ai_copies_example(const char *line) {
    char a[40][24], b[40][24];
    int na = ai_words(line, a, 40);
    for (int e = 0; e < (int)(sizeof k_ai_turns / sizeof k_ai_turns[0]); e++) {
        if (strncmp(k_ai_turns[e], "assistant", 9)) continue;
        int nb = ai_words(k_ai_turns[e] + 10, b, 40);
        for (int i = 0; i + 4 <= na; i++) for (int j = 0; j + 4 <= nb; j++)
            if (!strcmp(a[i], b[j]) && !strcmp(a[i + 1], b[j + 1]) && !strcmp(a[i + 2], b[j + 2]) && !strcmp(a[i + 3], b[j + 3])) return 1;
    }
    return 0;
}
static int ai_turn(Llm *m, int32_t *t, int max, const char *text, int open_assistant) {
    int o = 0;
    if (o < max) t[o++] = m->im_start;
    o += llm_tok(m, text, t + o, max - o);
    if (!open_assistant) { if (o < max) t[o++] = m->im_end; o += llm_tok(m, "\n", t + o, max - o); }
    return o;
}
/* The model's answer to one user turn after the fixed prompt: up to max tokens until the end of its turn or of its
 * max_lines-th line (0: no limit). 1 when it ended in time. */
static int ai_gen(Llm *m, int prefix, const char *user, int max, int max_lines, uint32_t *rng, char *raw, size_t rn) {
    int32_t t[800], out[200];
    int n = ai_turn(m, t, 700, user, 0), no = 0, ended = 0, lines = 0, cur = 0;
    n += ai_turn(m, t + n, 800 - n, "assistant\n", 1);
    size_t ro = 0;
    raw[0] = 0;
    llm_rewind(m, prefix);
    if (max > 200) max = 200;
    if (!llm_eval(m, t, n))
        while (no < max) {
            int tok = llm_sample(m, 0.8f, 40, 0.9f, 1.15f, out, no, rng);
            if (tok == m->im_end || tok == m->endoftext || tok == m->eos) { ended = 1; break; }
            char piece[64];
            int pl = llm_piece(m, tok, piece, sizeof piece), cut = pl;
            for (int i = 0; i < pl && cut == pl; i++)           /* (a line counts once it has words: blank lines do not) */
                if (piece[i] == '\n') { lines += cur != 0; cur = 0; if (max_lines && lines >= max_lines) cut = i; }
                else cur += piece[i] != ' ';
            if (ro + (size_t)cut + 1 < rn) { memcpy(raw + ro, piece, (size_t)cut); ro += (size_t)cut; raw[ro] = 0; }
            if (cut < pl) { ended = 1; break; }
            out[no++] = tok;
            if (llm_eval(m, &tok, 1)) break;
        }
    llm_rewind(m, prefix);
    return ended;
}
/* One line: the facts as the user turn, then up to 40 tokens until the end of the turn or of the line. *cat: the
 * personality category wanted (a thought), then the one it was written for (-1 any). */
static int ai_write(Llm *m, int prefix, int kind, uint32_t *rng, char *raw, size_t rn, char *line, size_t ln, char *about, int *cat, const char **why) {
    static AiFacts f;
    ai_facts(&f, kind, *cat);
    *cat = f.cat;
    if (!ai_gen(m, prefix, f.text, 40, 1, rng, raw, rn)) { *why = "did not finish"; return 0; }
    const char *names[CM_MAX], *ok[8];
    for (int i = 0; i < f.nnames; i++) names[i] = f.names[i];
    for (int i = 0; i < f.nok; i++) ok[i] = f.ok[i];
    snprintf(about, 100, "%s", f.about);
    return ai_clean(raw, kind, names, f.nnames, ok, f.nok, (const char (*)[100])f.words, f.nwords, line, ln, why);
}
/* ---- the line library: the AI's variations of the game's own Mii AI lines (Ai/Ain.msbt: what Miis say while they do
 * things) and chats between two residents, kept in <userdata>\chatter_lines.txt so the game gains lines as it is played:
 * "v <key> <line>" (key = the label without its _V### variant number) and "c <A> <B> <4 lines, A first>" ---- */
typedef struct { char key[48]; char line[220]; uint16_t used; } VlEntry;
typedef struct { char a[100], b[100]; char line[4][200]; uint16_t used; uint8_t n; DWORD last; } DlEntry;   /* n lines (2-4); last said (tick) */
#define VL_MAX 6000
#define DL_MAX 1500
static VlEntry *g_vl; static int g_nvl;
static DlEntry *g_dl; static int g_ndl;
static SRWLOCK g_lib_lk = SRWLOCK_INIT;                 /* (taken after g_cm_lk, never before it) */
static int g_lib_loaded;
static void lib_load(void) {                            /* (once, under g_lib_lk) */
    if (g_lib_loaded++) return;
    g_vl = calloc(VL_MAX, sizeof *g_vl); g_dl = calloc(DL_MAX, sizeof *g_dl);
    if (!g_vl || !g_dl) { free(g_vl); free(g_dl); g_vl = NULL; g_dl = NULL; return; }
    char p[MAX_PATH]; cm_path(p, sizeof p, "chatter_lines.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    static char l[1700];
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        char *t[7]; int nt = 0;
        for (char *q = l; nt < 7; ) { t[nt++] = q; q = strchr(q, '\t'); if (!q) break; *q++ = 0; }
        if (nt == 3 && !strcmp(t[0], "v") && g_nvl < VL_MAX) { snprintf(g_vl[g_nvl].key, sizeof g_vl[0].key, "%s", t[1]); snprintf(g_vl[g_nvl].line, sizeof g_vl[0].line, "%s", t[2]); g_nvl++; }
        else if (nt == 7 && !strcmp(t[0], "c") && g_ndl < DL_MAX) {
            DlEntry *d = &g_dl[g_ndl++];
            snprintf(d->a, sizeof d->a, "%s", t[1]); snprintf(d->b, sizeof d->b, "%s", t[2]);
            for (int k = 0; k < 4; k++) snprintf(d->line[k], sizeof d->line[k], "%s", t[3 + k]);
            d->n = 0;
            while (d->n < 4 && d->line[d->n][0]) d->n++;
            if (d->n < 2) g_ndl--;
        }
    }
    fclose(f);
    printf("chatter: line library: %d variations, %d chats%c", g_nvl, g_ndl, 10);
}
static void lib_append(const char *rec) {
    char p[MAX_PATH]; cm_path(p, sizeof p, "chatter_lines.txt");
    FILE *f = fopen(p, "a");
    if (f) { fputs(rec, f); fputc(10, f); fclose(f); }
}
static int vl_count(const char *key) { int n = 0; for (int i = 0; i < g_nvl; i++) n += !strcmp(g_vl[i].key, key); return n; }
static int vl_worn(const char *key) {                    /* every variation of this line said 4 times or more: time for a new one */
    int n = 0, worn = 0;
    for (int i = 0; i < g_nvl; i++) if (!strcmp(g_vl[i].key, key)) { n++; worn += g_vl[i].used >= 4; }
    return n && worn == n;
}
static int vl_take(const char *key, char *out, size_t n) {   /* the least used variation of this line */
    int best = -1;
    for (int i = 0; i < g_nvl; i++) if (!strcmp(g_vl[i].key, key) && (best < 0 || g_vl[i].used < g_vl[best].used)) best = i;
    if (best < 0) return 0;
    g_vl[best].used++;
    snprintf(out, n, "%s", g_vl[best].line);
    return 1;
}
static void vl_add(const char *key, const char *line) {
    AcquireSRWLockExclusive(&g_lib_lk);
    lib_load();
    int at = g_vl && g_nvl < VL_MAX ? g_nvl : -1, same = 0;
    for (int i = 0; g_vl && i < g_nvl; i++) if (!strcmp(g_vl[i].key, key)) same++;
    if (g_vl && (at < 0 || same >= 6)) {                     /* everlasting: the most used line of this key (or of all) makes room */
        int worst = -1;
        for (int i = 0; i < g_nvl; i++) if ((same >= 6 ? !strcmp(g_vl[i].key, key) : 1) && (worst < 0 || g_vl[i].used > g_vl[worst].used)) worst = i;
        at = worst;
    }
    if (at >= 0) {
        g_vl[at].used = 0;
        snprintf(g_vl[at].key, sizeof g_vl[0].key, "%s", key); snprintf(g_vl[at].line, sizeof g_vl[0].line, "%s", line);
        if (at == g_nvl) g_nvl++;
        char rec[320]; snprintf(rec, sizeof rec, "v\t%s\t%s", key, line); lib_append(rec);
    }
    ReleaseSRWLockExclusive(&g_lib_lk);
}
static int chat_alive(const char *name) { for (int i = 0; i < g_cm.nmii; i++) if (!g_cm.mii[i].left && !strcmp(g_cm.mii[i].name, name)) return 1; return 0; }
static int dl_count(const char *a, const char *b) { int n = 0; for (int i = 0; i < g_ndl; i++) n += !strcmp(g_dl[i].a, a) && !strcmp(g_dl[i].b, b); return n; }
static int dl_find(const char *a, const char *b, DWORD now) {   /* the least used chat a opens with b (any living b when NULL), not said in the last 30 minutes */
    int best = -1;
    for (int i = 0; i < g_ndl; i++) {
        if (strcmp(g_dl[i].a, a) || (b && strcmp(g_dl[i].b, b))) continue;
        if (g_dl[i].last && now - g_dl[i].last < 30 * 60 * 1000) continue;
        if (best < 0 || g_dl[i].used < g_dl[best].used) best = i;
    }
    return best;
}
static void dl_add(const char *a, const char *b, char lines[4][300], int nl) {
    AcquireSRWLockExclusive(&g_lib_lk);
    lib_load();
    int at = g_dl && g_ndl < DL_MAX ? g_ndl : -1, same = g_dl ? dl_count(a, b) : 0;
    if (g_dl && (at < 0 || same >= 4)) {                     /* everlasting: the pair's (or the library's) most used chat makes room */
        int worst = -1;
        for (int i = 0; i < g_ndl; i++) if ((same >= 4 ? !strcmp(g_dl[i].a, a) && !strcmp(g_dl[i].b, b) : 1) && (worst < 0 || g_dl[i].used > g_dl[worst].used)) worst = i;
        at = worst;
    }
    if (at >= 0) {
        DlEntry *d = &g_dl[at];
        if (at == g_ndl) g_ndl++;
        snprintf(d->a, sizeof d->a, "%s", a); snprintf(d->b, sizeof d->b, "%s", b); d->used = 0; d->last = 0; d->n = (uint8_t)nl;
        for (int k = 0; k < 4; k++) snprintf(d->line[k], sizeof d->line[k], "%s", k < nl ? lines[k] : "");
        static char rec[1700];
        snprintf(rec, sizeof rec, "c\t%s\t%s\t%s\t%s\t%s\t%s", a, b, d->line[0], d->line[1], d->line[2], d->line[3]);
        lib_append(rec);
    }
    ReleaseSRWLockExclusive(&g_lib_lk);
}
/* Requests from the pick to the writer (under g_ai_lk): variations to write, pairs of residents seen chatting. */
static struct { char key[48]; char orig[160]; } g_vq[32];
static int g_nvq;
static char g_want[8][2][100];
static int g_nwant;
static void vq_push(const char *key, const char *orig) {
    AcquireSRWLockExclusive(&g_ai_lk);
    int dup = 0;
    for (int i = 0; i < g_nvq; i++) dup |= !strcmp(g_vq[i].key, key);
    if (!dup && g_nvq < 32) { snprintf(g_vq[g_nvq].key, sizeof g_vq[0].key, "%s", key); snprintf(g_vq[g_nvq].orig, sizeof g_vq[0].orig, "%s", orig); g_nvq++; }
    ReleaseSRWLockExclusive(&g_ai_lk);
    if (!dup && g_ai.wake) SetEvent(g_ai.wake);
}
static void want_pair(const char *a, const char *b) {
    AcquireSRWLockExclusive(&g_ai_lk);
    int dup = 0;
    for (int i = 0; i < g_nwant; i++) dup |= !strcmp(g_want[i][0], a) && !strcmp(g_want[i][1], b);
    if (!dup && g_nwant < 8) { snprintf(g_want[g_nwant][0], 100, "%s", a); snprintf(g_want[g_nwant][1], 100, "%s", b); g_nwant++; }
    ReleaseSRWLockExclusive(&g_ai_lk);
    if (!dup && g_ai.wake) SetEvent(g_ai.wake);
}
/* The next pair of residents to write a chat for: one seen chatting without one first, else a pair with fewer than 2. */
static int chat_pair_needed(char *a, char *b) {
    int got = 0;
    AcquireSRWLockExclusive(&g_ai_lk);
    if (g_nwant) { snprintf(a, 100, "%s", g_want[0][0]); snprintf(b, 100, "%s", g_want[0][1]); memmove(g_want[0], g_want[1], sizeof g_want[0] * (size_t)(--g_nwant)); got = 1; }
    ReleaseSRWLockExclusive(&g_ai_lk);
    if (got && !b[0]) {                                 /* an opener without a partner yet: any other resident as the context */
        AcquireSRWLockShared(&g_cm_lk);
        int alive[CM_MAX], n = 0;
        for (int i = 0; i < g_cm.nmii; i++) if (!g_cm.mii[i].left && strcmp(g_cm.mii[i].name, a)) alive[n++] = i;
        if (n) snprintf(b, 100, "%s", g_cm.mii[alive[chatter_rand() % (unsigned)n]].name); else got = 0;
        ReleaseSRWLockShared(&g_cm_lk);
    }
    if (got) return 1;
    AcquireSRWLockShared(&g_cm_lk);
    AcquireSRWLockShared(&g_lib_lk);
    int alive[CM_MAX], n = 0;
    for (int i = 0; i < g_cm.nmii; i++) if (!g_cm.mii[i].left) alive[n++] = i;
    for (int k = 0, start = n ? (int)(chatter_rand() % (unsigned)(n * n)) : 0; k < n * n && !got && g_dl; k++) {
        int i = alive[((start + k) / n) % n], j = alive[(start + k) % n];
        if (i == j) continue;
        int have = 0, fresh = 0;                        /* an opener with fewer than 3 chats not yet said twice: write one more */
        for (int d = 0; d < g_ndl; d++) if (!strcmp(g_dl[d].a, g_cm.mii[i].name)) { have++; fresh += g_dl[d].used < 2; }
        if (fresh >= 3 || have >= 12) continue;
        snprintf(a, 100, "%s", g_cm.mii[i].name); snprintf(b, 100, "%s", g_cm.mii[j].name); got = 1;
    }
    ReleaseSRWLockShared(&g_lib_lk);
    ReleaseSRWLockShared(&g_cm_lk);
    return got;
}
/* A chat in progress: who opened it, who answers, which chat, the next line. A Mii says the lines that are theirs; the
 * one who answers is known only when they speak, so the opening line names nobody, and the answer switches to a chat
 * written for that pair. Under g_cm_lk and g_lib_lk. */
static struct { char a[100], b[100]; int dl, next; DWORD t; } g_cv[4];
static int chat_serve(const char *s, DWORD now, int english_pct, char *out, size_t n) {
    if (!g_dl) return 0;
    for (int i = 0; i < 4; i++) {                       /* a chat this Mii is in */
        if (!g_cv[i].a[0] || now - g_cv[i].t > 20000) continue;
        int mine = !strcmp(g_cv[i].a, s) ? 0 : !strcmp(g_cv[i].b, s) && g_cv[i].next > 1 ? 1 : -1;
        if (mine < 0) continue;
        if (g_cv[i].dl < 0) { g_cv[i].t = now; return 0; }  /* this one is in Mii-speak */
        int k = g_cv[i].next;
        if (k >= g_dl[g_cv[i].dl].n || (k & 1) != mine) return 0;
        snprintf(out, n, "%s", g_dl[g_cv[i].dl].line[k]);
        g_cv[i].next++; g_cv[i].t = now;
        return 1;
    }
    for (int i = 0; i < 4; i++) {                       /* someone opened a chat a moment ago: this Mii answers (the opener's lines */
        if (!g_cv[i].a[0] || now - g_cv[i].t > 12000 || g_cv[i].next != 1 || !strcmp(g_cv[i].a, s)) continue;   /* never name the one who answers) */
        int d = g_cv[i].dl;
        snprintf(g_cv[i].b, sizeof g_cv[i].b, "%s", s);
        if (d < 0) { g_cv[i].next = 2; g_cv[i].t = now; return 0; }
        snprintf(out, n, "%s", g_dl[d].line[1]);
        g_cv[i].next = 2; g_cv[i].t = now;
        return 1;
    }
    int d = (int)(chatter_rand() % 100) < english_pct ? dl_find(s, NULL, now) : -2;   /* this Mii opens a chat (or it stays Mii-speak) */
    if (d == -1) want_pair(s, "");
    if (d < 0) {
        int slot = 0;
        for (int i = 1; i < 4; i++) if (g_cv[i].t < g_cv[slot].t) slot = i;
        snprintf(g_cv[slot].a, sizeof g_cv[slot].a, "%s", s); g_cv[slot].b[0] = 0; g_cv[slot].dl = -1; g_cv[slot].next = 1; g_cv[slot].t = now ? now : 1;
        return 0;
    }
    g_dl[d].last = now ? now : 1;
    int slot = 0;
    for (int i = 1; i < 4; i++) if (g_cv[i].t < g_cv[slot].t) slot = i;
    snprintf(g_cv[slot].a, sizeof g_cv[slot].a, "%s", s); snprintf(g_cv[slot].b, sizeof g_cv[slot].b, "%s", g_dl[d].b);
    g_cv[slot].dl = d; g_cv[slot].next = 1; g_cv[slot].t = now ? now : 1;
    g_dl[d].used++;
    snprintf(out, n, "%s", g_dl[d].line[0]);
    return 1;
}
/* The label's line without its variant number (BringAndJumpWait_V003 -> BringAndJumpWait), and in words for the model
 * (TalkAboutOthersHouse -> talk about others house). */
static void chatter_key(const char *label, char *key, size_t n) {
    snprintf(key, n, "%s", label);
    char *last = NULL;
    for (char *q = key; (q = strstr(q, "_V")); q++) last = q;
    if (last && last[2] && strspn(last + 2, "0123456789") == strlen(last + 2)) *last = 0;
}
static void chatter_activity(const char *key, char *out, size_t n) {
    size_t o = 0;
    for (const char *q = key; *q && o + 3 < n; q++) {
        if (*q == '_' || isdigit((uint8_t)*q)) { if (o && out[o - 1] != ' ') out[o++] = ' '; continue; }
        if (isupper((uint8_t)*q) && o && out[o - 1] != ' ') out[o++] = ' ';
        out[o++] = (char)tolower((uint8_t)*q);
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}
/* A variation of one of the game's own lines: what the Mii is doing, what the game would have it think. */
static int ai_write_var(Llm *m, int prefix, const char *key, const char *orig, uint32_t *rng, char *raw, size_t rn, char *line, size_t ln, const char **why) {
    static AiFacts f;
    static const char *const season[] = {"a winter", "a winter", "a spring", "a spring", "a spring", "a summer", "a summer", "a summer", "an autumn", "an autumn", "an autumn", "a winter"};
    SYSTEMTIME st; GetLocalTime(&st);
    const char *part = st.wHour < 5 ? "night" : st.wHour < 12 ? "morning" : st.wHour < 17 ? "afternoon" : st.wHour < 21 ? "evening" : "night";
    char act[96], user[700];
    ai_facts(&f, 3, -1);                                   /* (for the names and words the line is checked against) */
    chatter_activity(key, act, sizeof act);
    (void)act;
    snprintf(user, sizeof user, "Facts: It is %s %s. A Mii is busy with something, and in the game it thinks: \"%s\"\nWrite a different thought this Mii could have at the same moment, in the same mood.",
             season[st.wMonth >= 1 && st.wMonth <= 12 ? st.wMonth - 1 : 0], part, orig);
    if (!ai_gen(m, prefix, user, 40, 1, rng, raw, rn)) { *why = "did not finish"; return 0; }
    {   char a[40][24], b[40][24];                          /* not the game's own line again: half its words in the same order */
        int na = ai_words(raw, a, 40), nb = ai_words(orig, b, 40), same = 0;
        for (int i = 0, j = 0; i < na && j < nb; ) { if (!strcmp(a[i], b[j])) { same++; i++; j++; } else if (na - i > nb - j) i++; else j++; }
        if (nb && same * 2 >= nb && same * 2 >= na) { *why = "the game's own line again"; return 0; }
    }
    const char *names[CM_MAX];
    for (int i = 0; i < f.nnames; i++) names[i] = f.names[i];
    return ai_clean(raw, 3, names, f.nnames, NULL, 0, (const char (*)[100])f.words, f.nwords, line, ln, why);
}
/* A chat of four lines between two residents, a first; each line checked like a said line (only the other one's name). */
/* The opening line without the other one's name: "Hey, Bo! Look" -> "Hey! Look", "Bo, look" -> "Look". 0 if the name stays. */
static int ai_unname(char *t, const char *b) {
    size_t bl = strlen(b);
    char pat[120];
    for (int pass = 0; pass < 4; pass++) {
        const char *form = pass == 0 ? ", %s" : pass == 1 ? "%s, " : pass == 2 ? " %s!" : " %s?";
        snprintf(pat, sizeof pat, form, b);
        for (char *q; (q = strstr(t, pat)); ) {
            if (pass == 1 && q != t) break;
            size_t pl = strlen(pat), keep = pass >= 2 ? 1 : 0;              /* (" Bo!" -> "!") */
            memmove(q, q + pl - keep, strlen(q + pl - keep) + 1);
            if (pass == 1) t[0] = (char)toupper((uint8_t)t[0]);
        }
    }
    (void)bl;
    return !ai_word(t, b, 0);
}
static int ai_write_chat(Llm *m, int prefix, const char *a, const char *b, uint32_t *rng, char *raw, size_t rn, char lines[4][300], int *nlines, const char **why) {
    static AiFacts f;
    char user[1900], who_a[400], show[160] = "";
    ai_facts(&f, 4, -1);
    char *topic = strstr(f.text, " Topic: ");
    if (topic) *topic = 0;                                 /* (the facts without the request for a single line) */
    AcquireSRWLockShared(&g_cm_lk);
    if (ai_person(a, who_a, sizeof who_a)) snprintf(show, sizeof show, " %s's personality shows in how they talk, never by name.", a);   /* the opener's (anyone may answer: the other's is left out) */
    ReleaseSRWLockShared(&g_cm_lk);
    snprintf(user, sizeof user, "%s%s Two Miis, %s and %s, meet on {island} and chat.\nWrite their chat: 4 short lines, %s first, each line starting with the name and a colon. %s never says %s's name.%s",
             f.text, who_a, a, b, a, a, b, show);
    if (!ai_gen(m, prefix, user, 160, 4, rng, raw, rn)) { *why = "did not finish"; return 0; }
    const char *names[CM_MAX];
    for (int i = 0; i < f.nnames; i++) names[i] = f.names[i];
    for (char *q = raw; (q = strstr(q, "\xEF\xBC\x9A")); ) { *q = ':'; memmove(q + 1, q + 3, strlen(q + 3) + 1); }   /* a full-width colon */
    const char *q = raw;
    int k = 0;
    const char *first_why = NULL;
    for (; k < 4; k++) {
        while (*q == '\n' || *q == ' ') q++;
        const char *who = k & 1 ? b : a, *other = k & 1 ? a : b;
        size_t wl = strlen(who);
        if (strncmp(q, who, wl)) { first_why = "not a chat in turns"; break; }
        const char *r = q + wl;
        while (*r == ' ') r++;
        if (*r == '(') { while (*r && *r != ')' && *r != '\n') r++; if (*r == ')') r++; while (*r == ' ') r++; }   /* "(laughing)" */
        if (*r != ':' && *r != ';') { first_why = "not a chat in turns"; break; }
        r++;
        while (*r == ' ') r++;
        if (*r == '\n') { r++; while (*r == ' ') r++; }                       /* the name on its own line */
        char one[300]; size_t o = 0;
        while (*r && *r != '\n' && o + 1 < sizeof one) one[o++] = *r++;
        one[o] = 0;
        q = r;
        if (!(k & 1) && ai_word(one, b, 0) && !ai_unname(one, b)) { first_why = "the opener names the other one"; break; }   /* (anyone may answer) */
        const char *okn[9] = {other};                      /* (and whoever the facts named: who moved away, a birthday) */
        for (int i = 0; i < f.nok; i++) okn[1 + i] = f.ok[i];
        const char *w = "";
        if (!ai_clean(one, 4, names, f.nnames, okn, 1 + f.nok, (const char (*)[100])f.words, f.nwords, lines[k], 300, &w)) { first_why = w; break; }
    }
    *nlines = k;
    if (k < 2) { *why = first_why ? first_why : "too short"; return 0; }
    return 1;
}
static int ai_prefix(Llm *m) {
    static int32_t t[1200];
    int n = 0;
    for (int i = 0; i < (int)(sizeof k_ai_turns / sizeof k_ai_turns[0]); i++) n += ai_turn(m, t + n, 1200 - n, k_ai_turns[i], 0);
    llm_rewind(m, 0);
    return llm_eval(m, t, n) ? -1 : m->n_past;
}
/* The AI only works while the CPU has room: before loading and before each line, at least 30 % of it idle over a second
 * (the game's loads use every core; on the island it leaves most idle). */
static int g_ai_idle;                                  /* the CPU's idle share at the last look, percent */
static void ai_wait_calm(void) {
    for (;;) {
        FILETIME i0, k0, u0, i1, k1, u1;
        GetSystemTimes(&i0, &k0, &u0); Sleep(1000); GetSystemTimes(&i1, &k1, &u1);
        #define FT64(f) (((ULONGLONG)(f).dwHighDateTime << 32) | (f).dwLowDateTime)
        ULONGLONG idle = FT64(i1) - FT64(i0), all = FT64(k1) - FT64(k0) + FT64(u1) - FT64(u0);   /* (kernel time includes idle) */
        #undef FT64
        g_ai_idle = all ? (int)(idle * 100 / all) : 100;
        if (!all || idle * 100 >= all * 30) return;
        Sleep(2000);
    }
}
/* The personality category to write the next thought for: of the residents (a random one first), the category with the
 * fewest thoughts ready (-1: no personality known). */
static int ai_want_cat(void) {
    int have[4] = {0}, cats[CM_MAX], n = 0, best = -1;
    AcquireSRWLockShared(&g_ai_lk);
    for (int i = 0; i < g_ai.n[2]; i++) if (g_ai.pcat[2][i] >= 0 && g_ai.pcat[2][i] < 4) have[g_ai.pcat[2][i]]++;
    ReleaseSRWLockShared(&g_ai_lk);
    AcquireSRWLockShared(&g_cm_lk);
    for (int i = 0; i < g_cm.nmii; i++) if (!g_cm.mii[i].left) { int c = cm_cat_of(g_cm.mii[i].name); if (c >= 0) cats[n++] = c; }
    ReleaseSRWLockShared(&g_cm_lk);
    for (int k = 0, start = n ? (int)(chatter_rand() % (unsigned)n) : 0; k < n; k++) { int c = cats[(start + k) % n]; if (best < 0 || have[c] < have[best]) best = c; }
    return best;
}
static DWORD WINAPI ai_main(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    SYSTEM_INFO si; GetSystemInfo(&si);
    /* Threads: up to 8 (the processors less 6 for the game) when the CPU is mostly idle, 4 otherwise (TOMO_LLM_THREADS fixes it). */
    int fixed = getenv("TOMO_LLM_THREADS") ? atoi(getenv("TOMO_LLM_THREADS")) : 0, nt = fixed ? fixed : (int)si.dwNumberOfProcessors - 6;
    nt = nt < 1 ? 1 : nt > 8 && !fixed ? 8 : nt;
    char err[160];
    DWORD t0 = GetTickCount();
    Llm *m = llm_open(g_ai.model, nt, 1536, err, sizeof err);
    if (m) m->nt_active = fixed || m->nt <= 4 ? m->nt : 4;     /* (the fixed prompt: 4, while the game may still be loading) */
    if (m) ai_wait_calm();
    int prefix = m ? ai_prefix(m) : -1;
    if (!m || prefix < 0) { printf("chatter: the AI model could not be used (%s): template lines only%c", m ? "the prompt does not fit" : err, 10); llm_close(m); g_ai.state = -1; return 0; }
    printf("chatter: AI model ready in %lu ms (%s, %d threads%s, prompt %d tokens)%c", GetTickCount() - t0, g_ai.model, m->nt, m->avx2 ? ", AVX2" : "", prefix, 10);
    g_ai.state = 2;
    uint32_t rng = GetTickCount() | 1;
    m->nt_active = fixed || m->nt <= 4 ? m->nt : 4;
    for (int kb_tries = 0;;) {
        if (g_kb_state != 1 || kb_tries == 0) if (g_kb_hint && kb_tries < 3) { kb_tries++; ai_wait_calm(); kb_from_game(g_kb_hint); }
        int kind = ai_room(1) ? 1 : ai_room(2) ? 2 : 0;
        char vkey[48] = "", vorig[160] = "", pa[100] = "", pb[100] = "";
        if (!kind) {
            AcquireSRWLockExclusive(&g_ai_lk);
            if (g_nvq) { snprintf(vkey, sizeof vkey, "%s", g_vq[0].key); snprintf(vorig, sizeof vorig, "%s", g_vq[0].orig); memmove(&g_vq[0], &g_vq[1], sizeof g_vq[0] * (size_t)(--g_nvq)); }
            ReleaseSRWLockExclusive(&g_ai_lk);
        }
        if (!kind && !vkey[0] && !(gfx_mii_chat_english() && chat_pair_needed(pa, pb))) { WaitForSingleObject(g_ai.wake, 60000); continue; }
        ai_wait_calm();
        if (!fixed) m->nt_active = g_ai_idle >= 60 ? m->nt : m->nt < 4 ? m->nt : 4;      /* more threads while the CPU is idle */
        char raw[700], line[300], about[100];
        const char *why = "";
        DWORD a = GetTickCount();
        if (kind) {
            int cat = kind == 2 ? ai_want_cat() : -1;
            if (ai_write(m, prefix, kind, &rng, raw, sizeof raw, line, sizeof line, about, &cat, &why)) {
                ai_push(kind, line, about, cat);
                if (g_ai.made++ < 30) printf("chatter: AI wrote a %s in %lu ms%s%s%s%s: %s%c", kind == 1 ? "line" : "thought", GetTickCount() - a, about[0] ? " about " : "", about,
                                             cat >= 0 ? " for " : "", cat >= 0 ? k_pcat_key[cat] : "", line, 10);
                continue;
            }
        } else if (vkey[0]) {
            if (ai_write_var(m, prefix, vkey, vorig, &rng, raw, sizeof raw, line, sizeof line, &why)) {
                vl_add(vkey, line);
                if (g_ai.made++ < 60) printf("chatter: AI wrote a variation of %s in %lu ms: %s (the game's: %s)%c", vkey, GetTickCount() - a, line, vorig, 10);
                continue;
            }
        } else {
            char lines[4][300];
            int nl = 0;
            if (ai_write_chat(m, prefix, pa, pb, &rng, raw, sizeof raw, lines, &nl, &why)) {
                dl_add(pa, pb, lines, nl);
                if (g_ai.made++ < 60) printf("chatter: AI wrote a chat %s / %s (%d lines) in %lu ms: %s | %s%c", pa, pb, nl, GetTickCount() - a, lines[0], lines[1], 10);
                continue;
            }
        }
        if (g_ai.dropped++ < 40) { for (char *q = raw; *q; q++) if (*q == 10) *q = '|'; printf("chatter: AI writing dropped (%s): %s%c", why, raw, 10); }
    }
}
static void ai_start(void) {
    if (InterlockedCompareExchange(&g_ai.state, 1, 0) != 0) return;
    if (!ai_find_model(g_ai.model, sizeof g_ai.model)) { printf("chatter: no AI model in build\\ai: template lines only%c", 10); g_ai.state = -1; return; }
    g_ai.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ai.thread = CreateThread(NULL, 0, ai_main, NULL, 0, NULL);
    if (!g_ai.thread) g_ai.state = -1;
}

/* ---- the hook ---- */
/* The label of the picked line (the game's message file: +0 its LMS message: +0x18 the section table, 0x18 bytes an
 * entry, +0 the data; +0x20 the LBL1 index; LBL1 = bucket count, {count, offset} per bucket, then {length, the label,
 * index}). Read each time: files come and go, an address can be another file later. */
static int chatter_label(uint64_t mfile, uint32_t idx, char *out, size_t n) {
    uint64_t msg = rd64(mfile);
    int li = msg ? (int)rd32(msg + 0x20) : -1;
    if (li < 0) return 0;
    uint64_t lbl = rd64(rd64(msg + 0x18) + (uint64_t)li * 0x18);
    uint32_t nb = lbl ? rd32(lbl) : 0;
    for (uint32_t b = 0; b < nb && b < 65536; b++) {
        uint64_t q = lbl + rd32(lbl + 8 + b * 8);
        for (uint32_t k = 0, cnt = rd32(lbl + 4 + b * 8); k < cnt && k < 4096; k++) {
            uint8_t ln = rd8(q);
            if (rd32(q + 1 + ln) == idx) {
                size_t l = ln < n ? ln : n - 1;
                memcpy(out, (const void *)(uintptr_t)(q + 1), l); out[l] = 0;
                return 1;
            }
            q += 1 + ln + 4;
        }
    }
    return 0;
}
static int chatter_readable(uint64_t p, size_t n) {         /* (TEST probe only; the last region is kept) */
    static uint64_t rb, re; static int rok;
    if (p >= rb && p < re && re - p >= n) return rok;           /* (no p + n: it wraps for p near 2^64) */
    MEMORY_BASIC_INFORMATION mb;
    if (p < 0x10000 || !VirtualQuery((const void *)(uintptr_t)p, &mb, sizeof mb)) return 0;
    rb = (uint64_t)(uintptr_t)mb.BaseAddress; re = rb + mb.RegionSize;
    rok = mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mb.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
    return rok && re - p >= n;
}
static int chatter_label_exists(uint64_t mfile, const char *name) {
    uint64_t msg = rd64(mfile);
    int li = msg ? (int)rd32(msg + 0x20) : -1;
    if (li < 0) return 0;
    uint64_t lbl = rd64(rd64(msg + 0x18) + (uint64_t)li * 0x18);
    uint32_t nb = lbl ? rd32(lbl) : 0, h = 0;
    size_t n = strlen(name);
    if (!nb) return 0;
    for (const char *q = name; *q; q++) h = h * 0x492 + (uint8_t)*q;
    uint64_t q = lbl + rd32(lbl + 8 + (h % nb) * 8);
    for (uint32_t k = 0, cnt = rd32(lbl + 4 + (h % nb) * 8); k < cnt && k < 4096; k++) {
        uint8_t ln = rd8(q);
        if (ln == n && !memcmp((const void *)(uintptr_t)(q + 1), name, n)) return 1;
        q += 1 + ln + 4;
    }
    return 0;
}
/* Who is speaking: for a walk-around thought the game's request (0x71004b9b04) keeps the thinking Mii's object (+0x20;
 * per Mii, 0x1C2000 apart), whose +0x488 is that Mii's data, holding the same random per-Mii values as the save (Mii.sav
 * 0x5409455d at +0x1a18, 0x21eea734 at +0x19c0; found 2026-10-30 by probing the owner for the save values). The request's x19 is saved by
 * 0x71004b9d8c at its x29 + 0x38, the frame that returns to 0x71004b9cd4. Its place in the memory, -1 unknown. Called under
 * g_cm_lk (chatter_readable keeps one region). */
static int chatter_speaker(Ctx *c) {
    uint64_t fp = c->x[29], sp = c->x[31];
    for (int d = 0; d < 12 && fp > sp && fp - sp < 0x40000 && chatter_readable(fp, 0x40); d++) {
        if (rd64(fp + 8) == 0x71004b9cd4ull) {
            uint64_t req = rd64(fp + 0x38), P, M;
            if (!chatter_readable(req + 0x20, 8)) return -1;
            P = rd64(req + 0x20) & ~(uint64_t)3;
            if (!chatter_readable(P + 0x488, 8)) return -1;
            M = rd64(P + 0x488) & ~(uint64_t)3;
            if (!chatter_readable(M + 0x19c0, 0x60)) return -1;
            uint32_t a = rd32(M + 0x1a18), b = rd32(M + 0x19c0);
            for (int i = 0; i < 70; i++)
                if (g_cm.slotname[i][0] && g_cm.ids[i][0] == a && g_cm.ids[i][1] == b)
                    for (int j = 0; j < g_cm.nmii; j++) if (!g_cm.mii[j].left && !strcmp(g_cm.mii[j].name, g_cm.slotname[i])) return j;
            return -1;
        }
        uint64_t up = rd64(fp);
        if (up <= fp) break;
        fp = up;
    }
    return -1;
}
static int chatter_mii_data(uint64_t M) {             /* the resident whose data this is (memory index), -1 */
    M &= ~(uint64_t)3;
    if (M < 0x10000 || !chatter_readable(M + 0x19c0, 0x60)) return -1;
    uint32_t a = rd32(M + 0x1a18), b = rd32(M + 0x19c0);
    if (a <= 0xffff) return -1;
    for (int i = 0; i < 70; i++)
        if (g_cm.slotname[i][0] && g_cm.ids[i][0] == a && g_cm.ids[i][1] == b)
            for (int j = 0; j < g_cm.nmii; j++) if (!g_cm.mii[j].left && !strcmp(g_cm.mii[j].name, g_cm.slotname[i])) return j;
    return -1;
}
static int chatter_mii_at(uint64_t q, int *how) {        /* q is a Mii's owner (how 1) or data (how 2): its memory index */
    q &= ~(uint64_t)3;
    if (q < 0x10000 || !chatter_readable(q, 8)) return -1;
    int i = chatter_mii_data(q);
    if (i >= 0) { *how = 2; return i; }
    if (!chatter_readable(q + 0x488, 8)) return -1;
    i = chatter_mii_data(rd64(q + 0x488));
    if (i >= 0) { *how = 1; return i; }
    return -1;
}
/* A game line as plain text, for the AI to hear: style tags dropped, a new balloon is a space; a line with a slot the game
 * fills in (a name, a word) is skipped. */
static int chatter_plain(uint64_t text, int len, char *out, size_t n) {
    size_t o = 0;
    for (int k = 0; k < len && k < 300 && o + 2 < n; k++) {
        uint16_t ch = rd16(text + (uint64_t)k * 2);
        if (!ch) break;
        if (ch == 0x0E) {
            uint16_t g = rd16(text + (uint64_t)(k + 1) * 2), t = rd16(text + (uint64_t)(k + 2) * 2);
            if (g == 0 && t == 4) out[o++] = ' ';
            else if (!(g == 6 || g == 300 || (g == 200 && (t == 8 || t == 9)))) return 0;
            k += 3 + rd16(text + (uint64_t)(k + 3) * 2) / 2;
            continue;
        }
        if (ch == 0x0F) { k += 2; continue; }
        if (ch >= 0x80) return 0;
        out[o++] = (char)ch;
    }
    out[o] = 0;
    return o > 3;
}
/* TEST: which of the pick's context arguments (x2..x5), or what they point at, is a resident's owner or data object. */
static void chatter_args_mii(Ctx *c, char *out, size_t n) {
    int o = 0, how;
    out[0] = 0;
    for (int a = 2; a <= 5; a++) {
        uint64_t p = c->x[a] & ~3ull;
        int j = chatter_mii_at(p, &how);
        if (j >= 0) o += snprintf(out + o, n - (size_t)o, " x%d=%s(%s)", a, g_cm.mii[j].name, how == 1 ? "owner" : "data");
        for (int off = 0; off < 0x80 && o < (int)n - 48 && p >= 0x10000 && chatter_readable(p, (size_t)off + 8); off += 8)
            if ((j = chatter_mii_at(rd64(p + (uint64_t)off), &how)) >= 0) o += snprintf(out + o, n - (size_t)o, " [x%d+0x%x]=%s(%s)", a, off, g_cm.mii[j].name, how == 1 ? "owner" : "data");
    }
    uint64_t sp = c->x[31], fp = c->x[29];                   /* the callers' stack: residents' objects there, and the frames */
    for (int off = 0; off < 0x3000 && o < (int)n - 48 && chatter_readable(sp + (uint64_t)off, 8); off += 8) {
        int j = chatter_mii_at(rd64(sp + (uint64_t)off), &how);
        if (j >= 0) o += snprintf(out + o, n - (size_t)o, " [sp+0x%x]=%s(%s)", off, g_cm.mii[j].name, how == 1 ? "owner" : "data");
    }
    for (int k = 0; k < 12 && fp > sp && fp - sp < 0x3000 && o < (int)n - 32 && chatter_readable(fp, 16); k++, fp = rd64(fp))
        o += snprintf(out + o, n - (size_t)o, " f+0x%llx:%llx", (unsigned long long)(fp - sp), (unsigned long long)rd64(fp + 8));
}
static void chatter_pick(Ctx *c, void *user) {
    uint64_t res = c->x[8], mfile = c->x[0], arg[4] = {c->x[2], c->x[3], c->x[4], c->x[5]};
    (void)user;
    g_chatter_pick(c);
    if (g_chatter_pct == -2) { const char *e = getenv("TOMO_CHATTER"); g_chatter_pct = e ? atoi(e) : -1; }
    /* How much is new (the user, 2026-10-30: "original game text should still be included"): what a Mii says to you is
     * always the game's line, the new one only added after it (some 30 %, lots 50 % of the time); thoughts and the Mii AI's
     * lines are swapped 2/3 as often; a chat between two Miis is English some 50 %, lots 80 % of the time, else Mii-speak. */
    int pct = g_chatter_pct >= 0 ? g_chatter_pct : gfx_mii_chatter() == 2 ? 50 : gfx_mii_chatter() ? 30 : 0;
    int chat_pct = g_chatter_pct >= 0 ? g_chatter_pct : gfx_mii_chatter() == 2 ? 80 : 50;
    if (!pct || !res || !mfile) return;
    uint64_t text = rd64(res);
    if (!text) return;
    if (!g_ai.state) ai_start();
    if (!g_kb_hint) {                              /* (the knowledge: the writer reads the game's archive from here) */
        uint64_t msg = rd64(mfile), sec = msg ? rd64(msg + 0x18) : 0;
        if (sec) g_kb_hint = rd64(sec);
    }
    char label[80] = "";
    chatter_label(mfile, (uint32_t)rd64(res + 0x10), label, sizeof label);
    int kind = !strncmp(label, "TTPOneDirection", 15) ? 1 : !strncmp(label, "InsertActionShort", 17) ? 2      /* touch talk, walk-around thought */
             : !strncmp(label, "ChatMiiNoTextChat", 17) ? 4 : label[0] && chatter_label_exists(mfile, "InsertActionShort_V000") ? 3 : 0;   /* a chat, the Mii AI's other lines */
    AcquireSRWLockExclusive(&g_cm_lk);
    cm_learn();
    if (g_chatter_pct >= 0 && g_chatter_picks < 80) {          /* TEST: what the game picked, tags as {group.type} */
        char a[160]; int o = 0;
        for (int k = 0; k < 200 && o < 140; k++) {
            uint16_t ch = rd16(text + (uint64_t)k * 2);
            if (!ch) break;
            if (ch == 0x0E) { o += snprintf(a + o, sizeof a - (size_t)o, "{%u.%u}", rd16(text + (uint64_t)(k + 1) * 2), rd16(text + (uint64_t)(k + 2) * 2)); k += 3 + rd16(text + (uint64_t)(k + 3) * 2) / 2; continue; }
            a[o++] = ch < 0x80 && ch >= 0x20 ? (char)ch : '?';
        }
        a[o] = 0;
        int who = chatter_speaker(c);
        printf("chatter: pick %lld %s (kind %d, %s) args %llx %llx %llx: %s%c", (long long)g_chatter_picks, label, kind, who >= 0 ? g_cm.mii[who].name : "speaker unknown",
               (unsigned long long)arg[0], (unsigned long long)arg[1], (unsigned long long)arg[2], a, 10);
        if (who < 0 || kind == 4) { char am[1200]; chatter_args_mii(c, am, sizeof am); if (am[0]) printf("chatter: args-mii %s:%s%c", label, am, 10); }   /* (a chat: the partner?) */
    }
    g_chatter_picks++;
    /* The game can pick the same line twice for one balloon (the greeting did): the same line gets the same answer. */
    static struct { uint64_t key; DWORD t; uint16_t *line; int len; } memo[16];
    static int nmemo;
    uint64_t key = rd64(res + 0x10);
    DWORD now = GetTickCount();
    for (int i = 0; i < 16 && kind != 4; i++)                 /* (not in a chat: there the other Mii may pick the same line) */
        if (memo[i].t && memo[i].key == key && now - memo[i].t < 10000) {
            if (memo[i].line) { wr64(res, (uint64_t)(uintptr_t)memo[i].line); wr32(res + 8, (uint32_t)memo[i].len); }
            ReleaseSRWLockExclusive(&g_cm_lk);
            return;
        }
    uint16_t *line16 = NULL;
    int len = 0, english = 1;
    for (int k = 0; k < 24; k++) {                 /* English only: the picked line's first letters are ASCII (or tags) */
        uint16_t ch = rd16(text + (uint64_t)k * 2);
        if (!ch) break;
        if (ch == 0x0E) { k += 3 + rd16(text + (uint64_t)(k + 3) * 2) / 2; continue; }
        if (ch >= 0x80) { english = 0; break; }
    }
    if (kind && english) {                                     /* the AI hears what Miis say */
        char heard[160];
        if (chatter_plain(text, (int)rd32(res + 8), heard, sizeof heard)) { snprintf(g_cm.heard[g_cm.heardi++ & 7], sizeof g_cm.heard[0], "%s", heard); if (g_cm.nheard < 8) g_cm.nheard++; }
    }
    if (g_chatter_pct == 100 && !kind) kind = 1;              /* (TEST: every line, as touch talk) */
    int spk = kind ? chatter_speaker(c) : -1, ai = 0, have = 0;   /* (-1: not found; then no line names a resident) */
    char l8[600];
    if (kind == 3 && english) {                    /* the Mii AI's own lines: a variation from the library; ask for more */
        char key[48], orig[160];
        chatter_key(label, key, sizeof key);
        AcquireSRWLockExclusive(&g_lib_lk);
        lib_load();
        int cnt = g_vl ? vl_count(key) : 0, worn = cnt && vl_worn(key);
        if (cnt && (int)(chatter_rand() % 100) < pct * 2 / 3) have = ai = vl_take(key, l8, sizeof l8);
        ReleaseSRWLockExclusive(&g_lib_lk);
        if ((cnt < 3 || worn) && g_ai.state >= 1 && chatter_plain(text, (int)rd32(res + 8), orig, sizeof orig)) vq_push(key, orig);
    } else if (kind == 4 && english && spk >= 0 && gfx_mii_chat_english()) {   /* a chat between two residents, in English */
        AcquireSRWLockExclusive(&g_lib_lk);
        lib_load();
        have = ai = chat_serve(g_cm.mii[spk].name, now, chat_pct, l8, sizeof l8);
        ReleaseSRWLockExclusive(&g_lib_lk);
    } else if ((kind == 1 || kind == 2) && english && (int)(chatter_rand() % 100) < (kind == 2 ? pct * 2 / 3 : pct)
               && (kind == 2 || (rd32(res + 8) > 0 && rd32(res + 8) < 160))) {   /* (said to you: only when the game's line fits before ours) */
        ai = ai_take(kind, spk >= 0 ? g_cm.mii[spk].name : NULL, kind == 2 && spk >= 0 ? cm_cat_of(g_cm.mii[spk].name) : -1, l8, sizeof l8);   /* the AI's line when one is ready, else a template */
        if (!ai) chatter_line(l8, sizeof l8, spk, kind);
        have = 1;
    }
    if (have) {
        static uint16_t ring[64][320];             /* the game may keep showing a line while others are picked */
        static int slot;
        line16 = ring[slot++ & 63];
        /* Said to you: the Mii says its own line first and adds the new one in a second balloon ({0.4}), with the closing
         * {300.2} only at the very end as in the game's own two-balloon lines. */
        int olen = (int)rd32(res + 8), at = 0;
        if (kind >= 2)                             /* a thought keeps its leading {6.x} tags (how the game shows thoughts); a chat */
            for (int k = 0, sz; k + 4 < olen && k < 32 && rd16(text + (uint64_t)k * 2) == 0x0E && rd16(text + (uint64_t)(k + 1) * 2) == 6; k += sz) {
                sz = 4 + rd16(text + (uint64_t)(k + 3) * 2) / 2;                                  /* line drops {6.11} (the Mii-speak voice) */
                if (sz > 16) break;
                if (kind == 4 && rd16(text + (uint64_t)(k + 2) * 2) == 11) continue;
                memcpy(line16 + at, (const void *)(uintptr_t)(text + (uint64_t)k * 2), (size_t)sz * 2);
                at += sz;
            }
        else if (kind == 1 && olen > 0 && olen < 160) {   /* the game's own line first, ours in a second balloon */
            memcpy(line16, (const void *)(uintptr_t)text, (size_t)olen * 2);
            at = olen;
            if (at >= 5 && line16[at - 5] == 0x0E && line16[at - 4] == 300 && line16[at - 3] == 2 && line16[at - 2] == 2) at -= 5;
            line16[at++] = 0x0E; line16[at++] = 0; line16[at++] = 4; line16[at++] = 0;
        }
        len = at + chatter_encode(l8, line16 + at, 320 - at);
        wr64(res, (uint64_t)(uintptr_t)line16);
        wr32(res + 8, (uint32_t)len);
        g_chatter_lines++;
        {   static int nlog; if (nlog++ < 60) printf("chatter: %s %s%s: %s%c", spk >= 0 ? g_cm.mii[spk].name : "a Mii", kind == 4 ? "chats" : kind == 3 ? "says while busy" : kind == 2 ? "thinks" : at ? "says after its own line" : "says", ai ? " (AI)" : "", l8, 10); }
    }
    memo[nmemo & 15].key = key; memo[nmemo & 15].t = now ? now : 1; memo[nmemo & 15].line = line16; memo[nmemo & 15].len = len; nmemo++;
    ReleaseSRWLockExclusive(&g_cm_lk);
}
/* The encoder against hand-made tags and UTF-8, and 300 made-up lines: every blank filled, the ending exactly once. */
static int chatter_selftest(void) {
    static const uint16_t want[] = {0x0E, 200, 8, 2, 0x0000, 'H', 0xE9, 0x0E, 200, 9, 2, 0xcd00, '.', 0x0E, 300, 2, 2, 0xcd01, 0};
    uint16_t o[320];
    if (chatter_encode("<o>H\xC3\xA9<e>.<h>", o, 320) != 18 || memcmp(o, want, sizeof want)) return 0;
    for (int i = 0; i < 300; i++) {
        char line[600];
        chatter_line(line, sizeof line, i % 3 - 1, 1 + i % 2);
        int len = chatter_encode(line, o, 320), ends = 0;
        for (int k = 0; k < len; k++) {
            if (o[k] == 0x0E) { ends += o[k + 1] == 200 && o[k + 2] == 9; k += 3 + o[k + 3] / 2; continue; }
            if (o[k] == '%' || o[k] == '<' || o[k] == '>') return 0;
        }
        if (ends != 1) return 0;
    }
    for (int j = 0; j < 6; j++) g_recent[j] = -1;
    static const char *const pre[][2] = {{"Hello world", "Hello| world"}, {"I'm here", "I|'m| here"}, {"wow...!", "wow|...!"}, {"a  b", "a| | b"},
        {"a\n\nb", "a|\n\n|b"}, {"x 42", "x| |4|2"}, {"end.", "end|."}};
    for (int i = 0; i < (int)(sizeof pre / sizeof pre[0]); i++) {
        char got[64]; int o = 0, n = (int)strlen(pre[i][0]);
        for (int k = 0; k < n; ) { int l = llm_piece_len(pre[i][0], n, k, 0); if (o) got[o++] = '|'; memcpy(got + o, pre[i][0] + k, (size_t)l); o += l; k += l; }
        got[o] = 0;
        if (strcmp(got, pre[i][1])) return 0;
    }
    static const char words[2][100] = {"Damn Good Pizza", "Bananas"};
    const char *names[1] = {"Melissa"}, *why = "";
    char out[300];
    if (!ai_clean("Hello there, {player}!", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why) || strcmp(out, "<o>Hello there, <p><e>!<h>")) return 0;
    if (!ai_clean("\"Is it nap time yet?\"", 2, names, 1, NULL, 0, words, 2, out, sizeof out, &why) || strcmp(out, "Is it nap time yet<e>?")) return 0;
    if (!ai_clean("Shell hunting on {island} is fun", 2, names, 1, NULL, 0, words, 2, out, sizeof out, &why) || strcmp(out, "Shell hunting on <i> is fun<e>.")) return 0;
    if (!ai_clean("I want Damn Good Pizza!", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why)) return 0;
    if (ai_clean("Damn Good Pizza sings all day!", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why) || ai_clean("Damn Good Pizza's new song!", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why)) return 0;
    if (ai_clean("I love Melissa so much!", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why)) return 0;
    if (!ai_clean("I wonder what Melissa is up to...", 2, names, 1, names, 1, words, 2, out, sizeof out, &why) || strcmp(out, "I wonder what Melissa is up to<e>...")) return 0;
    if (!ai_clean("Do you want pie, Melissa?", 4, names, 1, names, 1, words, 2, out, sizeof out, &why) || strcmp(out, "<o>Do you want pie, Melissa<e>?")) return 0;
    if (cm_ptype(1, 8, 1, 1) != 3 || cm_pcat(3) != 1 || cm_ptype(4, 4, 4, 4) != 6 || cm_pcat(6) != 0 || cm_ptype(8, 8, 8, 8) != 16 || cm_pcat(16) != 3) return 0;   /* Gregory's and pee's profiles: Ambitious Rogue, Reserved Perfectionist */
    if (cm_pair(0, 1) != 0 || cm_pair(1, 0) != 1 || cm_pair(1, 2) != 138 || cm_pair(3, 6) != 412 || cm_pair(6, 3) != 413 || cm_pair(69, 68) != 4829) return 0;   /* (the save's pair order) */
    char key[48], act[96];
    chatter_key("BringAndJumpWait_V003", key, sizeof key); if (strcmp(key, "BringAndJumpWait")) return 0;
    chatter_key("Stretch_3_2_V000", key, sizeof key); if (strcmp(key, "Stretch_3_2")) return 0;
    chatter_key("Stretch_3_0", key, sizeof key); if (strcmp(key, "Stretch_3_0")) return 0;
    chatter_activity("TalkAboutOthersHouse", act, sizeof act); if (strcmp(act, "talk about others house")) return 0;
    chatter_activity("Stretch_3_2", act, sizeof act); if (strcmp(act, "stretch")) return 0;
    if (ai_clean("What the hell is that?", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why)) return 0;
    if (ai_clean("Pip was here, {player}!", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why)) return 0;
    if (ai_clean("Hey {name}, look!", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why)) return 0;
    return 1;
}
static void chatter_install(void) {
    if (!chatter_selftest()) { printf("chatter: self-test FAILED, Mii chatter off%c", 10); return; }
    if (g_main.text_size != 0x256d250) { printf("chatter: Mii chatter is for game version 1.0.4 only so far%c", 10); return; }      /* (1.0.0: its pick is still to be found) */
    printf("chatter: Mii chatter hook %s (graphics.ini mii_chatter = %s)%c", hook_install(0x71004bbb20ull, chatter_pick, NULL, &g_chatter_pick) ? "installed" : "NOT installed",
           gfx_mii_chatter() ? "on" : "off", 10);
    if (gfx_mii_chatter() || getenv("TOMO_CHATTER")) { AcquireSRWLockExclusive(&g_cm_lk); cm_learn(); kb_load_file(); ReleaseSRWLockExclusive(&g_cm_lk); ai_start(); }
}
