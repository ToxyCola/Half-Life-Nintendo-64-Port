/*
 * HL64Builder.exe -- builds the Half-Life 64 ROM on Windows from your own copy of Half-Life.
 *
 * The program carries everything the build needs (packed onto the end of the .exe by
 * builder/pack.py): the N64 engine from this repository (already compiled), libdragon's asset
 * tools for Windows, the repository's converters (tools/*.py) and a private copy of Python with
 * numpy and Pillow. No Valve game data is included: the maps, textures, models and sounds are
 * converted from the Half-Life install found on this computer.
 *
 *   HL64Builder.exe                     find Half-Life through Steam, or ask for the folder
 *   HL64Builder.exe "D:\Games\Half-Life"   use that install (drag the folder onto the .exe)
 *   HL64Builder.exe --clean ...         convert everything again instead of reusing the last run
 *
 * The ROM is written next to the .exe as hl64.z64.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#ifndef HL64_VERSION
#define HL64_VERSION "dev"
#endif

static char g_root[MAX_PATH];     // %LOCALAPPDATA%\HL64Builder\<version>
static char g_work[MAX_PATH];     // %LOCALAPPDATA%\HL64Builder\work
static char g_exedir[MAX_PATH];

static void pause_exit(int code)
{
    printf("\nPress Enter to close this window.");
    fflush(stdout);
    getchar();
    exit(code);
}

static void fail(const char *fmt, const char *arg)
{
    printf("\nERROR: ");
    printf(fmt, arg ? arg : "");
    printf("\n");
    pause_exit(1);
}

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int dir_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// mkdir -p for a file's directory
static void make_dirs_for(const char *path)
{
    char tmp[MAX_PATH * 2];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = c;
        }
    }
}

// ------------------------------------------------------------------ the packed payload
// footer (last 16 bytes of the .exe): uint64 payload offset, "HL64PAY1"
// payload: "HL64PAK1", uint32 count, then per file: uint16 name length, name (/ separated),
//          uint64 size, uint64 packed size, zlib data
static int read_exact(HANDLE h, void *buf, DWORD n)
{
    DWORD got = 0;
    while (n) {
        if (!ReadFile(h, buf, n, &got, NULL) || got == 0) return 0;
        buf = (char *)buf + got;
        n -= got;
    }
    return 1;
}

static void extract_payload(void)
{
    char marker[MAX_PATH];
    snprintf(marker, sizeof marker, "%s\\.complete", g_root);
    if (file_exists(marker)) return;

    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    HANDLE h = CreateFileA(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) fail("cannot read %s", self);
    LARGE_INTEGER sz, pos;
    GetFileSizeEx(h, &sz);
    pos.QuadPart = sz.QuadPart - 16;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    uint64_t off;
    char magic[8];
    if (!read_exact(h, &off, 8) || !read_exact(h, magic, 8) || memcmp(magic, "HL64PAY1", 8))
        fail("this HL64Builder.exe is incomplete (no packed files). Download it again.%s", "");
    pos.QuadPart = (LONGLONG)off;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
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
        if (!read_exact(h, src, (DWORD)packed)) fail("damaged payload at %s", name);
        uLongf dl = (uLongf)size;
        if (size && uncompress(dst, &dl, src, (uLong)packed) != Z_OK) fail("damaged payload at %s", name);
        char out[MAX_PATH * 2];
        snprintf(out, sizeof out, "%s\\%s", g_root, name);
        for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
        make_dirs_for(out);
        HANDLE o = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (o == INVALID_HANDLE_VALUE) fail("cannot write %s", out);
        DWORD w;
        if (size && (!WriteFile(o, dst, (DWORD)size, &w, NULL) || w != size)) fail("cannot write %s (disk full?)", out);
        CloseHandle(o);
        free(src);
        free(dst);
        if ((i & 63) == 0) { printf("  %u / %u files\r", i, count); fflush(stdout); }
    }
    CloseHandle(h);
    printf("  %u / %u files\n", count, count);
    FILE *m = fopen(marker, "wb");
    if (m) fclose(m);
}

// ------------------------------------------------------------------ finding Half-Life
static int is_valve_dir(const char *d)
{
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\maps\\c1a0.bsp", d);
    return file_exists(p);
}

// accepts the Half-Life folder or its valve folder
static int resolve_valve(const char *in, char *out)
{
    char d[MAX_PATH * 2];
    snprintf(d, sizeof d, "%s", in);
    size_t n = strlen(d);
    while (n && (d[n - 1] == '\\' || d[n - 1] == '/' || d[n - 1] == ' ' || d[n - 1] == '"' || d[n - 1] == '\r' || d[n - 1] == '\n')) d[--n] = 0;
    char *s = d;
    while (*s == '"' || *s == ' ') s++;
    if (is_valve_dir(s)) { snprintf(out, MAX_PATH, "%s", s); return 1; }
    char v[MAX_PATH * 2];
    snprintf(v, sizeof v, "%s\\valve", s);
    if (is_valve_dir(v)) { snprintf(out, MAX_PATH, "%s", v); return 1; }
    return 0;
}

static int try_library(const char *lib, char *out)
{
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\steamapps\\common\\Half-Life", lib);
    return resolve_valve(p, out);
}

// Steam's install (registry), then every library listed in steamapps\libraryfolders.vdf
static int find_steam_halflife(char *out)
{
    char steam[MAX_PATH] = "";
    DWORD sz = sizeof steam;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &sz) != ERROR_SUCCESS) {
        sz = sizeof steam;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath", RRF_RT_REG_SZ, NULL, steam, &sz) != ERROR_SUCCESS)
            snprintf(steam, sizeof steam, "C:\\Program Files (x86)\\Steam");
    }
    for (char *p = steam; *p; p++) if (*p == '/') *p = '\\';
    if (try_library(steam, out)) return 1;
    char vdf[MAX_PATH * 2];
    snprintf(vdf, sizeof vdf, "%s\\steamapps\\libraryfolders.vdf", steam);
    FILE *f = fopen(vdf, "rb");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *k = strstr(line, "\"path\"");
            if (!k) continue;
            char *a = strchr(k + 6, '"');
            if (!a) continue;
            char *b = strchr(a + 1, '"');
            if (!b) continue;
            *b = 0;
            char lib[MAX_PATH];
            int j = 0;
            for (char *c = a + 1; *c && j < MAX_PATH - 1; c++) {   // "D:\\SteamLibrary" -> D:\SteamLibrary
                if (c[0] == '\\' && c[1] == '\\') c++;
                lib[j++] = *c;
            }
            lib[j] = 0;
            if (try_library(lib, out)) { fclose(f); return 1; }
        }
        fclose(f);
    }
    const char *guesses[] = {"C:\\Program Files (x86)\\Steam", "C:\\Program Files\\Steam", "D:\\Steam", "D:\\SteamLibrary", "E:\\SteamLibrary"};
    for (int i = 0; i < 5; i++) if (try_library(guesses[i], out)) return 1;
    return 0;
}

// libdragon's tools split paths on '/' only (and Windows accepts '/'): arguments get forward slashes
static const char *fwd(const char *p)
{
    static char buf[8][MAX_PATH * 2];
    static int k;
    char *b = buf[k++ & 7];
    snprintf(b, MAX_PATH * 2, "%s", p);
    for (char *c = b; *c; c++) if (*c == '\\') *c = '/';
    return b;
}

// ------------------------------------------------------------------ running the steps
static int run(const char *cmdline)
{
    // the child writes to this window (or wherever our output is redirected)
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    char *cl = _strdup(cmdline);
    fflush(stdout);
    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("could not start: %s\n", cmdline);
        free(cl);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    free(cl);
    return (int)code;
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

int main(int argc, char **argv)
{
    SetConsoleTitleA("Half-Life 64 ROM builder");
    printf("Half-Life 64 ROM builder (%s)\n", HL64_VERSION);
    printf("Builds hl64.z64 from your own copy of Half-Life. No game data is included in this program.\n\n");

    GetModuleFileNameA(NULL, g_exedir, sizeof g_exedir);
    char *sl = strrchr(g_exedir, '\\');
    if (sl) *sl = 0;
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata) != S_OK) snprintf(appdata, sizeof appdata, "%s", g_exedir);
    snprintf(g_root, sizeof g_root, "%s\\HL64Builder\\%s", appdata, HL64_VERSION);
    snprintf(g_work, sizeof g_work, "%s\\HL64Builder\\work", appdata);
    make_dirs_for(g_root);
    CreateDirectoryA(g_root, NULL);
    CreateDirectoryA(g_work, NULL);

    int clean = 0;
    const char *given = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--clean")) clean = 1;
        else given = argv[i];
    }

    extract_payload();

    // 1. Half-Life
    char valve[MAX_PATH] = "";
    if (given && !resolve_valve(given, valve)) printf("\"%s\" is not a Half-Life folder (no valve\\maps\\c1a0.bsp).\n", given);
    if (!valve[0] && find_steam_halflife(valve)) printf("Found Half-Life (Steam): %s\n", valve);
    while (!valve[0]) {
        char line[MAX_PATH * 2];
        printf("\nHalf-Life was not found automatically.\n"
               "Drag your Half-Life folder (the one that contains \"valve\") into this window, then press Enter:\n> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) pause_exit(1);
        if (!resolve_valve(line, valve)) printf("That folder doesn't contain valve\\maps\\c1a0.bsp.\n");
    }
    printf("Using: %s\n\n", valve);

    // 2. convert the game data (reused when the last run finished for the same install and version)
    char fsdir[MAX_PATH], stamp[MAX_PATH], want[MAX_PATH * 2], have[MAX_PATH * 2] = "";
    snprintf(fsdir, sizeof fsdir, "%s\\filesystem", g_work);
    snprintf(stamp, sizeof stamp, "%s\\converted.txt", g_work);
    snprintf(want, sizeof want, "%s\n%s\n", HL64_VERSION, valve);
    int reuse = !clean && read_text(stamp, have, sizeof have) && !strcmp(have, want) && dir_exists(fsdir);
    if (reuse) {
        printf("[1/3] Game data already converted by a previous run (use --clean to redo it).\n");
    } else {
        DeleteFileA(stamp);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int jobs = (int)si.dwNumberOfProcessors;
        if (jobs < 1) jobs = 1;
        if (jobs > 12) jobs = 12;
        printf("[1/3] Converting maps, textures, models, sprites, sounds and music.\n"
               "      This takes a while (about 30-90 minutes, most of it compressing the maps).\n\n");
        char path[8192], env[8192];
        GetEnvironmentVariableA("PATH", path, sizeof path);
        snprintf(env, sizeof env, "%s\\tools;%s\\python;%s", g_root, g_root, path);
        SetEnvironmentVariableA("PATH", env);
        snprintf(env, sizeof env, "%s\\tools\\navgen.exe", g_root);
        SetEnvironmentVariableA("HL64_NAVGEN", env);
        SetEnvironmentVariableA("PYTHONUTF8", "1");
        SetEnvironmentVariableA("PYTHONUNBUFFERED", "1");
        char cmd[MAX_PATH * 6];
        snprintf(cmd, sizeof cmd, "\"%s\\python\\python.exe\" \"%s\\hl64\\tools\\build_assets.py\" \"%s\" \"%s\" -j %d",
                 g_root, g_root, valve, fsdir, jobs);
        if (run(cmd) != 0) fail("converting the game data failed (see the messages above).%s", "");
        write_text(stamp, want);
    }

    // 3. the N64 filesystem and the ROM
    printf("\n[2/3] Packing the N64 filesystem\n");
    char cmd[MAX_PATH * 8], dfs[MAX_PATH], tmp[MAX_PATH], rom[MAX_PATH];
    snprintf(dfs, sizeof dfs, "%s\\hl64.dfs", g_work);
    snprintf(cmd, sizeof cmd, "\"%s\\tools\\mkdfs.exe\" \"%s\" \"%s\"", g_root, fwd(dfs), fwd(fsdir));
    if (run(cmd) != 0) fail("mkdfs failed%s", "");

    printf("[3/3] Writing the ROM\n");
    snprintf(tmp, sizeof tmp, "%s\\hl64.z64.tmp", g_work);
    DeleteFileA(tmp);
    snprintf(cmd, sizeof cmd,
             "\"%s\\tools\\n64tool.exe\" --toc --title \"HALF-LIFE 64\" --category N --output \"%s\" "
             "--align 256 \"%s/engine/hl64.elf.stripped\" \"%s/engine/hl64.elf.sym\" \"%s\" \"%s/engine/libdragon.version\"",
             g_root, fwd(tmp), fwd(g_root), fwd(g_root), fwd(dfs), fwd(g_root));
    if (run(cmd) != 0) fail("n64tool failed%s", "");
    snprintf(cmd, sizeof cmd, "\"%s\\tools\\ed64romconfig.exe\" --savetype sram256k --expansionpak unspecified --regionfree \"%s\"", g_root, fwd(tmp));
    if (run(cmd) != 0) fail("ed64romconfig failed%s", "");
    snprintf(rom, sizeof rom, "%s\\hl64.z64", g_exedir);
    if (!MoveFileExA(tmp, rom, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        snprintf(rom, sizeof rom, "%s\\hl64.z64", g_work);
        MoveFileExA(tmp, rom, MOVEFILE_REPLACE_EXISTING);
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    double mb = GetFileAttributesExA(rom, GetFileExInfoStandard, &fa) ? (fa.nFileSizeLow + (double)fa.nFileSizeHigh * 4294967296.0) / 1048576.0 : 0;
    printf("\nDone: %s (%.1f MB)\n", rom, mb);
    printf("Play it in ares, gopher64 or simple64 with the Expansion Pak on and the save type set to SRAM,\n"
           "or copy it to a 64 MB-capable flashcart. Keep it to yourself: it contains Half-Life's game data.\n");
    pause_exit(0);
    return 0;
}
