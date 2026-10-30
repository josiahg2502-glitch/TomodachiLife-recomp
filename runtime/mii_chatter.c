/* MII CHATTER (2026-10-30, the user's "AI mod": Miis with more to say, adaptive; included by tomo.c).
 * When you touch a Mii, the game picks what it says from BalloonTalkOneDirection.msbt (labels TTPOneDirection_F###): its
 * pick (1.0.4 0x71004bbb20: message file, the candidate labels, ...; returns {UTF-16 text, length, index | file id << 32}
 * through x8) is let through, and now and then the text is swapped for a new line written in the game's own message
 * language: the tags the game fills in itself ({200.8} the speaker's opening, {200.9} their sentence ending, {2.27} the
 * island's name, {5.0} yours, {6.5} a pause, {0.4} a new balloon, {300.2} the closing), so the game shows it and its voice
 * synthesis speaks it in that Mii's voice like its own lines (seen in the game's balloon 2026-10-30). The same pick serves
 * every balloon on the island; besides touch talk (40 % of the lines when on: replaced, or a second balloon after the Mii's
 * own) the mod takes the walk-around thoughts (Ai/Ain.msbt InsertActionShort_*, 20 %); story, activity and greeting lines
 * stay the game's. Which line it is: its label, found from the index the pick returns.
 * ADAPTIVE: it learns the island from the save and keeps what it learned in <userdata>\chatter_memory.txt: the residents
 * (Mii.sav), who moved in lately, who moved away (only the memory knows them once the save forgets), and the words you
 * taught the Miis (Player.sav). Lines are templates; the ones about the island are favoured once there is something to
 * say. A language model can write lines into the same pool later (chatter_line). graphics.ini mii_chatter = off | on (PC
 * Settings). English text only. TOMO_CHATTER=<percent> (TEST): how often, and at 100 any message file the pick reads,
 * with a log of the picks and a search for a resident's name around the caller (the speaker is not found yet: the
 * name sat two pointer steps into x2 for a room problem line, nowhere near for the walk-around thoughts). */
int gfx_mii_chatter(void);                         /* gfx_options.c */
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
    {   static int once; if (!once++) printf("chatter: knows %d residents, %d words, island \"%s\"%c", nn, g_cm.nword, g_cm.island, 10); }
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
/* Does the line hold this word (whole word, any case; stem = any ending)? */
static int ai_word(const char *s, const char *w, int stem) {
    size_t n = strlen(w);
    for (const char *p = s; *p; p++) {
        if (_strnicmp(p, w, n) || (p > s && isalpha((uint8_t)p[-1]))) continue;
        if (stem || !isalpha((uint8_t)p[n])) return 1;
    }
    return 0;
}
/* A model's line made safe for the game: one line of plain ASCII (curly quotes, dashes and ellipses straightened), no
 * markup, stage directions or other names, nothing rude (your taught words excepted); then the game's markup:
 * <o>..<e>.<h> said to you, ..<e>. thought. 0 and why when it cannot be used. */
