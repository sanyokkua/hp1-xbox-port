# Changelog

## Unreleased

Engine fixes from the Harry Potter Chamber of Secrets port:

- Renderer: textures with border addressing get the stage's own border colour (the fragment shader returns
  `SET_TEXTURE_BORDER_COLOR` outside [0, 1]); Metal samplers only have black or white borders.
- Controller: **LB is White, RB is Black** (they were swapped; as on the original controller and in xemu). The keyboard keeps
  Q = White, E = Black. A press shorter than one of the game's frames is still seen, and held for at least two reads.
- Runtime: the file handle table is locked (two threads opening files at once could share a slot); a read on a handle that
  was just closed ends the file instead of failing as a disc error; an event set hands the CPU to the thread it wakes, as
  on the Xbox (a lost wake-up hung CoS at a loading screen).
- Renderer: no copy of a render target that stays bound on a stage no shader reads (CoS: 24 -> 60 fps).
- Log: one `[metal]` line each for border addressing, W-buffer depth and fog-unit use.
  HP1 uses neither W-buffer depth nor the fog unit (both not ported).
- Runtime: a skipped wild indirect call at a site whose caller pops its own arguments (`add esp, N`) pops only the
  return address (esp was left N bytes high). Timers signal before their DPC and inside the timer lock, and a set resets
  inside it (a lost wake-up); 256 timer slots, a log when none is free. Reads, writes and waits hold their object, so a
  close on another thread no longer frees it in use; a handle closes once; handle slots are handed out next-fit (65536);
  a read or write at an offset is one `pread`/`pwrite` (two threads on one handle got each other's bytes). A Ke object
  shadow whose memory now holds the guest's own event is dropped. The heap refuses a size larger than itself (it rounded
  to 0).
- Lifter: 38 functions the disassembler missed are seeded. The lifter had given them empty bodies, so callers that
  jumped to a shared exit (`pop edi; pop esi; ret`) got their stack and registers back wrong. `tools/stubcheck.py`
  (run by the new `tools/relift.sh`) fails when a stub is real code.
- Controller: the app no longer aborts when a controller disconnects. sdl2-compat keeps its pad list without a lock, and
  the game's thread looked for pads again while the main thread did the same. Pads are now opened and closed on the main
  thread only; the game's thread just reads them.

## 0.1.1 (2026-11-23)

- Lifter: x87 80-bit operands (`fld`/`fstp` of a 10-byte value) were read as 4-byte floats and not stored at all. The C
  runtime's asin/acos load π/2 that way, which in the Chamber of Secrets port turned the player 90° when the stick pointed
  straight along an axis. Now converted exactly (`recomp_ld_f80` / `recomp_st_f80`); 21 loads and 4 stores in this game.

## 0.1.0 (2026-11-22)

First release: the Xbox USA disc of *Harry Potter and the Sorcerer's Stone*, built on the player's Mac.

- Base: the engine of the NFS Underground 2 port (nfsu2-sw with nfsu2-ios's Apple host and Metal renderer, NFSU2Deadware's Mac
  patches), with its NFSU2-only parts removed; this game's facts from HP-PS-Xbox-Recomp.
- The game: the logos, the intro, the menus, New Game through Diagon Alley, the Hogwarts arrival and the first missions at
  Hogwarts; saves and loading; controller (with rumble) and keyboard.
- Engine fixes for this game: one thread object and id per game thread (the loader's paths), x87 compare flags for `lahf` (black
  cutscenes), functions hidden behind jump tables seeded, fixed-function lighting and pre-lit rooms on Metal, a surface no longer
  rebuilt mid-frame (black menus and message boxes), projected shadows (texture generation, the texture matrix, no copies behind
  the light), the kernel import scan.
- In-game settings menu (Back + Start, F1 or Cmd+,): Display, Graphics (anti-aliasing, anisotropic filtering, bloom and ambient
  occlusion, gamma), Audio, Input (vibration, keyboard), Tweaks and Advanced. Tweaks: Skip Logo Movies, Skip Cutscenes (story
  cuts skipped, scenes inside a level fast-forwarded and silent), Skip Save Messages (no pause on the storage messages).
- A radial dead zone on the sticks (pads that rest off centre no longer push the camera or Harry).
- Everything the port is built from is in the repository (`engine/`): no download at build time.
- The app keeps everything in `~/Library/Application Support/Harry Potter Sorcerer's Stone`; logs in `logs/`.
- Installer DMG (no game code): imports the disc image, checks `default.xbe` by its section digests (images differ in a few
  media-patched bytes), lifts and compiles the game with Apple's Command Line Tools, puts the app in Applications. macOS 15+.
