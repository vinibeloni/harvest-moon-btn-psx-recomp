# Reuse survey: six PS1 / retro ports, and what "ready for general use" needs

Six projects were cloned shallow (`--depth 1`, 2026-10-25) outside this repo, in
`D:\Documents\projects\_ref\`, and read by theme. Every claim below cites a file that
was opened. What was not read is said at the end.

**The short answer.** None of the six is a static recompilation like this one. They are
rewrites (ReChan, wipeout-rewrite), decompilations run on an HLE layer (REDRIVER2,
ctr-native, RSDKv4) or a matching decomp (sotn-decomp). Their PS1 layers (Psy-X,
ctr-native `platform/native_*`) replace what `psxrecomp/runtime` already does, so
**there is no code worth porting**. What is worth taking is how they turn a working
build into something a stranger can download and run: disc validation on first launch,
a user config file, a release zip with a checksum, CI, and Linux packaging.

## 1. The six projects

| Project | Commit | License | What it is | Can we copy code? |
|---|---|---|---|---|
| SilverwireGames/ReChan | `5029a50` | MIT | C++ reimplementation of Jackie Chan Stuntmaster (SLUS-00684); HOR+ widescreen, mouse, mods | Yes, with the MIT notice |
| OpenDriver2/REDRIVER2 | `b2d8857` | MIT | Driver 2 decompiled to C, runs on Psy-X (HLE of GPU/GTE/SPU/CD) | Yes, with the MIT notice |
| CTR-tools/ctr-native | `3f5f6b8` | GPL-3.0 | Native PC port of Crash Team Racing on SDL3, static single exe | **No** (GPL into PolyForm NC). Ideas only |
| Xeeynamo/sotn-decomp | `600c917` | AGPL-3.0 | Matching decomp of SOTN (PSX/PSP/Saturn) | **No**. Ideas only |
| phoboslab/wipeout-rewrite | `b425ecb` | none declared | Rewrite of wipEout; SDL2 or Sokol backends; Emscripten | **No** (no license = all rights reserved). Ideas only |
| RSDKModding/RSDKv4-Decompilation | `a7f5195` | custom (`LICENSE.md`) | Retro Engine v4 decomp; mod API, `settings.ini`, dev menu | **No** (custom terms, e.g. DLC must be off by default). Ideas only |

## 2. Findings by theme

### 2.1 Disc validation on first launch — the biggest gap

- **ReChan** reads `SYSTEM.CNF`, reduces `BOOT=cdrom:\SLUS_006.84;1` to `SLUS-00684`
  and refuses to extract anything else
  (`ReChan/src/extra/psxdiscextractor.cpp:291-435`, "Could not verify the disc version
  ... Refusing to extract."). The user drops any `.bin`/`.iso` in `discimage/`; the file
  name does not matter (`ReChan/README.md`, *Getting Started*).
- **ctr-native** documents the exact layout it accepts: one MODE2/2352 track at byte 0,
  and explains why a 2048-byte ISO is not enough (XA/STR audio and video)
  (`ctr-native/platform/native_disc_image.c:15-25`, `package-linux.sh` README text).
- **Here:** `tools/regen.sh:20-29` takes the first `*.bin`/`*.iso`/`*.img` in the repo
  root and extracts `SLUS_011.15` from it. **Nothing checks the serial or a hash.** A
  PAL, Japanese or patched disc goes all the way to a recompile against the wrong
  binary, and the failure shows up later as a crash or a divergence.
- **Do:** in `tools/extract_psx_exe.py` (or `regen.sh`), read `SYSTEM.CNF`, require
  `SLUS_011.15`, and compare the extracted EXE's SHA-1 with
  `07fbcffbdd96080d39939e569f84abe76b9d1039` (the value published by
  beruanglaut-com/backtonature-decompiled; **verify it against our own EXE before
  committing it**). Fail with one sentence that says which disc is needed. Also
  reject a 2048-byte ISO, as ctr-native does. Small, high value.

### 2.2 User configuration

- **REDRIVER2** ships one commented `config.ini` (`REDRIVER2/data/config.ini`):
  data folder, keyboard binds per PSX button, SDL controller binds, with the list of
  valid names in the comments.
- **RSDKv4** loads `settings.ini` from the resources path, the game path or the base
  path, in that order (`RSDKv4/Userdata.cpp:226-232`), and documents a dev menu
  behind a config flag (`README.md`, *Additional Tweaks*).
- **Here:** settings are split between `game.toml` (build-time, 41 KB, and editing it
  **silently orphans the overlay cache**, see `CLAUDE.md` §3), `input.ini` and
  `keybinds.ini` (copied by `tools/make_portable.ps1`), and command-line flags
  (`--memcard-dir`). A player has no single file to edit, and the one that looks like
  a config file is the one that breaks performance.
- **Do:** document in the README which file a player may edit (`input.ini`,
  `keybinds.ini`) and which they must not (`game.toml`). A runtime config merger is
  a framework change (`psxrecomp/`), not this repo's work.

### 2.3 Release packaging

- **ReChan** `scripts/pack-win64.ps1` (25 lines): stage the exe and `res/pc`, write a
  `SHA256SUM.txt`, zip to `shipping/rechan-win64.zip`. It also has `pack-linux64.sh`.
- **ctr-native** `package-linux.sh`: refuses to run without the built binary, copies
  `LICENSE` and `THIRD_PARTY_NOTICES.md`, and writes a `README.txt` that says exactly
  where the disc goes and in which format.
- **REDRIVER2** has a Flatpak manifest (`io.github.opendriver.redriver2.yaml`, uses
  `--persist=.` for saves); **RSDKv4** has `flatpak/` with `.desktop`, AppStream
  `.appdata.xml` and an icon; **wipeout-rewrite** has a Windows `.rc`, `.ico` and
  manifest in `packaging/windows/`.
- **Here:** `tools/make_portable.ps1` (Windows folder or zip) and `tools/make_app.sh`
  (macOS `.app`) already cover most of this. Missing: a checksum file, `LICENSE` and a
  third-party notice inside the package (the OpenBIOS MIT license and the psxrecomp
  license), a Linux package, and an exe icon. The portable folder has `-IncludeDisc`;
  a **public** release must never use it (see `CLAUDE.md`, *No game data*).
- **Do:** add `SHA256SUM.txt`, `LICENSE` and a `THIRD_PARTY_NOTICES.md` to
  `make_portable.ps1` (copy ReChan's 3-line hash step, MIT). Linux packaging waits
  until the Linux build is verified.

### 2.4 Continuous integration

- **RSDKv4** `.github/workflows/build.yml`: builds every platform on push.
- **sotn-decomp** `.github/workflows/validate-and-report.yml`: builds and checks per
  version, but only on the owner's repo (`if: github.repository == ...`) because the
  game data comes from secrets.
- **REDRIVER2** uses AppVeyor (`.appveyor/Build.*`, `Install.*`).
- **Here:** no `.github/` at all. The framework has release workflows
  (`psxrecomp/.github/workflows/`). A CI job cannot regenerate the C (that needs the
  disc), but it **can** run `python3 tools/check_no_game_data.py` on every push and
  PR, and build-check the tools.
- **Do:** one workflow that runs `check_no_game_data.py`. This also protects the repo
  once other people send PRs, which the local `.githooks` gate cannot do.

### 2.5 Mods

- **ReChan** `docs/MODDING.md`: a folder per mod under `~mods`, no manifest, load order
  in `~mods/mods.ini`, later mods win, enable/disable from an in-game menu, invalid
  overrides fall back to the original asset. Asset replacement (PNG/WAV/GLB) by name.
- **RSDKv4** has a mod API in code (`RSDKv4/ModAPI.cpp`) and save redirection.
- **Here:** `mods/hmbtn/<name>/mod.toml` with guarded `[[patch]]`/`[[trigger]]`, which
  is already stricter than ReChan (every write checks the stock word). Missing compared
  to ReChan: a player-facing `docs/MODDING.md` (today the format lives only in
  `CLAUDE.md`), a defined load order and conflict rule, and a way to disable a mod
  without deleting its folder. Asset replacement does not apply: we run the original
  code on the original data.
- **Do:** write `docs/MODDING.md` from the `CLAUDE.md` section, and state the conflict
  rule (two mods patching the same word).

### 2.6 Widescreen

- **ReChan** fixes HUD placement by anchoring each overlay element to a screen edge
  (`src/extra/hudwsfix.h`, `HUDWidescreenPatches`, per-overlay `direction`), behind a
  build flag `FIX_ASPECT_RATIO`. This works because it owns the source of every HUD
  element.
- **Here:** widescreen is done from outside the game code (`docs/FRUSTUM.md`,
  `docs/RENDERING.md`). The idea of anchoring HUD elements left/right is the same, but
  the mechanism is not portable. Nothing to take beyond the idea.

### 2.7 README and first-run docs

- All six put **"you need your own disc"** and **what exactly to buy** at the top.
  RSDKv4 opens with where to buy the game; ReChan shows a folder diagram; REDRIVER2 links
  a wiki installation page; ctr-native puts the disc path in the packaged README.
- **Here:** the README starts with build instructions for a developer (`## Build`,
  `## Layout`, seeds, overlays). There is no "I just want to play" section.
