# TomodachiLife-recomp: development log

Static recompilation of *Tomodachi Life: Living the Dream* (Switch, title 010051F0207B2000, v0) to a native PC
program, built on the tools and runtime of [ssbu-recomp](../ssbu-recomp) (OpenSmash-Ultimate).

No game code, assets or keys go in this repository. Everything derived from the game is made from your own dump into
`build/` (ignored by git).

## Status: feasibility study done (2026-10-21)

| Check | Result |
|---|---|
| ExeFS extracted with `ssbu-recomp/tools/extract/dump_folder.py` | `main` (code 0x25ce6a0), `sdk`, `rtld`, no subsdk |
| SDK | NintendoSDK 20.5.3 (SSBU is far older) |
| Engine | Nintendo EPD stack: `sead`, `agl`, `xlink`, NintendoWare G3d2 / Ui2d / Font / Vfx2, Havok 2023.2 physics, NVN graphics |
| Lifter coverage (`recomp.py cover`) | 71,289 functions; 99.66% of words translated. The other 0.34% are almost all zero padding (32,796 words); about 1,150 real SIMD / misc instructions are not handled yet |
| SDK imports by `main` | 779; 337 are also imported by SSBU, **442 are new** (219 `nn::audio`, 46 `nn::hid`, 35 `nn::os`, 25 `nn::prepo`, 19 `nn::swkbd`, 14 `nn::mii`, ...). List: `build/imports_missing.txt` |

## Step 1: lifter coverage and full lift (2026-10-21)

The lifter is a copy of `ssbu-recomp/tools/lifter/recomp.py` and `recomp_eh.py` (commit 8323f2b), with
`runtime/neon.h`, `runtime.h`, `fpexact.h` and `eh_runtime.h` from the same commit. Changes:

- **Instructions added** (`recomp.py`, helpers at the end of `runtime/neon.h`): ORR / BIC (vector, immediate),
  FMAXP / FMINP / FMAXNMP / FMINNMP (vector and scalar), FACGE / FACGT, the three-different long forms (S/U ADDL, ADDW,
  SUBL, SUBW, ABAL, ABDL, MLAL, MLSL, MULL and their "2" forms), SQADD / UQADD / SQSUB / UQSUB, SABD / UABD / SABA /
  UABA, MLA / MLS (vector), CLZ / CLS (vector), SHLL, SQXTN / UQXTN / SQXTUN, the saturating shift-right-narrow
  family (SQ(R)SHRUN, SQ(R)SHRN, UQ(R)SHRN), FRINTN/M/P/Z/A/X/I and FCVTNS/NU/MS/MU/PS/PU/AS/AU (vector), LD2R / LD3R /
  LD4R. XTN2 now writes the upper half (SSBU's `v_xtn` ignored the "2" form). Saturation does not set FPSR.QC.
- **Every real instruction of `main` is now translated.** What is left is zero padding and trap words after calls
  that do not return (`guest_unimpl` if ever reached).
- **Differential test** (`difftest.py`, Unicorn as reference): 38,598 tests, all identical, including every new form.
- **Mid entries share their host's body** (`recomp_eh.py`): recomp.py lifts a branch target inside another function
  as a separate copy of the rest of that function. A few huge functions here have hundreds of such targets, which made
  3.0 GB of C (one 509 MB file). Now they are resume points of one `u_<host>(c, rp)` body, the same scheme the
  exception landing pads use: 795 MB, biggest file 2.3 MB, 40,160 mid entries in 7,927 hosts (all checked to have
  their `case` and label).
- Lift: 111,510 functions, 807 files. The game has no C++ exceptions (no `__cxa_throw` import, 0 LSDAs).
- `tools/lifter/compile_main.py` compiles the lift with clang 9 -O1 (falls back to -O0 for a file over 15 minutes).
  **All 809 files compile at -O1** (530 s, 12 jobs, 216 MB of objects; no file needed -O0).
- Link check (`lld-link -dll -noentry -force:unresolved`): a 158 MB DLL whose only unresolved symbols are the C math
  and memory functions (`memcpy`, `floor`, `fma`, ...) and the runtime's `call_indirect`, `host_import`, `guest_trap`,
  `guest_unimpl`, `stx_diag`. The SSBU runtime provides all of them. The 779 SDK imports go through `host_import`.
- Not tested yet: running any lifted function. The mid-entry change has the same shape as the landing pads SSBU runs,
  but nothing has executed it in this game yet.

## Steps 2-4: SDK lift, runtime, boot (2026-10-21/22)

The whole SSBU runtime and build tooling are forked into this tree (`runtime/`, `tools/`, `scripts/`); the game-specific
answers live in `runtime/tomo.c`. What works:

- **The game boots and runs its frame loop at 30 fps for as long as it is left running** (tested 4 minutes): nninitStartup,
  4,599 static constructors, nnMain, 40 game threads, NVN device / queue / window, the RomFS (66,343 files), fonts, the
  audio renderer (a silent one: wave buffers are consumed at the sample rate, nothing is mixed yet), save data, controller.
- **It draws its own loading screen** (the yellow pattern with the cycling food icon) with the OpenGL back end
  (`SSBU_RENDERER=gl`, the default of `scripts/dev_run.sh`; the Vulkan back end lacks `nvnCommandBufferReportCounter`).
- Lifter: jump-table bases are no longer taken for function starts; DT_RELR relocations; `ctr_el0`, cache maintenance.
- NVN 55 differences handled: API version 55.15, 16-byte `NVNtextureView`, `Bind*Deferred`, `MemoryPoolMapVirtual`
  (whole-pool aliases; partial mappings are copied: a shortcut), window origin / depth mode set after start-up, command
  memory modelled at 4 bytes per command, texture storage from the runtime's own layout.
- SDK 20 services: light semaphores / message queues, timer events, the nn::os memory heap, SdkMutex / SdkConditionVariable
  cut to host locks, ~300 pure SDK functions lifted (prepo, mii values, vibration, png/jpeg, the nn::audio client).

**Where it is (2026-10-22, afternoon): in the game world, through its intro.** The loading screen, the "demo save data
detected" dialog (OK), "transfer the demo save?" (Don't Transfer, then its confirmation) and the date-and-time check
(Yes, Proceed) are passed; the game loads its world, shows the island in the sea ("You've found a tiny island", OK),
the "take a look around the island" prompt (cursor on the left stick, camera on the right: both move), then its intro
dialogs ("Let's invite some folks to live here!", "There are no rules!"...), at the game's 30 fps cap. The scripted
path (poll-counted presses, nnhid.c; `@draws:` waits for the world's draw count): `SSBU_SEQ="900:Z 500:l 60:Z 500:r 60:Z
800:Z 600:r 60:Z @draws:280-420/30 300:Z 500:H/60 100:F/60 100:T/60 100:G/60 100:r/60 100:l/60 100:u/60 100:Z 300:Z
..."` with `scripts/ps_run.ps1` (Z = A, X = B, u/d/l/r = left stick, T/F/G/H = right stick, I/J/K/L = d-pad).

Host bugs fixed on the way, each one a hang:

- **Lost event wake-up** (`os_threads.c`): the scene start-up signals a manual-clear light event and clears it at once
  (`SignalLightEvent` / `ClearLightEvent` in the task-group step at 0x71005e1f40). The host event woke the waiter, which
  re-checked `signaled`, found it cleared and slept again, so the Scene Prepare Thread never left `WaitLightEvent`
  (0x71004aee60). Events now carry a signal generation: a waiter released by a signal returns even if a clear followed.
- **GPU hang** (`mx_translate.c`): program #5401 (the cube map array mip chain) ends its vertex shader in a BRX jump
  through a table in the shader's constant buffer 1; the value read at run time differs from the table the translation
  saw, the computed target is no case of the dispatcher and the shader looped for ever. The driver reset the GPU after
  2 s (event log 4101) and ended the process with 0xC0000409, silently. A BRX target outside its table now ends the
  stage. Also: each stage's constant block is copied when the program is set (`stage_keep_cdata`, the game reuses that
  memory) and the shader cache keys carry a translator salt.
- **Layered render targets** (`nvn_gl.c`): one face / layer of a cube map array or 2D array as a colour target was a
  one-layer array view, a layered attachment next to the plain 2D depth target (GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS):
  now a 2D view of that level and layer (`rt_view`); 3D textures attach by layer. A draw that samples the texture it
  renders to samples a refreshed copy (`feedback_guard`).
- **A mutex born locked** (`os_threads.c`): the game destroys its GPU memory pools while holding their mutex
  (0x71002ab68c); the host object keyed by that address kept the lock, and the next pool made at the same address
  started locked by a thread that had moved on: the world load hung in 0x7100329cc4. `InitializeMutex` /
  `FinalizeMutex` (and the SDK mutex resets) drop such a lock.

**Rendering and speed (2026-10-22, late):** the world runs at the game's 30 fps cap (from 14). Translated since:
`TEX.B` (bindless handle in a register, through a hash table of resident GL handles per texture / sampler id pair that
the back end fills as the game asks for handles), `RED`, `SHFL`, `FSWZADD`, `VMNMX`; the compute programs' code (their
instructions start at data + control +0x708, 0x100 in all 61 of the game's; until 1.0.1 they were started at +0x880, in
the middle). Per-frame costs cut: textures in CPU_NO_ACCESS pools are
GPU-owned (the game remakes its render targets every frame: 30 MB of uploads and hashing a frame gone), write watch on
the game's heap, no game-thread barrier for texture finalize / pool registration, no GPU sync after a dispatch. The
shaders of earlier runs are compiled before the game starts behind a "preparing shaders" screen with a bar (text-only
loading screen; `SSBU_PREPARE=0` skips it); shaders first met during play still hold a present while they compile.
`nvn_perf` (every 300 frames) now lists the costliest command types, the jobs the game thread waits for and the barriers
by caller. The retire barrier (buffer / pool finalize) stays, without it the sea foam drew wrong.