static int ai_clean(const char *in, int kind, const char *const *names, int nnames, const char *const *ok_names, int nok,
                    const char (*words)[100], int nwords, char *out, size_t n, const char **why) {
    static const char *const banned[] = {"assistant", "language model", "as an ai", "nintendo", "tomodachi", "video game", "chatgpt", "qwen", "player", "Pip", "Captain Zoom"};
    static const char *const rude_stem[] = {"fuck", "shit", "bitch", "bastard", "nigg", "fag", "retard", "slut", "whore", "cunt", "pussy", "porn"};
    static const char *const rude_word[] = {"hell", "damn", "crap", "sex", "sexy", "kill", "die", "dead", "death", "stupid", "idiot", "hate", "dick", "drunk", "drugs", "beer", "gun", "guns", "blood"};
    char a[400]; size_t o = 0;
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
    size_t l = strlen(s), lim = kind == 2 ? 70 : 90;
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
    for (int k = 0; k < nnames; k++) if (ai_word(plain, names[k], 0)) { *why = "names a resident (the speaker is unknown)"; return 0; }
    if (kind == 2 && (ai_word(plain, "you", 0) || ai_word(plain, "your", 0) || ai_word(plain, "yours", 0) || ai_word(plain, "yourself", 0))) { *why = "a thought that talks to someone"; return 0; }
    if (kind == 2) for (const char *p = s; (p = strstr(p, "{player}")); p += 8)        /* "..., {player}!" calls to you: not a thought */
        if ((p == s && p[8] == ',') || (p >= s + 2 && p[-2] == ',' && (!p[8] || strchr(".!?,", p[8])))) { *why = "a thought that talks to someone"; return 0; }
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
    snprintf(out, n, "%s%s<e>%s%s", kind == 1 ? "<o>" : "", body, s[e] ? s + e : ".", kind == 1 ? "<h>" : "");
    return 1;
}
static void ai_push(int kind, const char *line) {
    AcquireSRWLockExclusive(&g_ai_lk);
    if (g_ai.n[kind] < 8) snprintf(g_ai.pool[kind][g_ai.n[kind]++], sizeof g_ai.pool[0][0], "%s", line);
    ReleaseSRWLockExclusive(&g_ai_lk);
}
static int ai_take(int kind, char *out, size_t n) {
    int got = 0;
    AcquireSRWLockExclusive(&g_ai_lk);
    if (kind >= 1 && kind <= 2 && g_ai.n[kind]) {
        snprintf(out, n, "%s", g_ai.pool[kind][0]);
        memmove(g_ai.pool[kind][0], g_ai.pool[kind][1], sizeof g_ai.pool[0][0] * (size_t)(--g_ai.n[kind]));
        got = 1;
    }
    ReleaseSRWLockExclusive(&g_ai_lk);
    if (got && g_ai.wake) SetEvent(g_ai.wake);
    return got;
}
static int ai_room(int kind) { AcquireSRWLockShared(&g_ai_lk); int r = g_ai.n[kind] < 6; ReleaseSRWLockShared(&g_ai_lk); return r; }
/* What the model is told this time (the user turn), from the memory; names it may use go to ok[]. */
typedef struct { char text[1200]; char names[CM_MAX][100]; int nnames; char ok[8][100]; int nok; char words[300][100]; int nwords; } AiFacts;
static void ai_facts(AiFacts *f, int kind) {
    static const char *const season[] = {"a winter", "a winter", "a spring", "a spring", "a spring", "a summer", "a summer", "a summer", "an autumn", "an autumn", "an autumn", "a winter"};
    static const char *const topics[] = {"food", "a dream", "a hobby", "the weather", "the island", "the player", "music", "the sea", "a silly worry",
        "a plan for later", "a snack", "a nap", "clothes", "friendship", "a funny thought", "the stars", "a game they made up"};
    SYSTEMTIME st; GetLocalTime(&st);
    const char *part = st.wHour < 5 ? "night" : st.wHour < 12 ? "morning" : st.wHour < 17 ? "afternoon" : st.wHour < 21 ? "evening" : "night";
    char topic[160];
    int o = 0, live = 0;
    memset(f, 0, sizeof *f);
    AcquireSRWLockShared(&g_cm_lk);
    const char *gone = NULL;
    for (int i = 0; i < g_cm.nmii; i++) {
        if (!g_cm.mii[i].left) { live++; if (f->nnames < CM_MAX) snprintf(f->names[f->nnames++], 100, "%s", g_cm.mii[i].name); }
        else if (!gone || chatter_rand() % 2) gone = g_cm.mii[i].name;
    }
    if (gone) snprintf(f->ok[f->nok++], 100, "%s", gone);
    f->nwords = g_cm.nword;
    memcpy(f->words, g_cm.word, sizeof f->words[0] * (size_t)g_cm.nword);
    const char *heard = g_cm.nheard ? g_cm.heard[chatter_rand() % (unsigned)g_cm.nheard] : NULL;
    const char *w1 = f->nwords ? f->words[chatter_rand() % (unsigned)f->nwords] : NULL;
    /* One thing at a time (the model works every fact it is given into the line): a taught word 3 in 10, someone who moved
     * away 2 in 10, something a Mii said 2 in 10, else only a topic. */
    uint32_t r = chatter_rand() % 10;
    int use_word = w1 && r < 3, use_gone = gone && r >= 3 && r < 5, use_heard = heard && r >= 5 && r < 7;
    o += snprintf(f->text + o, sizeof f->text - (size_t)o, "Facts: It is %s %s.", season[st.wMonth >= 1 && st.wMonth <= 12 ? st.wMonth - 1 : 0], part);
    if (live >= 2) o += snprintf(f->text + o, sizeof f->text - (size_t)o, " %d Miis live on {island}.", live);
    if (use_gone) o += snprintf(f->text + o, sizeof f->text - (size_t)o, " A Mii named %s moved away from {island}.", gone);
    if (use_word) o += snprintf(f->text + o, sizeof f->text - (size_t)o, " The player taught the Miis the word \"%s\".", w1);
    if (use_heard) o += snprintf(f->text + o, sizeof f->text - (size_t)o, " A Mii said lately: \"%s\"", heard);
    ReleaseSRWLockShared(&g_cm_lk);
    if (use_word) snprintf(topic, sizeof topic, "the word \"%s\"", w1);
    else if (use_gone) snprintf(topic, sizeof topic, "missing %s", gone);
    else if (use_heard) snprintf(topic, sizeof topic, "what that Mii said");
    else snprintf(topic, sizeof topic, "%s", topics[chatter_rand() % (sizeof topics / sizeof topics[0])]);
    snprintf(f->text + o, sizeof f->text - (size_t)o, " Topic: %s.\n%s", topic, kind == 1 ? "A Mii says a line to the player." : "A Mii thinks to itself while walking around.");
}
/* The fixed part of the prompt (ChatML): what Miis are like and three examples (with a made-up name, Pip, that lines may
 * not copy). */
