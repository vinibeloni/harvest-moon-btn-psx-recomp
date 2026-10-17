/* hmbtn_patchengine.c -- drop-in data mods for Harvest Moon: Back to Nature.
 *
 * A mod is a folder <exe dir>/mods/hmbtn/<name>/ holding one file, mod.toml.
 * Put the folder there and the mod is active; remove it and it is gone. No
 * rebuild, no regen, no overlay recompile.
 *
 * mod.toml is a small TOML subset with three kinds of table:
 *
 *     name = "Example"                  # name / version / author / description
 *
 *     [[patch]]                         # guarded replacement of one code word
 *     address  = 0x800FFD2C             # KSEG0 RAM, word aligned
 *     expected = 0x24020001             # the stock word
 *     replace  = 0x24020002
 *     note     = "why"
 *
 *     [[trigger]]                       # a button combination, once per press
 *     modes   = [2, 15, 19]             # fire only while the game's mode word is one of these
 *     buttons = 0x010C                  # all of these pad bits held (Select|L1|R1)
 *     note    = "why"
 *
 *     [[require]]                       # belongs to the [[trigger]] above it;
 *     address = 0x80127208              # every one must hold or the press is refused
 *     mask    = 0xFFFFFFFF              # optional, default all bits
 *     equals  = 1                       # (word & mask) == equals
 *
 *     [[write]]                         # belongs to the [[trigger]] above it
 *     address = 0x8006C878
 *     op      = "or"                    # "set" or "or"
 *     value   = 0x100
 *
 * Every patch is guarded: the word is replaced only while RAM still holds
 * `expected`, so a mod written for other bytes does nothing instead of
 * corrupting the guest. Gameplay code lives in overlays that the game loads
 * after boot and swaps in and out, so the guard is re-checked every VBlank and
 * the patch is re-applied whenever its overlay becomes resident again.
 *
 * A trigger fires on the VBlank where its combination becomes held and re-arms
 * when it is released. A trigger held at launch does not fire until released.
 *
 * A mod that fails to parse is rejected whole. Nothing is logged to a file or
 * to stderr; observe the engine through the mod_counters debug command
 * (hmbtn.patchengine.*).
 */
#include "mod_plugins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#define HMBTN_ENGINE_ID    "hmbtn.patchengine"
#define HMBTN_MAX_PATCHES  8192u
#define HMBTN_MAX_TRIGGERS 64u
#define HMBTN_MAX_WRITES   512u
#define HMBTN_MAX_REQUIRES 512u
#define HMBTN_MAX_FILE     (1u << 20)
#define HMBTN_PATH_CAP     1024u

/* The main EXE and the overlay windows all sit inside KSEG0 RAM above the
 * kernel area. Triggers write data (flags, latches) that can live lower, so
 * their range starts at the bottom of RAM. */
#define HMBTN_PATCH_LO     0x80010000u
#define HMBTN_WRITE_LO     0x80000000u
#define HMBTN_ADDR_HI      0x80200000u

/* The game's mode word, and the pointer to its pad struct. Measured in the
 * running game by holding buttons: word +0 is an auto-repeat pulse (zero for a
 * held direction on most frames), word +4 is the held buttons (active high,
 * stable), word +8 is the newly-pressed edge. Held is therefore +4. */
#define HMBTN_MODE_ADDR     0x8005E39Cu
#define HMBTN_PAD_PTR_ADDR  0x8005E3A8u
#define HMBTN_PAD_HELD_OFFS 4u

typedef struct HmbtnPatch {
    uint32_t address;
    uint32_t expected;
    uint32_t replace;
} HmbtnPatch;

enum { HMBTN_OP_SET = 0, HMBTN_OP_OR = 1 };

typedef struct HmbtnWrite {
    uint32_t address;
    uint32_t value;
    uint32_t op;
} HmbtnWrite;

/* A condition on one RAM word: (word & mask) == equals. */
typedef struct HmbtnRequire {
    uint32_t address;
    uint32_t mask;
    uint32_t equals;
} HmbtnRequire;

typedef struct HmbtnTrigger {
    uint32_t mode_mask;     /* bit n set = the trigger may fire in game mode n */
    uint32_t buttons;
    uint32_t first_write;
    uint32_t write_count;
    uint32_t first_require;
    uint32_t require_count;
    int      armed;
} HmbtnTrigger;

static HmbtnPatch   g_patches[HMBTN_MAX_PATCHES];
static uint32_t     g_patch_count;
static HmbtnTrigger g_triggers[HMBTN_MAX_TRIGGERS];
static uint32_t     g_trigger_count;
static HmbtnWrite   g_writes[HMBTN_MAX_WRITES];
static uint32_t     g_write_count;
static HmbtnRequire g_requires[HMBTN_MAX_REQUIRES];
static uint32_t     g_require_count;