**The driver crash (2026-10-22, afternoon):** every run died on the render thread some 2,400 frames into the island,
inside `glDeleteTextures` (the crash report now names the hitch scope and lists the textures freed last; the minidump
reader `mdscan.py` showed nvoglv64 jumping through a garbage pointer). The game makes its render targets anew every
frame, a dozen a frame, and deleting and recreating their GL textures, views, bindless handles and framebuffers each
frame is what the driver could not take. Two changes: finalized GPU-written textures are kept as spares and the next
texture of the same shape takes their GL objects over (`tex_take_spare`: 2,100 textures made in a 5 minute run instead
of 29,000, the framebuffer cache then hits); and no framebuffer ever names a texture at the time it is deleted (cache
entries are detached and their names reused instead of deleted; the static blit / clear / present framebuffers are
detached after each use). Views of decoded (ASTC) textures name the decoded storage format (RGBA8; the levels are decoded by astc_decode.comp on the GPU, nvn_gl_astc.c, or in software with TOMO_GPU_ASTC=0). Not touched yet:
audio output, rumble, the Vulkan back end. Visible gaps: dialog text has an offset blue copy under lines with
highlighted words; the island's buildings are flat black blocks; thin vertical lines near signs.

Diagnostics: `TOMO_SAMPLE=1` (1 ms function sampler of every guest thread and the main thread, report at exit, and a
thread / wait report after 3 s without a present), `TOMO_GPUTIME=<ms>` (every draw and dispatch finished and timed; with
`TOMO_GPUTIME_EVERY=<n>` only every n-th; the last 256 commands are kept in `gputime_last.txt` through a file mapping
that survives a GPU reset), `scripts/ps_run.ps1` (runs the game from PowerShell with the real exit code: under the bash
`timeout` wrapper a driver reset showed as exit 127 and a truncated log), `TOMO_EVLOG=1` (events waited on for over 2 s and who signals them), `TOMO_TASKLOG=1` (scene task states),
`TOMO_HEAPLOG=1|<heap name>`, `TOMO_ALIASLOG=1`, `TOMO_TEXFREE=<bits>` (1 / 2 / 4: keep base textures / views /
resident handles instead of freeing them), `SSBU_GLDEBUG=1`, `SSBU_PEEK=<addr,...>` in the crash report,
`tools/debug/*.py` (disassemble main or sdk by address / name, callers, relocation of a slot, cross-references, host
code at a DLL RVA). `scripts/dev_build.sh [dll]` stages and rebuilds without running; `scripts/ps_run.ps1` pins the
process directory to `build/work` (PowerShell's Set-Location does not move it, and the exe then lost `../cache`: the
shader preparation waited for a pair list that never loaded). Screenshot folders (`SSBU_SHOT_DIR`) must exist.

## Reproduce

