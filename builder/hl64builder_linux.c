/*
 * hl64builder -- builds the Half-Life 64 ROM on Linux from your own copy of Half-Life.
 *
 * Same idea as the Windows HL64Builder.exe (hl64builder.c): everything the build needs is packed
 * onto the end of this program by builder/pack.py: the N64 engine from this repository (already
 * compiled), libdragon's asset tools for Linux and the repository's converters (the tools folder's Python scripts).
 * Python 3 comes from the system; if it lacks numpy or Pillow they are installed into a private
 * virtual environment on the first run. No Valve game data is included.
 *
 *   ./hl64builder                       find Half-Life through Steam, or ask for the folder
 *   ./hl64builder ~/Games/Half-Life     use that install
 *   ./hl64builder --clean ...           convert everything again instead of reusing the last run
 *
 * The ROM is written next to the program as hl64.z64.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#ifndef HL64_VERSION
#define HL64_VERSION "dev"
#endif

#define PATHLEN (PATH_MAX + 64)

static char g_root[PATHLEN];      // ~/.local/share/HL64Builder/<version> (version = tag + commit: a rebuilt
                                  // release never reuses an older build's engine or converted data)
static char g_work[PATHLEN];      // ~/.local/share/HL64Builder/work
static char g_exedir[PATHLEN];

static void fail(const char *fmt, const char *arg)
{
    printf("\nERROR: ");
    printf(fmt, arg ? arg : "");
    printf("\n");
    exit(1);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static int dir_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p for a file's directory
static void make_dirs_for(const char *path)
{
    char tmp[PATHLEN * 2];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

// ------------------------------------------------------------------ the packed payload
// footer (last 16 bytes of the program): uint64 payload offset, "HL64PAY1"
// payload: "HL64PAK1", uint32 count, then per file: uint16 name length, name (/ separated),
//          uint64 size, uint64 packed size, zlib data
static int read_exact(FILE *f, void *buf, size_t n)
{
    return fread(buf, 1, n, f) == n;
}

static void extract_payload(void)
{
    char marker[PATHLEN * 2];
    snprintf(marker, sizeof marker, "%s/.complete", g_root);
    if (file_exists(marker)) return;

    FILE *h = fopen("/proc/self/exe", "rb");
    if (!h) fail("cannot read this program%s", "");
    uint64_t off;
    char magic[8];
    if (fseek(h, -16, SEEK_END) || !read_exact(h, &off, 8) || !read_exact(h, magic, 8) || memcmp(magic, "HL64PAY1", 8))
        fail("this hl64builder is incomplete (no packed files). Download it again.%s", "");
    if (fseek(h, (long)off, SEEK_SET)) fail("damaged payload%s", "");
    char hdr[8];
    uint32_t count;
    if (!read_exact(h, hdr, 8) || memcmp(hdr, "HL64PAK1", 8) || !read_exact(h, &count, 4)) fail("damaged payload%s", "");

    printf("Unpacking the builder (first run only) to\n  %s\n", g_root);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t nl;
        char name[512];
        uint64_t size, packed;
        if (!read_exact(h, &nl, 2) || nl >= sizeof name || !read_exact(h, name, nl)) fail("damaged payload%s", "");
        name[nl] = 0;
        if (!read_exact(h, &size, 8) || !read_exact(h, &packed, 8)) fail("damaged payload%s", "");
        unsigned char *src = malloc(packed ? packed : 1), *dst = malloc(size ? size : 1);
        if (!src || !dst) fail("out of memory unpacking %s", name);
        if (!read_exact(h, src, packed)) fail("damaged payload at %s", name);
        uLongf dl = (uLongf)size;
        if (size && uncompress(dst, &dl, src, (uLong)packed) != Z_OK) fail("damaged payload at %s", name);
        char out[PATHLEN * 2];
        snprintf(out, sizeof out, "%s/%s", g_root, name);
        make_dirs_for(out);
        int exec = !strncmp(name, "tools/", 6) && !strchr(name + 6, '/') && !strstr(name, ".py");
        FILE *o = fopen(out, "wb");
        if (!o) fail("cannot write %s", out);
        if (size && fwrite(dst, 1, size, o) != size) fail("cannot write %s (disk full?)", out);
        if (fclose(o)) fail("cannot write %s (disk full?)", out);
        if (exec) chmod(out, 0755);
        free(src);
        free(dst);
        if ((i & 63) == 0) { printf("  %u / %u files\r", i, count); fflush(stdout); }
    }
    fclose(h);
    printf("  %u / %u files\n", count, count);
    FILE *m = fopen(marker, "wb");
    if (m) fclose(m);
}

// ------------------------------------------------------------------ finding Half-Life
static int is_valve_dir(const char *d)
{
    char p[PATHLEN * 2];
    snprintf(p, sizeof p, "%s/maps/c1a0.bsp", d);
    return file_exists(p);
}

// accepts the Half-Life folder or its valve folder (a trailing slash, quotes or a "~/" are fine)
static int resolve_valve(const char *in, char *out)
{
    char d[PATHLEN * 2];
    const char *home = getenv("HOME");
    if (in[0] == '~' && in[1] == '/' && home) snprintf(d, sizeof d, "%s%s", home, in + 1);
    else snprintf(d, sizeof d, "%s", in);
    size_t n = strlen(d);
    while (n > 1 && (d[n - 1] == '/' || d[n - 1] == ' ' || d[n - 1] == '"' || d[n - 1] == '\'' || d[n - 1] == '\r' || d[n - 1] == '\n')) d[--n] = 0;
    char *s = d;
    while (*s == '"' || *s == '\'' || *s == ' ') s++;
    if (is_valve_dir(s)) { snprintf(out, PATH_MAX, "%s", s); return 1; }
    char v[PATHLEN * 2];
    snprintf(v, sizeof v, "%s/valve", s);
    if (is_valve_dir(v)) { snprintf(out, PATH_MAX, "%s", v); return 1; }
    return 0;
}

static int try_library(const char *lib, char *out)
{
    char p[PATHLEN * 2];
    snprintf(p, sizeof p, "%s/steamapps/common/Half-Life", lib);
    if (resolve_valve(p, out)) return 1;
    snprintf(p, sizeof p, "%s/SteamApps/common/Half-Life", lib);    // older Steam used this spelling
    return resolve_valve(p, out);
}

// every Steam install we know of, plus every library listed in its steamapps/libraryfolders.vdf
static int find_steam_halflife(char *out)
{
    const char *home = getenv("HOME");
    if (!home) return 0;
    const char *rel[] = {".steam/steam", ".steam/root", ".local/share/Steam",
                         ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                         "snap/steam/common/.local/share/Steam"};
    for (int i = 0; i < 5; i++) {
        char steam[PATHLEN];
        snprintf(steam, sizeof steam, "%s/%s", home, rel[i]);
        if (!dir_exists(steam)) continue;
        if (try_library(steam, out)) return 1;
        char vdf[PATHLEN * 2];
        snprintf(vdf, sizeof vdf, "%s/steamapps/libraryfolders.vdf", steam);
        FILE *f = fopen(vdf, "rb");
        if (!f) continue;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *k = strstr(line, "\"path\"");
            if (!k) continue;
            char *a = strchr(k + 6, '"');
            if (!a) continue;
            char *b = strchr(a + 1, '"');
            if (!b) continue;
            *b = 0;
            char lib[PATHLEN];
            int j = 0;
            for (char *c = a + 1; *c && j < (int)sizeof lib - 1; c++) {   // "/mnt/a\\b" style escapes
                if (c[0] == '\\' && c[1] == '\\') c++;
                lib[j++] = *c;
            }
            lib[j] = 0;
            if (try_library(lib, out)) { fclose(f); return 1; }
        }
        fclose(f);
    }
    return 0;
}

// ------------------------------------------------------------------ running the steps
static int run(char *const argv[])
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "could not start: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}
static int read_text(const char *path, char *buf, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = (int)fread(buf, 1, max - 1, f);
    buf[n < 0 ? 0 : n] = 0;
    fclose(f);
    return 1;
}

// the Python to run the converters with: the system's if it has numpy and Pillow, else a private venv
static void pick_python(char *py, size_t max)
{
    char *chk[] = {"python3", "-c", "import numpy, PIL", NULL};
    char *ver[] = {"python3", "-c", "import sys; sys.exit(sys.version_info < (3, 8))", NULL};
    if (run(ver) != 0) fail("Python 3 (3.8 or newer) is needed: install python3.%s", "");
    int quiet = dup(2);                              // the import check prints a traceback when numpy is missing
    freopen("/dev/null", "w", stderr);
    int ok = run(chk) == 0;
    fflush(stderr);
    dup2(quiet, 2);
    close(quiet);
    if (ok) { snprintf(py, max, "python3"); return; }

    char venv[PATHLEN * 2];
    snprintf(venv, sizeof venv, "%s/venv", g_root);
    snprintf(py, max, "%s/bin/python", venv);
    char *vchk[] = {py, "-c", "import numpy, PIL", NULL};
    if (file_exists(py) && run(vchk) == 0) return;
    printf("Installing numpy and Pillow into a private Python environment (first run only)\n");
    char *mk[] = {"python3", "-m", "venv", venv, NULL};
    if (run(mk) != 0) fail("could not create a Python virtual environment (on Debian/Ubuntu: apt install python3-venv),\n"
                           "or install numpy and Pillow for python3 yourself.%s", "");
    char *pip[] = {py, "-m", "pip", "install", "-q", "numpy", "pillow", NULL};
    if (run(pip) != 0) fail("pip could not install numpy and Pillow (is the network up?).%s", "");
}

static int has_tool(const char *name)
{
    char p[PATHLEN * 2];
    snprintf(p, sizeof p, "%s/tools/%s", g_root, name);
    return file_exists(p);
}

int main(int argc, char **argv)
{
    printf("Half-Life 64 ROM builder (%s)\n", HL64_VERSION);
    printf("Builds hl64.z64 from your own copy of Half-Life. No game data is included in this program.\n\n");

    ssize_t n = readlink("/proc/self/exe", g_exedir, sizeof g_exedir - 1);
    if (n < 0) fail("cannot find this program%s", "");
    g_exedir[n] = 0;
    char *sl = strrchr(g_exedir, '/');
    if (sl) *sl = 0;
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char base[PATHLEN];
    if (xdg && *xdg) snprintf(base, sizeof base, "%s/HL64Builder", xdg);
    else if (home) snprintf(base, sizeof base, "%s/.local/share/HL64Builder", home);
    else snprintf(base, sizeof base, "%s/.hl64builder", g_exedir);
    snprintf(g_root, sizeof g_root, "%s/%s", base, HL64_VERSION);
    snprintf(g_work, sizeof g_work, "%s/work", base);
    char tmpf[PATHLEN * 2];
    snprintf(tmpf, sizeof tmpf, "%s/x", g_root);
    make_dirs_for(tmpf);
    snprintf(tmpf, sizeof tmpf, "%s/x", g_work);
    make_dirs_for(tmpf);

    int clean = 0;
    const char *given = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--clean")) clean = 1;
        else given = argv[i];
    }

    extract_payload();

    // 1. Half-Life
    char valve[PATH_MAX] = "";
    if (given && !resolve_valve(given, valve)) printf("\"%s\" is not a Half-Life folder (no valve/maps/c1a0.bsp).\n", given);
    if (!valve[0] && find_steam_halflife(valve)) printf("Found Half-Life (Steam): %s\n", valve);
    while (!valve[0]) {
        char line[PATHLEN * 2];
        if (!isatty(0)) fail("Half-Life was not found. Run again with the Half-Life folder as the argument.%s", "");
        printf("\nHalf-Life was not found automatically.\n"
               "Type the path of your Half-Life folder (the one that contains \"valve\"), then press Enter:\n> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) return 1;
        if (!resolve_valve(line, valve)) printf("That folder doesn't contain valve/maps/c1a0.bsp.\n");
    }
    printf("Using: %s\n\n", valve);

    // 2. convert the game data (reused when the last run finished for the same install and version)
    char fsdir[PATHLEN], stamp[PATHLEN], want[PATHLEN * 2], have[PATHLEN * 2] = "";
    snprintf(fsdir, sizeof fsdir, "%s/filesystem", g_work);
    snprintf(stamp, sizeof stamp, "%s/converted.txt", g_work);
    snprintf(want, sizeof want, "%s\n%s\n", HL64_VERSION, valve);
    int reuse = !clean && read_text(stamp, have, sizeof have) && !strcmp(have, want) && dir_exists(fsdir);
    if (reuse) {
        printf("[1/3] Game data already converted by a previous run (use --clean to redo it).\n");
    } else {
        remove(stamp);
        long jobs = sysconf(_SC_NPROCESSORS_ONLN);
        if (jobs < 1) jobs = 1;
        if (jobs > 12) jobs = 12;
        if (!has_tool("mkdfs")) fail("the packed tools are missing from %s: delete that folder and run again.", g_root);
        char py[PATHLEN * 2];
        pick_python(py, sizeof py);
        printf("[1/3] Converting maps, textures, models, sprites, sounds and music.\n"
               "      This takes a while (about 30-90 minutes, most of it compressing the maps).\n\n");
        const char *oldpath = getenv("PATH");
        char env[8192];
        snprintf(env, sizeof env, "%s/tools:%s", g_root, oldpath ? oldpath : "/usr/bin:/bin");
        setenv("PATH", env, 1);
        snprintf(env, sizeof env, "%s/tools/navgen", g_root);
        setenv("HL64_NAVGEN", env, 1);
        setenv("PYTHONUTF8", "1", 1);
        setenv("PYTHONUNBUFFERED", "1", 1);
        char script[PATHLEN * 2], jn[16];
        snprintf(script, sizeof script, "%s/hl64/tools/build_assets.py", g_root);
        snprintf(jn, sizeof jn, "%ld", jobs);
        char *cmd[] = {py, script, valve, fsdir, "-j", jn, NULL};
        if (run(cmd) != 0) fail("converting the game data failed (see the messages above).%s", "");
        write_text(stamp, want);
    }

    // 3. the N64 filesystem and the ROM
    printf("\n[2/3] Packing the N64 filesystem\n");
    char dfs[PATHLEN], tmp[PATHLEN], rom[PATHLEN * 2];
    char mkdfs[PATHLEN * 2], n64tool[PATHLEN * 2], ed64[PATHLEN * 2], elf[PATHLEN * 2], sym[PATHLEN * 2], ver[PATHLEN * 2];
    snprintf(dfs, sizeof dfs, "%s/hl64.dfs", g_work);
    snprintf(mkdfs, sizeof mkdfs, "%s/tools/mkdfs", g_root);
    snprintf(n64tool, sizeof n64tool, "%s/tools/n64tool", g_root);
    snprintf(ed64, sizeof ed64, "%s/tools/ed64romconfig", g_root);
    snprintf(elf, sizeof elf, "%s/engine/hl64.elf.stripped", g_root);
    snprintf(sym, sizeof sym, "%s/engine/hl64.elf.sym", g_root);
    snprintf(ver, sizeof ver, "%s/engine/libdragon.version", g_root);
    char *c1[] = {mkdfs, dfs, fsdir, NULL};
    if (run(c1) != 0) fail("mkdfs failed%s", "");

    printf("[3/3] Writing the ROM\n");
    snprintf(tmp, sizeof tmp, "%s/hl64.z64.tmp", g_work);
    remove(tmp);
    char *c2[] = {n64tool, "--toc", "--title", "HALF-LIFE 64", "--category", "N", "--output", tmp,
                  "--align", "256", elf, sym, dfs, ver, NULL};
    if (run(c2) != 0) fail("n64tool failed%s", "");
    char *c3[] = {ed64, "--savetype", "sram256k", "--expansionpak", "unspecified", "--regionfree", tmp, NULL};
    if (run(c3) != 0) fail("ed64romconfig failed%s", "");
    snprintf(rom, sizeof rom, "%s/hl64.z64", g_exedir);
    if (rename(tmp, rom) != 0) {
        // other filesystem, or the program's folder is read-only
        char cp[PATHLEN * 2];
        snprintf(cp, sizeof cp, "%s", rom);
        char *c4[] = {"cp", tmp, cp, NULL};
        if (run(c4) == 0) remove(tmp);
        else {
            snprintf(rom, sizeof rom, "%s/hl64.z64", g_work);
            rename(tmp, rom);
        }
    }
    struct stat st;
    double mb = stat(rom, &st) == 0 ? st.st_size / 1048576.0 : 0;
    printf("\nDone: %s (%.1f MB)\n", rom, mb);
    printf("Play it in ares, gopher64 or simple64 with the Expansion Pak on and the save type set to SRAM,\n"
           "or copy it to a 64 MB-capable flashcart. Keep it to yourself: it contains Half-Life's game data.\n");
    return 0;
}