static int exe_dir(char *out, size_t cap)
{
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) return 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &size) != 0) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = 0;
#endif
    char *slash = strrchr(out, '/');
    char *back  = strrchr(out, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (!slash) return 0;
    *slash = 0;
    return 1;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = (char *)malloc(HMBTN_MAX_FILE + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, HMBTN_MAX_FILE + 1, f);
    fclose(f);
    if (n > HMBTN_MAX_FILE) { free(buf); return NULL; }
    buf[n] = 0;
    return buf;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    *end = 0;
    return s;
}

/* Cut a trailing "# comment" that is not inside a quoted string. */
static void strip_comment(char *line)
{
    int quoted = 0;
    for (char *p = line; *p; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == '#' && !quoted) { *p = 0; return; }
    }
}

/* Unsigned integer in any strtoull base (0x... or decimal), no quotes. */
static int parse_u32(const char *v, uint32_t *out)
{
    if (!*v || *v == '"') return 0;
    char *end = NULL;
    unsigned long long x = strtoull(v, &end, 0);
    if (end == v || *end != 0 || x > 0xFFFFFFFFull) return 0;
    *out = (uint32_t)x;
    return 1;
}

static int is_quoted(const char *v)
{
    size_t n = strlen(v);
    return n >= 2 && v[0] == '"' && v[n - 1] == '"';
}

/* "[2, 0x0F, 19]" -> bitmask of those mode numbers (each 0..31, at least one). */
static int parse_mode_mask(char *v, uint32_t *out)
{
    size_t n = strlen(v);
    if (n < 3 || v[0] != '[' || v[n - 1] != ']') return 0;
    v[n - 1] = 0;
    uint32_t mask = 0;
    for (char *tok = v + 1; tok; ) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = 0;
        uint32_t m;
        if (!parse_u32(trim(tok), &m) || m > 31u) return 0;
        mask |= 1u << m;
        tok = comma ? comma + 1 : NULL;
    }
    *out = mask;
    return mask != 0;
}

/* "text" -> text, in place. Returns NULL unless v is a quoted string. */
static char *unquote(char *v)
{
    if (!is_quoted(v)) return NULL;
    v[strlen(v) - 1] = 0;
    return v + 1;
}

static int patch_valid(const HmbtnPatch *p)
{
    return (p->address & 3u) == 0 &&
           p->address >= HMBTN_PATCH_LO && p->address < HMBTN_ADDR_HI &&
           p->expected != p->replace;
}

static int write_valid_addr(uint32_t a)
{
    return (a & 3u) == 0 && a >= HMBTN_WRITE_LO && a < HMBTN_ADDR_HI;
}

static int write_valid(const HmbtnWrite *w)
{
    return write_valid_addr(w->address);
}

enum { SEC_TOP = 0, SEC_PATCH, SEC_TRIGGER, SEC_WRITE, SEC_REQUIRE };

typedef struct ParseState {
    int          section;
    HmbtnPatch   patch;
    unsigned     patch_have;      /* bit 0 address, 1 expected, 2 replace */
    HmbtnTrigger trig;
    unsigned     trig_have;       /* bit 0 mode, 1 buttons */
    HmbtnWrite   wr;
    unsigned     wr_have;         /* bit 0 address, 1 op, 2 value */
    HmbtnRequire rq;
    unsigned     rq_have;         /* bit 0 address, 1 equals (mask is optional) */
    int          have_trigger;    /* a [[trigger]] is open for [[write]]/[[require]] */
} ParseState;

/* Close whichever table is open, committing it to the global arrays. */
static int flush_section(ParseState *ps)
{
    switch (ps->section) {
    case SEC_PATCH:
        if (ps->patch_have != 7u || !patch_valid(&ps->patch) ||
            g_patch_count >= HMBTN_MAX_PATCHES) return 0;
        g_patches[g_patch_count++] = ps->patch;
        break;
    case SEC_TRIGGER:
        if (ps->trig_have != 3u || ps->trig.mode_mask == 0 ||
            ps->trig.buttons == 0 || ps->trig.buttons > 0xFFFFu ||
            g_trigger_count >= HMBTN_MAX_TRIGGERS) return 0;
        ps->trig.first_write = g_write_count;
        ps->trig.write_count = 0;
        ps->trig.first_require = g_require_count;
        ps->trig.require_count = 0;
        ps->trig.armed = 0;
        g_triggers[g_trigger_count++] = ps->trig;
        ps->have_trigger = 1;
        break;
    case SEC_REQUIRE:
        if (!ps->have_trigger || ps->rq_have != 3u || !write_valid_addr(ps->rq.address) ||
            g_require_count >= HMBTN_MAX_REQUIRES) return 0;
        g_requires[g_require_count++] = ps->rq;
        g_triggers[g_trigger_count - 1].require_count++;
        break;
    case SEC_WRITE:
        if (!ps->have_trigger || ps->wr_have != 7u || !write_valid(&ps->wr) ||
            g_write_count >= HMBTN_MAX_WRITES) return 0;
        g_writes[g_write_count++] = ps->wr;
        g_triggers[g_trigger_count - 1].write_count++;
        break;
    default:
        break;
    }
    return 1;
}

