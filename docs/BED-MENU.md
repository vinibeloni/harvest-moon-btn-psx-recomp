# The bed (diary) menu — what was measured, and why "Save without sleeping" is not a text edit

Session of 2026-10-18, Windows build, USA disc SLUS-01115. Everything here was read
from the running game (RAM dumps, write traces, screenshots) and from the overlay
images in `build/overlay_static.json`; nothing is guessed. Written so the idea can be
picked up later without redoing the investigation.

**The idea.** Add a menu item to the farmhouse bed menu that opens the game's own
save screen without sleeping — the opposite of what the game offers (it only saves
after sleeping). Keeps the game's look, needs no hotkey, and avoids the Select/MAP
problem of `save-anywhere`.

**Status: the mechanism is proven; the extra line is not built.** Decision pending.

---

## 1. Verified: the bed menu

The menu lives in overlay member 11 (`event_s.bin`, window A, base `0x800B7E20`).
Facing the bed and pressing Cross opens it. Four options, one text object:

```
 Sleep without making
 diary entry
 Make diary entry and
 then sleep
 Read diary
 Exchange animals
```

| What | Address | Notes |
|---|---|---|
| Menu function (draw + input) | `0x800FFB08` | returns `idx + 1` when an option is chosen, `-1` on cancel, `0` while open |
| Caller / state machine | `0x800FFA64` | state halfword `0x801273DA`: `0` active, `0xFE` closing, `0xFF` idle |
| Cursor index | `0x801273D8` (halfword) | 0..3; Down `+1`, Up `+3`, wrapped by `andi v0,v0,3` at `0x800FFC2C` |
| Option text | `0x80053A22` + `*(0x80054BBC)` | the offset is 0 for the USA disc (a language selector) |
| Panel | call `0x80022580`, width `0xAC`, height `0x58` | the height constant is the `addiu` at `0x800FFB68` |
| Cursor y | `0x18 + 14 * line` | `0x800FFC34..0x800FFC74`; options 0 and 1 occupy 2 lines each |
| Selection result | `0x8012CEF8` | `idx + 1`; the caller closes the menu for `-1,1,2,3` |

**What each option does** (code at `0x800FFCE4..0x800FFDA4`):

- Option 0: `jal 0x800CE210(0x80071A00)` — the sleep routine.
- Option 1: `jal 0x8001AF70(0x48, 1)` (sets event flag 72 = "make a diary entry"), then the
  same sleep routine.
- Option 2 ("Read diary"): `0x8009B490 = 0xF`, then the common tail.
- Option 3 ("Exchange animals"): `0x8009B490 = 0x11`, then the common tail.
- Common tail: `0x80070CC4 = 0`, `0x800132F0(0xA, 0, 2)`, `0x8001FE34(0xA)`.

`0x8009B490` is a **scene id**, not a game mode. Writing `7` there gives a black screen
(scene 7 does not exist) — tried, do not repeat.

### Text encoding

16-bit little-endian units: `A..Z = 0x00..0x19`, `a..z = 0x20..0x39`, space `0x0FFD`,
newline `0xFFFE`, string end `0xFFFF`. A leading space in each option leaves room for the
cursor. The Start-menu glyphs are further codes (`[41]`, `[42]` are button icons).
The option text is one run of 99 units (198 bytes) from `0x80053A22`.

### The Start menu is not a list

Start in the field opens five information pages (farm summary, amount shipped, helpers,
earnings/expenses, tool levels) switched with Left/Right. Nothing there is selectable.

## 2. Verified: the game's own save screen can be opened from the bed menu

Replacing the six words at `0x800FFD48..0x800FFD5C` (the Option 2 block) turned
"Read diary" into "open the memory card save screen":

| Address | Stock | Patched | Meaning |
|---|---|---|---|
| `0x800FFD48` | `0x3C028008` | `0x3C028006` | `lui v0,0x8006` |
| `0x800FFD4C` | `0x2442AFF0` | `0x24040007` | `addiu a0,zero,7` |
| `0x800FFD50` | `0x3C030002` | `0xAC44E39C` | `sw a0,-0x1C64(v0)` → `0x8005E39C = 7` |
| `0x800FFD54` | `0x00431021` | `0xAC40E3A0` | `sw zero,-0x1C60(v0)` → `0x8005E3A0 = 0` |
| `0x800FFD58` | `0x2404000F` | `0x00000000` | nop |
| `0x800FFD5C` | `0xAC4454A0` | `0x00000000` | nop |