static const char *const k_ai_turns[] = {
    "system\nYou write lines for Miis, the funny little people who live on an island in a life simulation game. A line is one "
    "or two short sentences in simple English, under 80 characters. Miis are cheerful, silly, curious and kind, a little odd, "
    "and they love food, naps, hobbies, dreams, their island and the player. The words the player taught are things Miis have "
    "heard about: they talk about them, but are never them. Miis share feelings, wishes and silly ideas, never facts about "
    "real people or things. A thought is to itself, never to anyone. Never be mean, rude, "
    "scary or sad for long. Write {player} for the player's name and {island} for the island's name. Use no other names "
    "unless the facts give them. Write only the line.",
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
};
static int ai_turn(Llm *m, int32_t *t, int max, const char *text, int open_assistant) {
    int o = 0;
    if (o < max) t[o++] = m->im_start;
    o += llm_tok(m, text, t + o, max - o);
    if (!open_assistant) { if (o < max) t[o++] = m->im_end; o += llm_tok(m, "\n", t + o, max - o); }
    return o;
}
/* One line: the facts as the user turn, then up to 40 tokens until the end of the turn or of the line. */
static int ai_write(Llm *m, int prefix, int kind, uint32_t *rng, char *raw, size_t rn, char *line, size_t ln, const char **why) {
    static AiFacts f;
    int32_t t[700];
    ai_facts(&f, kind);
    int n = ai_turn(m, t, 600, f.text, 0);
    n += ai_turn(m, t + n, 700 - n, "assistant\n", 1);
    llm_rewind(m, prefix);
    if (llm_eval(m, t, n)) { *why = "the prompt does not fit"; return 0; }
    int32_t out[48]; int no = 0, ended = 0;
    size_t ro = 0;
    raw[0] = 0;
    while (no < 40) {
        int tok = llm_sample(m, 0.8f, 40, 0.9f, 1.15f, out, no, rng);
        if (tok == m->im_end || tok == m->endoftext || tok == m->eos) { ended = 1; break; }
        char piece[64];
        int pl = llm_piece(m, tok, piece, sizeof piece);
        if (memchr(piece, '\n', (size_t)pl)) { ended = 1; break; }
        if (ro + (size_t)pl + 1 < rn) { memcpy(raw + ro, piece, (size_t)pl); ro += (size_t)pl; raw[ro] = 0; }
        out[no++] = tok;
        if (llm_eval(m, &tok, 1)) break;
    }
    llm_rewind(m, prefix);
    if (!ended) { *why = "did not finish"; return 0; }
    const char *names[CM_MAX], *ok[8];
    for (int i = 0; i < f.nnames; i++) names[i] = f.names[i];
    for (int i = 0; i < f.nok; i++) ok[i] = f.ok[i];
    return ai_clean(raw, kind, names, f.nnames, ok, f.nok, (const char (*)[100])f.words, f.nwords, line, ln, why);
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
static void ai_wait_calm(void) {
    for (;;) {
        FILETIME i0, k0, u0, i1, k1, u1;
        GetSystemTimes(&i0, &k0, &u0); Sleep(1000); GetSystemTimes(&i1, &k1, &u1);
        #define FT64(f) (((ULONGLONG)(f).dwHighDateTime << 32) | (f).dwLowDateTime)
        ULONGLONG idle = FT64(i1) - FT64(i0), all = FT64(k1) - FT64(k0) + FT64(u1) - FT64(u0);   /* (kernel time includes idle) */
        #undef FT64
        if (!all || idle * 100 >= all * 30) return;
        Sleep(2000);
    }
}
static DWORD WINAPI ai_main(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    SYSTEM_INFO si; GetSystemInfo(&si);
    int nt = getenv("TOMO_LLM_THREADS") ? atoi(getenv("TOMO_LLM_THREADS")) : (int)si.dwNumberOfProcessors - 6;
    nt = nt < 1 ? 1 : nt > 4 && !getenv("TOMO_LLM_THREADS") ? 4 : nt;
    char err[160];
    DWORD t0 = GetTickCount();
    Llm *m = llm_open(g_ai.model, nt, 1024, err, sizeof err);
    if (m) ai_wait_calm();
    int prefix = m ? ai_prefix(m) : -1;
    if (!m || prefix < 0) { printf("chatter: the AI model could not be used (%s): template lines only%c", m ? "the prompt does not fit" : err, 10); llm_close(m); g_ai.state = -1; return 0; }
    printf("chatter: AI model ready in %lu ms (%s, %d threads%s, prompt %d tokens)%c", GetTickCount() - t0, g_ai.model, m->nt, m->avx2 ? ", AVX2" : "", prefix, 10);
    g_ai.state = 2;
    uint32_t rng = GetTickCount() | 1;
    for (;;) {
        int kind = ai_room(1) ? 1 : ai_room(2) ? 2 : 0;
        if (!kind) { WaitForSingleObject(g_ai.wake, 60000); continue; }
        ai_wait_calm();
        char raw[300], line[300];
        const char *why = "";
        DWORD a = GetTickCount();
        if (ai_write(m, prefix, kind, &rng, raw, sizeof raw, line, sizeof line, &why)) {
            ai_push(kind, line);
            if (g_ai.made++ < 30) printf("chatter: AI wrote a %s in %lu ms: %s%c", kind == 1 ? "line" : "thought", GetTickCount() - a, line, 10);
        } else if (g_ai.dropped++ < 30) printf("chatter: AI line dropped (%s): %s%c", why, raw, 10);
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
/* TEST: where does a resident's name sit around the pick (up to three pointer steps from depth 0)? */
static int g_probe_hits;
static void chatter_probe(const char *path, uint64_t p, int depth) {
    if (g_probe_hits >= 24 || !chatter_readable(p, 0x200)) return;
    const uint16_t *h = (const uint16_t *)(uintptr_t)p;
    for (int off = 0; off < 0x100; off++)
        for (int i = 0; i < g_cm.nmii && h[off] && h[off] < 0x80; i++) {
            const char *nm = g_cm.mii[i].name;
            if ((uint8_t)nm[0] != h[off]) continue;
            int k = 1;
            while (nm[k] && off + k < 0x100 && h[off + k] == (uint8_t)nm[k]) k++;
            if (!nm[k] && k >= 2 && off + k < 0x100 && !h[off + k] && g_probe_hits++ < 24) printf("chatter: probe %s+0x%x = \"%s\"%c", path, off * 2, nm, 10);
        }
    if (depth < 3)
        for (int off = 0; off < 0x200; off += 8) {
            char sub[128]; snprintf(sub, sizeof sub, "%s+0x%x>", path, off);
            chatter_probe(sub, rd64(p + off), depth + 1);
        }
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
static void chatter_pick(Ctx *c, void *user) {
    uint64_t res = c->x[8], mfile = c->x[0], arg[4] = {c->x[2], c->x[3], c->x[4], c->x[5]};
    (void)user;
    g_chatter_pick(c);
    if (g_chatter_pct == -2) { const char *e = getenv("TOMO_CHATTER"); g_chatter_pct = e ? atoi(e) : -1; }
    int pct = g_chatter_pct >= 0 ? g_chatter_pct : gfx_mii_chatter() ? 40 : 0;
    if (!pct || !res || !mfile) return;
    uint64_t text = rd64(res);
    if (!text) return;
    if (!g_ai.state) ai_start();
    char label[80] = "";
    chatter_label(mfile, (uint32_t)rd64(res + 0x10), label, sizeof label);
    int kind = !strncmp(label, "TTPOneDirection", 15) ? 1 : !strncmp(label, "InsertActionShort", 17) ? 2 : 0;   /* touch talk, walk-around thought */
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
        printf("chatter: pick %lld %s (kind %d) args %llx %llx %llx: %s%c", (long long)g_chatter_picks, label, kind,
               (unsigned long long)arg[0], (unsigned long long)arg[1], (unsigned long long)arg[2], a, 10);
        if (g_chatter_picks < 6) for (int r = 19; r <= 28; r++) { char nm[8]; snprintf(nm, sizeof nm, "x%d", r); g_probe_hits = 0; chatter_probe(nm, c->x[r], 1); }   /* the caller's saved registers, two steps */
    }
    g_chatter_picks++;
    /* The game can pick the same line twice for one balloon (the greeting did): the same line gets the same answer. */
    static struct { uint64_t key; DWORD t; uint16_t *line; int len; } memo[16];
    static int nmemo;
    uint64_t key = rd64(res + 0x10);
    DWORD now = GetTickCount();
    for (int i = 0; i < 16; i++)
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
    if (kind && english && (int)(chatter_rand() % 100) < (kind == 2 ? pct / 2 : pct)) {
        static uint16_t ring[64][320];             /* the game may keep showing a line while others are picked */
        static int slot;
        char l8[600];
        line16 = ring[slot++ & 63];
        int ai = ai_take(kind, l8, sizeof l8);     /* the AI's line when one is ready, else a template */
        if (!ai) chatter_line(l8, sizeof l8, -1, kind);   /* (the speaker: not found yet, so no lines about other residents) */
        /* Half the time the Mii says its own line first and adds the new one in a second balloon ({0.4}), with the
         * closing {300.2} only at the very end as in the game's own two-balloon lines. */
        int olen = (int)rd32(res + 8), at = 0;
        if (kind == 2)                             /* a thought keeps its leading {6.x} tags (how the game shows thoughts) */
            while (at + 4 < olen && at < 32 && rd16(text + (uint64_t)at * 2) == 0x0E && rd16(text + (uint64_t)(at + 1) * 2) == 6) {
                int sz = 4 + rd16(text + (uint64_t)(at + 3) * 2) / 2;
                if (sz > 16) break;
                memcpy(line16 + at, (const void *)(uintptr_t)(text + (uint64_t)at * 2), (size_t)sz * 2);
                at += sz;
            }
        else if ((g_chatter_pct == 100 ? g_chatter_lines & 1 : chatter_rand() & 1) && olen > 0 && olen < 160) {
            memcpy(line16, (const void *)(uintptr_t)text, (size_t)olen * 2);
            at = olen;
            if (at >= 5 && line16[at - 5] == 0x0E && line16[at - 4] == 300 && line16[at - 3] == 2 && line16[at - 2] == 2) at -= 5;
            line16[at++] = 0x0E; line16[at++] = 0; line16[at++] = 4; line16[at++] = 0;
        }
        len = at + chatter_encode(l8, line16 + at, 320 - at);
        wr64(res, (uint64_t)(uintptr_t)line16);
        wr32(res + 8, (uint32_t)len);
        g_chatter_lines++;
        {   static int nlog; if (nlog++ < 40) printf("chatter: a Mii %s%s: %s%c", kind == 2 ? "thinks" : at ? "says after its own line" : "says", ai ? " (AI)" : "", l8, 10); }
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
    if (ai_clean("I love Melissa so much!", 1, names, 1, NULL, 0, words, 2, out, sizeof out, &why)) return 0;
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
    if (gfx_mii_chatter() || getenv("TOMO_CHATTER")) { AcquireSRWLockExclusive(&g_cm_lk); cm_learn(); ReleaseSRWLockExclusive(&g_cm_lk); ai_start(); }
}