/* Parse one mod.toml. Returns 1 and keeps its entries, or 0 and keeps none. */
static int parse_mod(char *text)
{
    const uint32_t p0 = g_patch_count, t0 = g_trigger_count, w0 = g_write_count,
                   r0 = g_require_count;
    ParseState ps;
    int ok = 1;

    memset(&ps, 0, sizeof ps);

    char *line = text;
    while (ok && line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        strip_comment(line);
        char *s = trim(line);
        line = next;
        if (!*s) continue;

        if (s[0] == '[') {
            int sec;
            if      (strcmp(s, "[[patch]]")   == 0) sec = SEC_PATCH;
            else if (strcmp(s, "[[trigger]]") == 0) sec = SEC_TRIGGER;
            else if (strcmp(s, "[[write]]")   == 0) sec = SEC_WRITE;
            else if (strcmp(s, "[[require]]") == 0) sec = SEC_REQUIRE;
            else { ok = 0; break; }
            if (!flush_section(&ps)) { ok = 0; break; }
            if (sec == SEC_TRIGGER) ps.have_trigger = 0;
            ps.section = sec;
            ps.patch_have = ps.trig_have = ps.wr_have = ps.rq_have = 0;
            memset(&ps.patch, 0, sizeof ps.patch);
            memset(&ps.trig,  0, sizeof ps.trig);
            memset(&ps.wr,    0, sizeof ps.wr);
            memset(&ps.rq,    0, sizeof ps.rq);
            ps.rq.mask = 0xFFFFFFFFu;
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) { ok = 0; break; }
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);

        switch (ps.section) {
        case SEC_PATCH:
            if      (!strcmp(key, "address"))  { ok = parse_u32(val, &ps.patch.address);  ps.patch_have |= 1u; }
            else if (!strcmp(key, "expected")) { ok = parse_u32(val, &ps.patch.expected); ps.patch_have |= 2u; }
            else if (!strcmp(key, "replace"))  { ok = parse_u32(val, &ps.patch.replace);  ps.patch_have |= 4u; }
            else if (!strcmp(key, "note"))     { ok = is_quoted(val); }
            else ok = 0;
            break;
        case SEC_TRIGGER:
            if      (!strcmp(key, "modes"))   { ok = parse_mode_mask(val, &ps.trig.mode_mask); ps.trig_have |= 1u; }
            else if (!strcmp(key, "buttons")) { ok = parse_u32(val, &ps.trig.buttons); ps.trig_have |= 2u; }
            else if (!strcmp(key, "note"))    { ok = is_quoted(val); }
            else ok = 0;
            break;
        case SEC_WRITE:
            if      (!strcmp(key, "address")) { ok = parse_u32(val, &ps.wr.address); ps.wr_have |= 1u; }
            else if (!strcmp(key, "value"))   { ok = parse_u32(val, &ps.wr.value);   ps.wr_have |= 4u; }
            else if (!strcmp(key, "op")) {
                char *op = unquote(val);
                if      (op && !strcmp(op, "set")) ps.wr.op = HMBTN_OP_SET;
                else if (op && !strcmp(op, "or"))  ps.wr.op = HMBTN_OP_OR;
                else ok = 0;
                ps.wr_have |= 2u;
            }
            else if (!strcmp(key, "note")) { ok = is_quoted(val); }
            else ok = 0;
            break;
        case SEC_REQUIRE:
            if      (!strcmp(key, "address")) { ok = parse_u32(val, &ps.rq.address); ps.rq_have |= 1u; }
            else if (!strcmp(key, "equals"))  { ok = parse_u32(val, &ps.rq.equals);  ps.rq_have |= 2u; }
            else if (!strcmp(key, "mask"))    { ok = parse_u32(val, &ps.rq.mask); }
            else if (!strcmp(key, "note"))    { ok = is_quoted(val); }
            else ok = 0;
            break;
        default:
            if (!strcmp(key, "name") || !strcmp(key, "version") ||
                !strcmp(key, "author") || !strcmp(key, "description")) {
                ok = is_quoted(val);
            } else {
                ok = 0;
            }
            break;
        }
    }

    if (ok && !flush_section(&ps)) ok = 0;

    /* A trigger that wrote nothing is a mistake, not a no-op. */
    for (uint32_t i = t0; ok && i < g_trigger_count; i++)
        if (g_triggers[i].write_count == 0) ok = 0;

    if (!ok) {
        g_patch_count   = p0;
        g_trigger_count = t0;
        g_write_count   = w0;
        g_require_count = r0;
    }
    return ok;
}

