# How the port was made

A log of the steps, the decisions and what went wrong. It follows the NFS Underground ports
([nfsu2-xbox-port](https://github.com/sanyokkua/nfsu2-xbox-port), [nfsu-xbox-port](https://github.com/sanyokkua/nfsu-xbox-port)),
which were made first with the same engine; this file says where Harry Potter differs. A PS2 port of the same game was tried
before this one; its recompiler's instruction bugs (sticks, floors, timing) made the Xbox version, on a proven stack, the better
path.

## Sources (pinned)

| What | Where | Pin |
|---|---|---|
| Engine, overlay menu, installer | [sanyokkua/nfsu2-xbox-port](https://github.com/sanyokkua/nfsu2-xbox-port) | `d23c105` (v0.1.0): xboxrecomp, nfsu2-sw and nfsu2-ios (`85d2cbf`), NFSU2Deadware's patches |
| This game's facts | [MiddleDucky/HP-PS-Xbox-Recomp](https://github.com/MiddleDucky/HP-PS-Xbox-Recomp) (MIT, Windows, D3D8 on D3D12) | `f475af9c` |
| Disc | Xbox USA, `default.xbe` title `0x45410037`, XDK 5659, entry `0x153DD1`, `.text` 1,570,612 bytes | section digests in `mac/xbe_check.py` |

HP-PS-Xbox-Recomp plays the game from start to finish on Windows. Its hand-written addresses are for the **European** executable:
in the library sections after `.text` (D3D, DSOUND, XPP, data) the USA address is the European one + 0x20; `.text` functions
shift by -0x5E0..+0x8E0, so they were matched by bytes.

## 1. The disc

- An XISO (partition at 0): `default.xbe` (3.2 MB), 75 `.hog` archives, 18 `.xmv` movies, `disk.txt`; 1.8 GB.
- As on the NFS Underground disc, the image's `.text` doesn't match its own stored digest (a media patch by the tool that made
  the image), so the check compares the title, each section's size and stored digest, and every other section's data
  (`mac/xbe_check.py`, 13 sections).

## 2. Lift, build, the XDK glue

- **Lift:** the NFS Underground 2 port's `regen.sh` with HP1's executable: 59 s, about 6,000 functions, 27 C files. The XDK
  functions the engine hooks (memmove, `D3D_BlockOnTime`, the vblank and PGRAPH interrupt handlers, the DSOUND DSP post, the
  AC'97 reset) were found in HP1's executable by masked byte signatures of the same functions in the NFSU1 and NFSU2 executables
  (`tools/sigmatch.py`; both agreed, each hit unique). Linked the first time.
- **NFSU2-only parts removed** from the engine: the Switch and iOS hosts, the VP6 movie player (HP1 decodes its own XMV movies),
  the HUD mover, the network card, the text patch, the texture and prompt packs.

## 3. Bring-up, in the order the bugs appeared

Each was a black screen, a hang or a crash.

1. **Cutscene stuck on its loading screen:** the loader built every path as `\Device\CdRom0\(null)a001_0001.banr`. XAPI keeps a
   per-thread table keyed by the thread id; the runtime gave every thread one shared thread object and no id. Now each thread has
   its own object and id (`xbox_memory_layout.c`, `kernel_bridge.c`).
2. **Black cutscene after loading:** `fucompi; lahf; test ah, 0x44; jp` picks "draw the scene or black". The lifter built `lahf`
   only after SSE compares. Now x87 compares set the flags `lahf` reads.
3. **Menu invisible:** its per-frame callback sits right after a jump table, which the disassembler read as code, so the
   function was missing (`[ICALL] Failed to resolve VA 0x00023BD0` every frame). Seeded (`engine/config/seed_functions.json`).
   Later crashes of the same kind (Ollivanders, the Flipendo scene) were fixed the same way; one bad seed in the middle of an
   instruction cut a function in two and blacked out the Hogwarts arrival for 90 s: check every seed is an instruction start.
4. **The controller never opened:** replaced XAPI's XInput with HP-PS-Xbox-Recomp's direct design, fed from the runtime's pad
   report (so the keyboard, scripted input and the menu's neutral pad still apply). Rumble completes at once: the game waited
   forever the first time Harry was hit.
5. **White walls, then black rooms:** the Metal renderer had no fixed-function lighting (the NFS games use vertex programs).
   Added the chip's lighting per vertex, and the emission from the vertex colour for HP1's pre-lit rooms.
6. **Black Remembrall, hint boxes and save screens:** the game drew them, but the renderer rebuilt and cleared the screen surface
   mid-frame (the 3D scene and the UI came with different clip widths). Now a surface is reused or grown with its pixels kept.
7. **Tiled shadows, then mirrored ones:** HP1 projects character shadows with texture-coordinate generation and a texture
   matrix, which the renderer passed through as plain coordinates (a copy of the shadow on every floor plank). Both are in the
   vertex shader now; behind the projector (q <= 0) the lookup falls outside the texture, so no mirrored wedges.
8. **The kernel import scan overwrote code:** the scan before the import table took the XPP section's bytes for 7 imports. Only
   ordinals below 512 now: 110 of 110 imports bridged.
9. **x87 80-bit operands (2026-11-23, from the Chamber of Secrets port):** `fld xword ptr [m80]` was lifted as a 4-byte
   float and `fstp m80` as a no-op. The CRT's asin/acos load π/2 that way for |x| = 1 (≈ 0 instead), which in CoS turned
   Harry 90° when the stick pointed straight along an axis. The lifter now emits `recomp_ld_f80` / `recomp_st_f80`
   (`recomp_types.h`), with a unit test; in HP1 21 loads and 4 stores changed (`/* fld tbyte */`, `/* fstp tbyte */`).
   Relift, build and `tools/smoke.sh` pass.

10. **Engine fixes from the Chamber of Secrets port (2026-11-23, released in 0.1.2):** From `hpcos-xbox-port` (its notes:
   XBOX360_PORTING_NOTES.md section 14), commits a324956, e448ada, 84cc945, 582dc61, 0013c4f, 8e7050a:
   - **Texture border colour in the shader** (CoS `ca756e0`): border addressing (mode 4) samples clamp-to-edge and the
     fragment shader returns the stage's `SET_TEXTURE_BORDER_COLOR` outside [0, 1] (`nv2a_samp`, `Nv2aMslFs.bc/bm`). A
     `[metal] border addressing` line names each stage and colour a title uses. Checked: New Game to Ollivanders and walking
     there, and `HP1_START_LEVEL=20` (Hogwarts, Harry on the floor): no stage with border addressing in these scenes; the
     frames before and after match (differences only where the run's timing differed). The old sampler gave opaque white or
     black for border colours with alpha >= 0x80, which the shader now returns exactly.
   - **LB = White, RB = Black** and the **press latch** (CoS `dcc5179`, `12f88fc`): the bumpers were swapped in
     `xinput_device.c` in every port of this engine; the keyboard keeps Q = White, E = Black (`apple_keyboard.c`). Presses
     are latched from SDL's controller events between two reads (`RECOMP_PAD_LATCH=0` off) and held for
     `RECOMP_PAD_MIN_READS` reads (default 2). `RECOMP_PAD_SCRIPT` does not go through SDL events, so scripted runs do not
     exercise the latch; needs a pad in a play test.
   - **Runtime** (CoS `dcc5179`, `f38f328`, `cf6d9c2`): the handle table under a spinlock; a read on a handle closed within
     2 s returns `STATUS_END_OF_FILE`; `NtSetEvent` waits (2 ms at most) until the waiter it releases has queued for the
     guest lock (`RECOMP_WAKE_HANDOFF=0` off, `RECOMP_EVT_TRACE=1`).
   - **No feedback copy** for a render target bound on a stage no combiner reads (CoS `9178756`, `nv2a_psh_reads_tex`).
   - **Not ported:** HP1 sets neither CONTROL0 bit 16 nor the fog unit in the scenes run, so the W-buffer depth, the AO's
     linear mode and the fog unit are not ported. The InsertCallback latch hold is not needed: this tree has no clear-value
     latch (only CoS's XDK 5028 calls back through it). Viewport constants 58/59 from `SET_VIEWPORT_*`: this XDK writes the
     constants itself. The worker wait, loader callbacks, file-cache hold, DirectSound address tag, 2D tile seams, cutscene
     skip and left-stick range are CoS's own code or tuning.
   - Verified: `cmake --build build/release`; `tools/smoke.sh` OK; the Metal shader test (408 shader pairs, 8 recorded by
     the run, 0 failures); frame throughput as before. The runtime's own unit tests (`engine/xboxrecomp/tests/kernel_*`)
     need `pkg-config`, not installed on this Mac: not run.

11. **Runtime lifetime fixes and the stub check** (CoS `58773a1`, `33a2b93`, the check from `6ce58e0`, 2026-11-24):
    - A skipped wild indirect call pops only the return address when the call site is followed by `add esp, N`
      (`recomp_icall_skip_esp`, all four `RECOMP_ICALL_SAFE*` failure paths); before, esp ended N bytes high.
    - Timers signal inside `g_timer_lock` and before the DPC; `KeSetTimer` resets inside the lock; 256 slots, a log when
      full. Handle tokens are handed out next-fit (65536 slots) on top of this tree's locked table (`84cc945`): a late
      read on a closed token now finds no handle instead of the next file. A Ke shadow whose memory reads as the guest's
      own event/semaphore is dropped; KeInitializeEvent/Mutant/Semaphore insert the shadow after the header.
    - Win32 objects: `obj_get` takes a reference; open handles counted apart from references; `CloseHandle` drops one
      handle once; `DuplicateHandle` adds one; registry 262144 slots with a log. NtReadFile/NtWriteFile hold the fd
      (`w32_handle_get_fd`/`w32_handle_put`) and use `pread`/`pwrite` at an offset. The heap refuses sizes larger than
      itself. Not ported: CoS's factory region (0x104), spawner guard, entity diagnostics.
    - **Stub check:** `tools/stubcheck.py` (`.text` range read from the XBE) classes every stub in
      `recomp_stubs_unresolved.c`; `tools/relift.sh` (new here: regen + check + build) fails on real code. HP1 had
      **26 of 40 stubs** at real instruction starts; 3 more are data (a 0x08-byte table in `.text`, a D3DX table) reached
      only from misdecodes, listed with a note in `engine/config/stub_not_code.json`. Real ones included shared exits
      (0x14DA67 `pop edi; pop esi; ret`, and `ret` at 0xC6540, 0x189478), the CoS freeze class. 38 seeds in 3 rounds
      (each split turns jumps across it into new stubs: epilogues at 0x80525, 0x104641, 0x105221, 0x10D914, 0x10F281);
      now 17 stubs, 0 real. Seeds added with `tools/addseed.py`.
    - Verified: relift + build; `tools/smoke.sh` OK (75 s); a 150 s scripted New Game (`build/script_floor.txt`) into
     Diagon Alley (hpm01) with clean exit and no wild calls, in a throwaway HOME.

12. **Pad hotplug abort** (CoS `e64d110`, 2026-11-24): the app aborted in `SDL_NumJoysticks` (`pointer being freed was not
    allocated`) when a controller disconnected: `xbox_InputGetState` -> `xbox_InputInit` rescanned on the game's thread while
    sdl2-compat refreshed the same unlocked pad list on the main thread. `xbox_input_rescan()` now opens and closes pads on
    the main thread (at start and on JOYDEVICEADDED/REMOVED, `apple_main.c`); the game's thread reads them under `g_pad_lock`
    and no longer calls `SDL_GameControllerUpdate`; rumble takes the lock. Stress switch `NFSU2_PAD_HOTPLUG_TEST=<ms>`
    attaches and detaches a virtual pad. Verified: `tools/smoke.sh` OK; `NFSU2_PAD_HOTPLUG_TEST=37 tools/smoke.sh 120`
    OK (about 1,600 cycles, no abort).

## 3a. Backports from the PoA port (2026-11-24)

- Disasm `_prune_data_call_targets`: relift with it, `functions.json` identical (6082 starts, same 17 stubs): no start dropped
  or added. Disasm tests: 62 passed.
- Kernel: `XC_FACTORY_AV_REGION` (0x103) = 0x00400100. This build is fixed to the NTSC-US disc (`region_profile.h`), so there is
  no region to pick. `XC_FACTORY_GAME_REGION` (0x104) is still unanswered here (PoA and CoS answer 1); not changed, no symptom.
- `tools/stubcheck.py` range: `.text` plus D3D, XGRPH, DSOUND, XMV, D3DX, XPP (raw data only). 17 stubs, 0 real code: nothing to seed.
- `tools/smoke.sh 75`: OK, 5 frames, last mean 50.

## 4. Port features

- **Settings menu** (the NFS ports' overlay): HP1's rows in `engine/src/overlay/hp1_options.c`. Bloom needed a lower "this is a 3D
  scene" threshold (HP1's rooms have 80-170 draws, the NFS games' 150+): `NFSU2_POST_MIN_DRAWS=40`.
- **Skip Logo Movies** (from HP-PS-Xbox-Recomp): the movie player returns at once for the logos; the boot reads only a flag,
  which stays as if the movie ran to its end.
- **Skip Cutscenes:** the game's own cuts (`cut/*.hog`) have a skip check that the override answers "pressed" once per cut (the
  arrival cut asks twice per update). Scenes inside a level (letterboxed level scripts) have no skip: they run 8 game ticks per
  frame, muted, with lines of speech stopped. The wand shuffle in Ollivanders steps per drawn frame and gets an 8x step too. A
  try at an 8x step for everything per frame sent the game back to the title at the end of Diagon Alley and was reverted. Each
  fast-forward logs its length; over 4 s it names the script command that held it.
- **Skip Save Messages:** the storage checks hold each step at least 500 or 750 ms in busy loops on a stopwatch (as on the
  console); the stopwatch answers "long enough" to exactly those loops (matched by their bytes).
- **Stick dead zone:** pads rest off centre (a tester's at -1800 / +800 of 32767); a radial dead zone of ~11% with the rest
  rescaled to full range (`engine/xboxrecomp/src/input/stick_deadzone.h`, unit test in `engine/xboxrecomp/tests/stick_deadzone`).

## 5. A self-contained repository and the installer

As in the NFS Underground ports: `engine/` holds everything, `tools/kit.py` collects the lifter and the compiled runtime (no game
code) into the installer, `mac/build_game.sh` lifts, compiles and links on the player's Mac, `tests/dmg_no_game_code.sh` checks the
DMG. HP1 changes: the target `hp1_recomp`, the `HP1_` installer variables, the disc check, and the test's game file patterns
(`*.hog`, `*.xmv`). The test's 64-byte code windows now skip padding: HP1's executable sections hold zero runs that match any
binary (SDL, Capstone) and gave false failures; a DMG with a 400 KB slice of the game's code still fails it.

## Verification

- Play tests by the owner on an M1 Pro with a gamepad: New Game through Diagon Alley, the Hogwarts arrival and the first missions,
  saves and loading; each bug above came from a play test or its recording and was reproduced before the fix.
- Scripted runs (`RECOMP_PAD_SCRIPT` with buttons and sticks, `HP1_START_LEVEL`, loading saves from the title menu) for every fix:
  the arrival skipped in about a second, Ollivanders' scene in 1.7 s, the camera turned by the right stick at both frame rates.
- `tools/smoke.sh`: 75 s, the menu opened and closed, the last frame not black: OK.
- `tools/make_dmg.sh`: DMG 5.2 MB, `tests/dmg_no_game_code.sh` PASS (94 disc files, 44 content windows of 3.1 MB of code).
- A headless install from the DMG into a clean HOME (`HP1_NO_UI=1 HP1_IMPORT_ISO=… HP1_INSTALL_DIR=…`): copy 1.8 GB + check +
  lift + compile 27 files + link + icon = **1 min 44 s** on the M1 Pro; the installed app started from its player folder
  (`game/`, `player/`, `saves/`, `cache/`, `logs/hp1.log`), bridged 110 of 110 kernel imports, played the opening cutscenes and
  exited cleanly on the smoke timer.
- `tools/publish_check.sh`: PASS.

## Pitfalls (each cost a run)

- **A seed inside an instruction** cuts a function in two; its switch cases become fake functions without their tables.
- **Widening a time hack** ("everything per frame 8x") broke a level transition that no test reached. Keep such changes to code
  a test reaches.
- **Sampling a video at 1 fps** hid which part of a scene was slow; 3 fps around the moment showed it.
- **zsh** reads `$t:shot` as a variable modifier: write `${t}` in generated scripts.
- **An apostrophe in the game's name** ("Sorcerer's") breaks single-quoted shell and AppleScript strings: the installer builds
  those with double quotes, and `osacompile` checks them.
- **Frame dumps at 4x are 11-20 MB each:** a long run of them filled the disk and later dumps came out black. Dump sparsely and
  delete them.
- **SDL from two threads.** sdl2-compat's pad list (`SDL_NumJoysticks`) has no lock. Only the thread that pumps SDL's events
  may open, close or enumerate pads; other threads read open ones.