That is exactly what `SetMode(7)` writes (the same two words `save-anywhere` writes).
Result in the running game: choosing the option shows "Select a MEMORY CARD slot"
(mode 7); aborting ("Aborting save. Is that OK?" → Yes) returns to the farmhouse in mode
15, with no map open and no hang. **I did not complete a real save**, to leave the
memory card alone; a real save from this path is still to be tested (it is the same
screen and the same mode word `save-anywhere` already saves with).

`[[patch]]` entries inside this overlay applied and stuck (`hmbtn.patchengine.applied`
counted all six), so the engine does patch overlay code that is resident.

## 3. Why an extra (fifth) line does not fit

A fifth option needs: more text, a cursor that wraps over 5 entries instead of 4, a taller
panel (`0x58` → `0x66`), and a new action. Code words can be reclaimed by rewriting the
dispatch compactly; the text is the blocker. The new line needs about 46 more bytes and
**there is no safe place to put it**:

- The strings next to the option text are all referenced. `0x80053AE8` ("How to operate /
  Configuration") is loaded by `0x800FEE74`; `0x80053B22` by `0x800FF05C`; `0x80053CB8` by
  `0x800FF3A0`.
- The big zero run at `0x80056574` (3,416 bytes) is not free: code refers into it
  (`0x80056A54`, `0x80057254`, `0x8005720E`...).
- The 4.7 KB gap `0x8012B5E8..0x8012C838` in the overlay's variable area has no direct
  reference, but a write trace showed it is a work buffer: `0x800FBFC4..0x800FBFD0` clears
  all of it every time the Start menu opens.
- `0x80010000..0x80011278` (before the entry point) has no zero run at all.
- The only large zero run in member 11 is `0x80128808` (22 KB), its own variables.
- Rewording does not help: the panel holds about 21 characters per line, and the five
  options need 122 units against the 99 available.
- `docs/FRUSTUM.md` §5 already proves the 2 MB has no free block.

Method, to redo it: dump RAM in 64 KB chunks with `read_ram`; list zero runs; scan every
code word of the main image and of each overlay (`overlay_static.json`) for `lui`+offset
pairs and for pointer words landing in the run; then `wtrace_add` the candidate and play
through the Start menu, the map and the bed menu.

## 4. The ways forward (pick one)

1. **Replace "Exchange animals" with "Save without sleeping".** Same-length text edit plus
   the Option 3 block above, in 5–8 `[[patch]]` words. Cost: the animal exchange (used
   with a second diary) is gone. Safe and small.
2. **A hidden button in the bed menu** (e.g. Triangle), with the help window below the
   menu ("Directional buttons / Move cursor / X button: Select / O button: Close diary")
   edited to say so. Keeps all four options. Needs about ten reclaimed code words and a
   second look at what Triangle does elsewhere (unclear). More work, more fragile.
3. **Real fifth option using the framework's 8 MiB RAM** (`psx_mod_set_main_ram_8mb`,
   `psxrecomp/docs/MOD_PACKAGES.md`). Gives free memory for text, but it changes the
   machine geometry for the whole game, savestates record the size, and the recompiled
   code's assumptions about RAM mirroring are untested. Highest risk.

## 5. Notes that cost time

- Walking to the bed by blind D-pad presses is unreliable (isometric camera, obstacles,
  and the character can leave the house). Have a person park the character at the bed.
- The raw pad word given to `press` / `set_input` is active low: Down `0xFFBF`, Up `0xFFEF`,
  Left `0xFF7F`, Right `0xFFDF`, Cross `0xBFFF`, Circle `0xDFFF`, Start `0xFFF7`. Menus
  and the title need about 20 frames. A second Cross while the bed menu is open **selects**
  an option — it slept through a day once.
- The game's pad pointer `0x8005E3A8` alternates buffers; read the pointer and the word
  back to back (one connection) or the sample is of the stale buffer.
- Two Claude sessions can share the game process and the debug port (4370): one's
  `taskkill` or `set_input` shows up as the other's character walking away or the game
  returning to the title. Coordinate before using the game.
