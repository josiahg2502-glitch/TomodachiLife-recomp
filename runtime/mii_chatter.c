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
static void chatter_pick(Ctx *c, void *user) {
    uint64_t res = c->x[8], mfile = c->x[0], arg[4] = {c->x[2], c->x[3], c->x[4], c->x[5]};
    (void)user;
    g_chatter_pick(c);
    if (g_chatter_pct == -2) { const char *e = getenv("TOMO_CHATTER"); g_chatter_pct = e ? atoi(e) : -1; }
    int pct = g_chatter_pct >= 0 ? g_chatter_pct : gfx_mii_chatter() ? 40 : 0;
    if (!pct || !res || !mfile) return;
    uint64_t text = rd64(res);
    if (!text) return;
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
    if (g_chatter_pct == 100 && !kind) kind = 1;              /* (TEST: every line, as touch talk) */
    if (kind && english && (int)(chatter_rand() % 100) < (kind == 2 ? pct / 2 : pct)) {
        static uint16_t ring[64][320];             /* the game may keep showing a line while others are picked */
        static int slot;
        char l8[600];
        line16 = ring[slot++ & 63];
        chatter_line(l8, sizeof l8, -1, kind);     /* (the speaker: not found yet, so no lines about other residents) */
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
        {   static int nlog; if (nlog++ < 40) printf("chatter: a Mii %s: %s%c", kind == 2 ? "thinks" : at ? "says after its own line" : "says", l8, 10); }
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
    return 1;
}
static void chatter_install(void) {
    if (!chatter_selftest()) { printf("chatter: self-test FAILED, Mii chatter off%c", 10); return; }
    if (g_main.text_size != 0x256d250) { printf("chatter: Mii chatter is for game version 1.0.4 only so far%c", 10); return; }      /* (1.0.0: its pick is still to be found) */
    printf("chatter: Mii chatter hook %s (graphics.ini mii_chatter = %s)%c", hook_install(0x71004bbb20ull, chatter_pick, NULL, &g_chatter_pick) ? "installed" : "NOT installed",
           gfx_mii_chatter() ? "on" : "off", 10);
    if (gfx_mii_chatter() || getenv("TOMO_CHATTER")) { AcquireSRWLockExclusive(&g_cm_lk); cm_learn(); ReleaseSRWLockExclusive(&g_cm_lk); }
}
