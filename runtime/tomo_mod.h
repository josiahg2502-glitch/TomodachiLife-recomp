/* Code mods for the Tomodachi Life recomp: a plugin (plugin.dll on Windows, plugin.so on Linux) in a mod's folder is loaded
 * before the game starts and may replace any of the game's functions. See docs/MODDING.md. */
#ifndef TOMO_MOD_H
#define TOMO_MOD_H
#include <stdint.h>

/* The game's registers, as the translated code keeps them: x[0..7] are the arguments and x[0] the result of a call. Guest
 * addresses are host addresses: a pointer in x[0] can be read directly. */
typedef union { uint8_t b[16]; uint16_t h[8]; uint32_t w[4]; uint64_t d[2]; float f[4]; double df[2]; } TomoV128;
typedef struct {
    uint64_t x[32];          /* x[31] is SP */
    TomoV128 v[32];
    uint32_t N, Z, C, V;
    uint64_t tpidr;
} TomoCtx;
typedef void (*TomoGuestFn)(TomoCtx *c);

typedef struct TomoModApi {
    int version;             /* TOMO_MOD_API_VERSION */
    uint32_t game_text_size; /* tells the game's versions apart: 0x25ce6a0 = 1.0.0, 0x256d250 = 1.0.4 (function addresses differ) */
    /* Replaces the function at addr (0x71xxxxxxxx, as in a disassembly of main.elf). repl runs instead of it, on the game's
     * thread; *orig calls the original, with the registers as they are in c. Returns 0 when it cannot be done. */
    int (*hook)(uint64_t addr, void (*repl)(TomoCtx *c, void *user), void *user, TomoGuestFn *orig);
    /* Calls a function of the game: arguments in c->x[0..7], the result in c->x[0]; c is the context a hook was given. */
    void (*call)(TomoCtx *c, uint64_t addr);
    /* A line in the game's log (build\work\play_*.log). */
    void (*log)(const char *fmt, ...);
    const char *mod_dir;     /* this mod's folder */
} TomoModApi;
#define TOMO_MOD_API_VERSION 1

/* Every plugin exports this. Return 0 to say it could not start (it stays loaded and does nothing more). */
#ifdef _WIN32
#define TOMO_MOD_EXPORT __declspec(dllexport)
#else
#define TOMO_MOD_EXPORT __attribute__((visibility("default")))
#endif
TOMO_MOD_EXPORT int tomo_mod_init(const TomoModApi *api);
#endif
