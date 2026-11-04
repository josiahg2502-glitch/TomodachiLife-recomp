# Modding

Every folder in `build\mods` (made at the first start; `build-linux/mods` on Linux) is one mod. A mod can hold files, code,
or both. To turn a mod off, move its folder out.

## Files: textures, models, text, sound, gameplay data

The first `romfs` folder inside a mod lies over the game's files, in the layout made for emulators and a modded Switch
(LayeredFS): `build\mods\MyMod\romfs\...` or `build\mods\MyMod\atmosphere\contents\010051F0207B2000\romfs\...`. A file of a
mod replaces the game's file of the same path, whatever it is: textures, models (`Model\`), layouts, messages, sounds, and
gameplay parameters (`Parameter\`, `GameData\`, `AI\`, the `.bgyml` files). When two mods change the same file, the one whose
folder name comes first wins. Mods for the Switch game work as they are.

`exefs` patches cannot work: the game's code is translated ahead of time, so there is no machine code to patch. Use a plugin.

## Code: plugins

A file `plugin.dll` (Windows) or `plugin.so` (Linux) in a mod's folder is loaded before the game starts, in name order. It
exports one function:

```c
#include "tomo_mod.h"   // runtime/tomo_mod.h in the source; beside this file in a release

static TomoGuestFn g_orig;
static const TomoModApi *g_api;

static void on_function(TomoCtx *c, void *user) {
    g_api->log("called with x0 = %llx", (unsigned long long)c->x[0]);
    g_orig(c);                     // the game's own function; leave it out to replace it
}

int tomo_mod_init(const TomoModApi *api) {
    g_api = api;
    if (api->game_text_size != 0x256d250) return 0;      // addresses below are for 1.0.4
    return api->hook(0x7100241170, on_function, NULL, &g_orig);
}
```

Function addresses are those of the game's `main.elf` (`0x71xxxxxxxx`, in any disassembler); the version you read them from
must be the one running (`game_text_size` tells 1.0.0 and 1.0.4 apart). In a hook, `c->x[0..7]` are the arguments and
`c->x[0]` the result; guest addresses are host addresses, so `*(int *)(uintptr_t)c->x[0]` reads the game's memory directly;
`api->call(c, addr)` calls one of the game's functions.

Build on Windows (no C runtime needed unless you use it):

```bash
clang -shared -O2 -Iruntime -fuse-ld=lld -nostdlib -Wl,-noentry -o plugin.dll plugin.c
```

Build on Linux:

```bash
clang -shared -fPIC -O2 -Iruntime -o plugin.so plugin.c
```

A plugin runs with the game's rights and on its threads: only install plugins you trust. The Android version does not load
plugins (Android does not allow code from a writable folder); its file mods work as above, in the `mods` folder beside
`game` (`TomodachiLife/mods`).