static void load_mod(const char *mods_dir, const char *name)
{
    char path[HMBTN_PATH_CAP];
    if ((size_t)snprintf(path, sizeof path, "%s/%s/mod.toml", mods_dir, name)
            >= sizeof path) {
        psx_mod_counter_add("hmbtn.patchengine.mods_rejected", 1);
        return;
    }
    char *text = read_file(path);
    if (!text) return;      /* a folder without mod.toml is not a mod */
    if (parse_mod(text)) psx_mod_counter_add("hmbtn.patchengine.mods_loaded", 1);
    else                 psx_mod_counter_add("hmbtn.patchengine.mods_rejected", 1);
    free(text);
}

static void scan_mods(void)
{
    char base[HMBTN_PATH_CAP];
    char mods_dir[HMBTN_PATH_CAP];
    if (!exe_dir(base, sizeof base)) return;
    if ((size_t)snprintf(mods_dir, sizeof mods_dir, "%s/mods/hmbtn", base)
            >= sizeof mods_dir) return;

#ifdef _WIN32
    char pattern[HMBTN_PATH_CAP];
    if ((size_t)snprintf(pattern, sizeof pattern, "%s/*", mods_dir)
            >= sizeof pattern) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        load_mod(mods_dir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(mods_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        load_mod(mods_dir, e->d_name);
    }
    closedir(d);
#endif
}

static void on_activate(void)
{
    g_patch_count = g_trigger_count = g_write_count = g_require_count = 0;
    scan_mods();
}

/* Held buttons of the first pad, or 0 when the pad pointer is not valid RAM. */
static uint32_t held_buttons(void)
{
    uint32_t ptr = psx_mod_read_word(HMBTN_PAD_PTR_ADDR);
    if ((ptr & 3u) != 0 || ptr < HMBTN_WRITE_LO ||
        ptr + HMBTN_PAD_HELD_OFFS >= HMBTN_ADDR_HI) return 0;
    return psx_mod_read_word(ptr + HMBTN_PAD_HELD_OFFS) & 0xFFFFu;
}

/* Every [[require]] of the trigger must hold: (word & mask) == equals. */
static int requires_hold(const HmbtnTrigger *t)
{
    for (uint32_t i = 0; i < t->require_count; i++) {
        const HmbtnRequire *r = &g_requires[t->first_require + i];
        if ((psx_mod_read_word(r->address) & r->mask) != r->equals) return 0;
    }
    return 1;
}

static void run_writes(const HmbtnTrigger *t)
{
    for (uint32_t i = 0; i < t->write_count; i++) {
        const HmbtnWrite *w = &g_writes[t->first_write + i];
        if (w->op == HMBTN_OP_OR)
            psx_mod_write_word(w->address, psx_mod_read_word(w->address) | w->value);
        else
            psx_mod_write_word(w->address, w->value);
    }
}

/* Apply every guarded patch whose bytes are currently resident, then run the
 * triggers. Overlays are reloaded from disc by the game, which restores the
 * stock word, so the comparison against `expected` is what makes the patches
 * re-apply correctly. */
static void on_vblank(void)
{
    for (uint32_t i = 0; i < g_patch_count; i++) {
        const HmbtnPatch *p = &g_patches[i];
        if (psx_mod_read_word(p->address) == p->expected) {
            psx_mod_write_code_word(p->address, p->replace);
            psx_mod_counter_add("hmbtn.patchengine.applied", 1);
        }
    }

    if (g_trigger_count == 0) return;
    const uint32_t held = held_buttons();
    const uint32_t mode = psx_mod_read_word(HMBTN_MODE_ADDR);
    for (uint32_t i = 0; i < g_trigger_count; i++) {
        HmbtnTrigger *t = &g_triggers[i];
        const int down = (held & t->buttons) == t->buttons;
        if (down && t->armed) {
            /* One decision per press: a press refused by the mode or a
             * condition is spent, so holding it until the condition clears
             * does not fire later. */
            t->armed = 0;
            if (mode < 32u && ((t->mode_mask >> mode) & 1u) && requires_hold(t)) {
                run_writes(t);
                psx_mod_counter_add("hmbtn.patchengine.triggered", 1);
            } else {
                psx_mod_counter_add("hmbtn.patchengine.blocked", 1);
            }
        } else if (!down) {
            t->armed = 1;
        }
    }
}

PSX_MOD_CONSTRUCTOR(register_hmbtn_patchengine)
{
    psx_mod_register_activation_plugin(HMBTN_ENGINE_ID, on_activate);
    psx_mod_register_vblank_plugin(HMBTN_ENGINE_ID, on_vblank);
}
