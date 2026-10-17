/* hmbtn_patchengine.c -- drop-in data mods for Harvest Moon: Back to Nature.
 *
 * A mod is a folder <exe dir>/mods/hmbtn/<name>/ holding one file, mod.toml.
 * Put the folder there and the mod is active; remove it and it is gone. No
 * rebuild, no regen, no overlay recompile.
 *
 * mod.toml is a small TOML subset:
 *
 *     name = "Example"                  # name / version / author / description
 *
 *     [[patch]]
 *     address  = 0x800FFD2C             # KSEG0, word aligned
 *     expected = 0x24020001             # the stock instruction word
 *     replace  = 0x24020002
 *     note     = "why"
 *
 * Every patch is guarded: the word is replaced only while RAM still holds
 * `expected`, so a mod written for other bytes does nothing instead of
 * corrupting the guest. Gameplay code lives in overlays that the game loads
 * after boot and swaps in and out, so the guard is re-checked every VBlank and
 * the patch is re-applied whenever its overlay becomes resident again.
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
#define HMBTN_MAX_FILE     (1u << 20)
#define HMBTN_PATH_CAP     1024u

/* The main EXE and the overlay windows all sit inside KSEG0 RAM above the
 * kernel area; nothing a mod needs lies outside it. */
#define HMBTN_ADDR_LO      0x80010000u
#define HMBTN_ADDR_HI      0x80200000u

typedef struct HmbtnPatch {
    uint32_t address;
    uint32_t expected;
    uint32_t replace;
} HmbtnPatch;

static HmbtnPatch g_patches[HMBTN_MAX_PATCHES];
static uint32_t   g_patch_count;

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

/* A value is either a quoted string (accepted, contents unused) or an
 * unsigned integer in any strtoul base (0x... or decimal). */
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

static int patch_valid(const HmbtnPatch *p)
{
    return (p->address & 3u) == 0 &&
           p->address >= HMBTN_ADDR_LO && p->address < HMBTN_ADDR_HI &&
           p->expected != p->replace;
}

/* Parse one mod.toml. Returns 1 and keeps its patches, or 0 and keeps none. */
static int parse_mod(char *text)
{
    const uint32_t start = g_patch_count;
    HmbtnPatch cur;
    unsigned have = 0;       /* bit 0 address, 1 expected, 2 replace */
    int in_patch = 0;
    int ok = 1;

    memset(&cur, 0, sizeof cur);

    char *line = text;
    while (ok && line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        strip_comment(line);
        char *s = trim(line);
        line = next;
        if (!*s) continue;

        if (strcmp(s, "[[patch]]") == 0) {
            if (in_patch) {
                if (have != 7u || !patch_valid(&cur) ||
                    g_patch_count >= HMBTN_MAX_PATCHES) { ok = 0; break; }
                g_patches[g_patch_count++] = cur;
            }
            memset(&cur, 0, sizeof cur);
            have = 0;
            in_patch = 1;
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) { ok = 0; break; }
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (in_patch) {
            if (strcmp(key, "address") == 0) {
                ok = parse_u32(val, &cur.address);  have |= 1u;
            } else if (strcmp(key, "expected") == 0) {
                ok = parse_u32(val, &cur.expected); have |= 2u;
            } else if (strcmp(key, "replace") == 0) {
                ok = parse_u32(val, &cur.replace);  have |= 4u;
            } else if (strcmp(key, "note") == 0) {
                ok = is_quoted(val);
            } else {
                ok = 0;
            }
        } else {
            if (strcmp(key, "name") == 0 || strcmp(key, "version") == 0 ||
                strcmp(key, "author") == 0 || strcmp(key, "description") == 0) {
                ok = is_quoted(val);
            } else {
                ok = 0;
            }
        }
    }

    if (ok && in_patch) {
        if (have != 7u || !patch_valid(&cur) ||
            g_patch_count >= HMBTN_MAX_PATCHES) ok = 0;
        else g_patches[g_patch_count++] = cur;
    }

    if (!ok) g_patch_count = start;
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
    g_patch_count = 0;
    scan_mods();
}

/* Apply every guarded patch whose bytes are currently resident. Overlays are
 * reloaded from disc by the game, which restores the stock word, so the
 * comparison against `expected` is what makes this re-apply correctly. */
static void on_vblank(void)
{
    for (uint32_t i = 0; i < g_patch_count; i++) {
        const HmbtnPatch *p = &g_patches[i];
        if (psx_mod_read_word(p->address) == p->expected) {
            psx_mod_write_code_word(p->address, p->replace);
            psx_mod_counter_add("hmbtn.patchengine.applied", 1);
        }
    }
}

PSX_MOD_CONSTRUCTOR(register_hmbtn_patchengine)
{
    psx_mod_register_activation_plugin(HMBTN_ENGINE_ID, on_activate);
    psx_mod_register_vblank_plugin(HMBTN_ENGINE_ID, on_vblank);
}