- **Do:** add a *Play* section at the top: download the release, put your
  `.cue` + `.bin` beside the exe, run `Play.bat`, where saves go. Keep the developer
  sections below it.

## 3. What "ready for general use" needs, in order

Each item is its own change. Nothing here is implemented yet.

| # | Change | Where | Taken from | Size |
|---|---|---|---|---|
| 1 | Validate the disc (serial in `SYSTEM.CNF` + EXE SHA-1; reject 2048-byte ISOs) | `tools/extract_psx_exe.py`, `tools/regen.sh` | ReChan, ctr-native | small |
| 2 | *Play* section at the top of the README; disc layout diagram | `README.md` | ReChan, ctr-native | small |
| 3 | CI running `check_no_game_data.py` on push and PR | `.github/workflows/` (new) | RSDKv4, sotn-decomp | small |
| 4 | `SHA256SUM.txt`, `LICENSE`, `THIRD_PARTY_NOTICES.md` in the package | `tools/make_portable.ps1`, `tools/make_app.sh` | ReChan, ctr-native | small |
| 5 | Player-facing `docs/MODDING.md`, with load order and conflict rule | `docs/` | ReChan | small |
| 6 | Say which config files a player may edit, and that `game.toml` is not one | `README.md` | REDRIVER2, RSDKv4 | small |
| 7 | GitHub release with the Windows zip (no disc) | GitHub | all | medium |
| 8 | Linux package (AppImage or Flatpak), once a Linux build is verified | `tools/` | REDRIVER2, RSDKv4 | medium |

Decisions that belong to the owner, not to code:

- **Which repo is the public one.** `origin` is a fork of
  `JorgeNachtigall/harvest-moon-btn-psx-recomp`. Lists and users will look for one home.
- **`main` is behind.** `origin/main` is 14 commits behind `feat/drop-in-mods` (mods,
  `LICENSE`). A visitor sees the old version.
- **License.** PolyForm Noncommercial is not detected by GitHub (the repo page shows no
  license) and is not an open-source license. It is a valid choice; say it in the README.
- **The repo page** has no description and no topics.

## 4. What was not read

- Psy-X (REDRIVER2 `src_rebuild/` PSX layer) and ctr-native `platform/native_gpu.c`,
  `native_gte_core.c`, `native_audio.c` were listed, not read: they implement what
  `psxrecomp/runtime` already does, and their licenses rule out copying the GPL one.
- ReChan `docs/REFERENCE.md` (599 KB) and `docs/BUILDING.md`.
- RSDKv4 `ModAPI.cpp` beyond its existence, and its `build.yml` content.
- wipeout-rewrite source beyond `CMakeLists.txt` (Emscripten switch at line 12) and
  `packaging/`. A web build of this project is not considered: the runtime is native.
- No project was built or run.