Everything below is in `scripts/dev_run.sh` (rebuild what changed and run for N seconds):
```
sh scripts/regen_sdk.sh                     # after a change of tools/lifter/sdklift_roots.txt
sh scripts/dev_run.sh run.log 60 dll        # dll = relink the game DLL too; log in build/work/run.log
```
First build from the dump:
```
set SSBU_PROD_KEYS=<your prod.keys>
python ..\ssbu-recomp\tools\extract\dump_folder.py <game.nsp> <game.nsp> build\dump --exefs-only
python ..\ssbu-recomp\tools\extract\nso2elf.py build\dump\exefs\main build\dump\exefs\main.elf
set RECOMP_BIN=build\dump\exefs
cd tools\lifter
python recomp.py cover
python difftest.py sample && python difftest.py gen
clang -O0 -w -fno-strict-aliasing -I ..\..\runtime difftest_gen.c difftest_host.c -o ..\..\build\difftest.exe
set DIFFTEST_EXE=..\..\build\difftest.exe
python difftest.py check
python recomp_eh.py ..\..\build\gen_eh 16
python compile_main.py ..\..\build\gen_eh ..\..\build\obj 12
```

(`python` is `..\ssbu-recomp\deps\python\python.exe`, `clang` is `..\ssbu-recomp\deps\llvm\bin\clang.exe`, clang 9.)

The keys must cover key generation 0x0f or newer, since this game is newer than the keys in some emulator folders.

## State at 1.0.1

Windows (OpenGL on NVIDIA, Vulkan on other cards), Linux and Android (Vulkan). What was learned in the last round, for whoever
picks this up:

- **Renderer choice:** `gfx_options.renderer` (`auto` | `always_vulkan` | `opengl`), `nvn_renderer_is_vk`. `--renderer` prints
  the choice; `SSBU_GPU_VENDOR=other` simulates a non-NVIDIA card. AMD's OpenGL driver crashes or runs at 1 fps on this game.
- **Vulkan on other GPUs** can be simulated on an NVIDIA card: `SSBU_VK_TEST` bits 8 (no D24), 512 (no vertex input dynamic
  state), 1024 (no extended dynamic state 3, so no pipeline libraries); `SSBU_VK_PUSH=0`, `SSBU_VK_NO_BC=1`, `SSBU_VK_VALIDATE=1`.
  Run them on a copy of `build/cache/vk`: records of one state are replayed by the next run.
- **The frame:** the game remakes ~22 render targets every frame (a frame graph: new texture objects over the same memory),
  submits one big command list and presents. `vpool_*` in `nvn_vk_draw.c` reuses their images. The game cannot start the next
  frame before the replay of this one has finished, so the replay's length is the frame time.
- **Write watch:** one scan per batch; chunks nothing asked about for ~1000 scans leave it (`ww_chunk_sweep`). A kept range or
  texture is trusted only when the scan applied last asked for its chunks (`ww_chunk_trusted`).
- **Measuring:** other programs on the same PC change the numbers by 30 %. Compare inside one run: a switch with a value 2
  flips every 300 frames (`SSBU_VK_IMGPOOL=2`, `SSBU_VK_RINGKEEP=2`, `SSBU_VK_FASTDRAW=2`), then join the `nvn_vk_perf` and
  `nvn_vk: frames` lines by window.
- **Test sequences:** `SSBU_SEQ="600:Z @untilmark:island:Z/200 <polls>"` reaches the island; `TOMO_ISLE_GOTO="<poll>:x,z ..."` glides
  its camera (the hand must be up: `150:l/2` steps keep it up); `TOMO_MOUSE_TEST`, `TOMO_SAVE_TEST=<poll>`, `TOMO_MENU_TEST=<frame>`.
- **Audio:** `TOMO_AUDIOLOG=5` logs every voice's gain ten times a second and the first samples of each sound (searching those
  in the `.bars` archives and streams names the sound). The game's sound engine is `alto`: attenuation curves in
  `Sound/Attenuation`, leak-out of buildings in `SoundLeakOutParam`. Passed through: the compressor (only the `Voice_Speaker` bus).
- **Save:** the save manager's request flag: `tomo.c` `save_now_step`. Its autosave counts at +0x948.
- **Code mods:** `runtime/tomo_mod.h`, [MODDING.md](MODDING.md).
- **Known gaps of the port against its own bar:** the PC Settings page opens by key, not from inside the game's own Settings
  screen; arm64 and real AMD / Intel / Steam Deck hardware were not available for testing.
