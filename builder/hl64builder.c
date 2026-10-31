/*
 * HL64Builder.exe -- builds the Half-Life 64 ROM on Windows from your own copy of Half-Life.
 *
 * The program carries everything the build needs (packed onto the end of the .exe by
 * builder/pack.py): the N64 engine from this repository (already compiled), libdragon's asset
 * tools for Windows, the repository's converters (tools/*.py) and a private copy of Python with
 * numpy and Pillow. No Valve game data is included: the maps, textures, models and sounds are
 * converted from the Half-Life install found on this computer.
 *
 * It is a small window: the Half-Life folder (found through Steam when it can be), where the ROM
 * goes (next to the .exe unless changed) and, optionally, where the temporary files live (the
 * unpacked tools and the converted game data, %LOCALAPPDATA%\HL64Builder by default). The choices
 * are remembered for next time. The steps' output is shown in the window as they run.
 *
 *   HL64Builder.exe ["D:\Games\Half-Life"] [--clean] [--build]   (a folder given, or dropped on the
 *   .exe, is used; --build starts at once)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include <stdarg.h>

#ifndef HL64_VERSION
#define HL64_VERSION "dev"
#endif

static char g_root[MAX_PATH];     // <temp>\<version> (temp: %LOCALAPPDATA%\HL64Builder or the chosen folder) (version = tag + commit: a rebuilt
                                   // release never reuses an older build's engine or converted data)
static char g_work[MAX_PATH];     // <temp>\work
static char g_exedir[MAX_PATH];

static HWND g_wnd, g_log, g_prog, g_status;
static volatile LONG g_failed;
static void log_text(const char *s);
static void logf_(const char *fmt, ...)
{
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log_text(b);
}
// a failure ends the build thread: the message goes to the log and a box
static void fail(const char *fmt, const char *arg)
{
    char b[1024];
    snprintf(b, sizeof b, fmt, arg ? arg : "");
    logf_("\r\nERROR: %s\r\n", b);
    InterlockedExchange(&g_failed, 1);
    PostMessageA(g_wnd, WM_APP + 2, 0, 0);
    ExitThread(1);
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

    logf_("Unpacking the builder's tools (first run only) to\r\n  %s\r\n", g_root);
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
        if ((i & 63) == 0) SendMessageA(g_prog, PBM_SETPOS, (WPARAM)(i * 10 / (count ? count : 1)), 0);
    }
    CloseHandle(h);
    logf_("  %u files\r\n", count);
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
// a step's output (stdout and stderr) comes back through a pipe into the window's log
static int run(const char *cmdline)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = NULL;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    char *cl = _strdup(cmdline);
    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        logf_("could not start: %s\r\n", cmdline);
        free(cl); CloseHandle(rd); CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    char buf[1024], line[4096];
    int ln = 0;
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\r') continue;               // progress counters rewrite their line: keep them as lines
            line[ln++] = c;
            if (c == '\n' || ln >= (int)sizeof line - 3) {
                if (line[ln - 1] == '\n') { line[ln - 1] = '\r'; line[ln++] = '\n'; }
                line[ln] = 0;
                log_text(line);
                ln = 0;
            }
        }
    }
    if (ln) { line[ln] = 0; log_text(line); log_text("\r\n"); }
    CloseHandle(rd);
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

// ------------------------------------------------------------------ the build (a worker thread)
static char g_valve[MAX_PATH], g_outdir[MAX_PATH], g_tempdir[MAX_PATH], g_rom[MAX_PATH];
static int g_clean;

static void set_status(const char *s) { SetWindowTextA(g_status, s); }
static void set_progress(int pct, int marquee)
{
    LONG st = GetWindowLongA(g_prog, GWL_STYLE);
    if (marquee && !(st & PBS_MARQUEE)) { SetWindowLongA(g_prog, GWL_STYLE, st | PBS_MARQUEE); SendMessageA(g_prog, PBM_SETMARQUEE, TRUE, 40); }
    if (!marquee && (st & PBS_MARQUEE)) { SendMessageA(g_prog, PBM_SETMARQUEE, FALSE, 0); SetWindowLongA(g_prog, GWL_STYLE, st & ~PBS_MARQUEE); }
    if (!marquee) SendMessageA(g_prog, PBM_SETPOS, pct, 0);
}

static DWORD WINAPI build_thread(LPVOID unused)
{
    (void)unused;
    snprintf(g_root, sizeof g_root, "%s\\%s", g_tempdir, HL64_VERSION);
    snprintf(g_work, sizeof g_work, "%s\\work", g_tempdir);
    make_dirs_for(g_root);
    CreateDirectoryA(g_tempdir, NULL);
    CreateDirectoryA(g_root, NULL);
    CreateDirectoryA(g_work, NULL);
    if (!dir_exists(g_work)) fail("cannot create the temporary folder %s", g_work);
    make_dirs_for(g_outdir);
    CreateDirectoryA(g_outdir, NULL);
    if (!dir_exists(g_outdir)) fail("cannot create the output folder %s", g_outdir);

    set_status("Unpacking the builder's tools...");
    set_progress(0, 0);
    extract_payload();
    set_progress(10, 0);

    // convert the game data (reused when the last run finished for the same install and version)
    char fsdir[MAX_PATH], stamp[MAX_PATH], want[MAX_PATH * 2], have[MAX_PATH * 2] = "";
    snprintf(fsdir, sizeof fsdir, "%s\\filesystem", g_work);
    snprintf(stamp, sizeof stamp, "%s\\converted.txt", g_work);
    snprintf(want, sizeof want, "%s\n%s\n", HL64_VERSION, g_valve);
    int reuse = !g_clean && read_text(stamp, have, sizeof have) && !strcmp(have, want) && dir_exists(fsdir);
    if (reuse) {
        logf_("[1/3] Game data already converted by a previous run (tick \"Convert everything again\" to redo it).\r\n");
    } else {
        DeleteFileA(stamp);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int jobs = (int)si.dwNumberOfProcessors;
        if (jobs < 1) jobs = 1;
        if (jobs > 12) jobs = 12;
        set_status("Converting the game data (about 30-90 minutes)...");
        set_progress(0, 1);
        logf_("[1/3] Converting maps, textures, models, sprites, sounds and music.\r\n"
              "      This takes a while (about 30-90 minutes, most of it compressing the maps).\r\n\r\n");
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
                 g_root, g_root, g_valve, fsdir, jobs);
        if (run(cmd) != 0) fail("converting the game data failed (see the messages above).%s", "");
        write_text(stamp, want);
    }

    // the N64 filesystem and the ROM
    set_status("Packing the N64 filesystem...");
    set_progress(85, 0);
    logf_("\r\n[2/3] Packing the N64 filesystem\r\n");
    char cmd[MAX_PATH * 8], dfs[MAX_PATH], tmp[MAX_PATH];
    snprintf(dfs, sizeof dfs, "%s\\hl64.dfs", g_work);
    snprintf(cmd, sizeof cmd, "\"%s\\tools\\mkdfs.exe\" \"%s\" \"%s\"", g_root, fwd(dfs), fwd(fsdir));
    if (run(cmd) != 0) fail("mkdfs failed%s", "");

    set_status("Writing the ROM...");
    set_progress(95, 0);
    logf_("[3/3] Writing the ROM\r\n");
    snprintf(tmp, sizeof tmp, "%s\\hl64.z64.tmp", g_work);
    DeleteFileA(tmp);
    snprintf(cmd, sizeof cmd,
             "\"%s\\tools\\n64tool.exe\" --toc --title \"HALF-LIFE 64\" --category N --output \"%s\" "
             "--align 256 \"%s/engine/hl64.elf.stripped\" \"%s/engine/hl64.elf.sym\" \"%s\" \"%s/engine/libdragon.version\"",
             g_root, fwd(tmp), fwd(g_root), fwd(g_root), fwd(dfs), fwd(g_root));
    if (run(cmd) != 0) fail("n64tool failed%s", "");
    snprintf(cmd, sizeof cmd, "\"%s\\tools\\ed64romconfig.exe\" --savetype sram256k --expansionpak unspecified --regionfree \"%s\"", g_root, fwd(tmp));
    if (run(cmd) != 0) fail("ed64romconfig failed%s", "");
    snprintf(g_rom, sizeof g_rom, "%s\\hl64.z64", g_outdir);
    if (!MoveFileExA(tmp, g_rom, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        snprintf(g_rom, sizeof g_rom, "%s\\hl64.z64", g_work);
        MoveFileExA(tmp, g_rom, MOVEFILE_REPLACE_EXISTING);
        logf_("Couldn't write to the output folder: the ROM was left in the temporary folder.\r\n");
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    double mb = GetFileAttributesExA(g_rom, GetFileExInfoStandard, &fa) ? (fa.nFileSizeLow + (double)fa.nFileSizeHigh * 4294967296.0) / 1048576.0 : 0;
    logf_("\r\nDone: %s (%.1f MB)\r\n", g_rom, mb);
    logf_("Play it in ares, gopher64 or simple64 with the Expansion Pak on and the save type set to SRAM,\r\n"
          "or copy it to a 64 MB-capable flashcart. Keep it to yourself: it contains Half-Life's game data.\r\n");
    set_progress(100, 0);
    PostMessageA(g_wnd, WM_APP + 1, 0, 0);
    return 0;
}

// ------------------------------------------------------------------ the window
enum { ID_VALVE = 101, ID_VALVE_BR, ID_OUT, ID_OUT_BR, ID_TEMP_ON, ID_TEMP, ID_TEMP_BR, ID_CLEAN, ID_BUILD, ID_OPEN, ID_LOG };
static HWND h_valve, h_out, h_temp_on, h_temp, h_temp_br, h_clean, h_build, h_open, h_valve_br, h_out_br;
static HFONT g_font, g_mono;
static char g_default_temp[MAX_PATH], g_settings[MAX_PATH];
static HANDLE g_thread;

// the log: appended from the build thread (SendMessage hands it to the window's thread)
static void log_text(const char *s)
{
    if (!g_log) return;
    int n = GetWindowTextLengthA(g_log);
    if (n > 900000) { SendMessageA(g_log, EM_SETSEL, 0, 300000); SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)""); n = GetWindowTextLengthA(g_log); }
    SendMessageA(g_log, EM_SETSEL, n, n);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)s);
}

static void get_text(HWND h, char *out, int n)
{
    GetWindowTextA(h, out, n);
    size_t l = strlen(out);   // trim spaces, quotes and trailing slashes
    while (l && (out[l - 1] == ' ' || out[l - 1] == '"' || out[l - 1] == '\\' || out[l - 1] == '/')) out[--l] = 0;
    char *p = out;
    while (*p == ' ' || *p == '"') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
}

static int CALLBACK browse_cb(HWND hwnd, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data && ((const char *)data)[0]) SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, data);
    return 0;
}
static void browse(HWND edit, const char *title)
{
    char cur[MAX_PATH];
    get_text(edit, cur, sizeof cur);
    BROWSEINFOA bi = {0};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    bi.lpfn = browse_cb;
    bi.lParam = (LPARAM)cur;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    char path[MAX_PATH];
    if (SHGetPathFromIDListA(pidl, path)) SetWindowTextA(edit, path);
    CoTaskMemFree(pidl);
}

static void save_settings(void)
{
    char v[MAX_PATH], o[MAX_PATH], t[MAX_PATH];
    get_text(h_valve, v, sizeof v); get_text(h_out, o, sizeof o); get_text(h_temp, t, sizeof t);
    WritePrivateProfileStringA("builder", "halflife", v, g_settings);
    WritePrivateProfileStringA("builder", "output", o, g_settings);
    WritePrivateProfileStringA("builder", "temp", t, g_settings);
    WritePrivateProfileStringA("builder", "custom_temp", SendMessageA(h_temp_on, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0", g_settings);
}

static void set_busy(int busy)
{
    EnableWindow(h_build, !busy);
    EnableWindow(h_valve, !busy); EnableWindow(h_valve_br, !busy);
    EnableWindow(h_out, !busy); EnableWindow(h_out_br, !busy);
    EnableWindow(h_temp_on, !busy); EnableWindow(h_clean, !busy);
    int custom = SendMessageA(h_temp_on, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(h_temp, !busy && custom); EnableWindow(h_temp_br, !busy && custom);
    SetWindowTextA(h_build, busy ? "Building..." : "Build ROM");
}

static void start_build(void)
{
    char v[MAX_PATH];
    get_text(h_valve, v, sizeof v);
    if (!resolve_valve(v, g_valve)) {
        MessageBoxA(g_wnd, "That folder isn't a Half-Life install: it needs valve\\maps\\c1a0.bsp.\n\n"
                    "Choose the Half-Life folder (the one that contains \"valve\").", "Half-Life 64 ROM builder", MB_ICONWARNING);
        return;
    }
    get_text(h_out, g_outdir, sizeof g_outdir);
    if (!g_outdir[0]) {
        GetModuleFileNameA(NULL, g_outdir, sizeof g_outdir);
        char *sl = strrchr(g_outdir, '\\'); if (sl) *sl = 0;
    }
    if (SendMessageA(h_temp_on, BM_GETCHECK, 0, 0) == BST_CHECKED) get_text(h_temp, g_tempdir, sizeof g_tempdir);
    else g_tempdir[0] = 0;
    if (!g_tempdir[0]) snprintf(g_tempdir, sizeof g_tempdir, "%s", g_default_temp);
    g_clean = SendMessageA(h_clean, BM_GETCHECK, 0, 0) == BST_CHECKED;
    save_settings();
    SetWindowTextA(g_log, "");
    logf_("Half-Life 64 ROM builder (%s)\r\nHalf-Life: %s\r\nROM to: %s\r\nTemporary files: %s\r\n\r\n", HL64_VERSION, g_valve, g_outdir, g_tempdir);
    g_failed = 0;
    ShowWindow(h_open, SW_HIDE);
    set_busy(1);
    g_thread = CreateThread(NULL, 0, build_thread, NULL, 0, NULL);
}

static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(!strcmp(cls, "EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void layout(int W, int H)
{
    int bw = 90, m = 12, ew = W - 2 * m - bw - 8;
    MoveWindow(h_valve, m, 54, ew, 24, TRUE);   MoveWindow(h_valve_br, W - m - bw, 53, bw, 26, TRUE);
    MoveWindow(h_out, m, 108, ew, 24, TRUE);    MoveWindow(h_out_br, W - m - bw, 107, bw, 26, TRUE);
    MoveWindow(h_temp, m, 162, ew, 24, TRUE);   MoveWindow(h_temp_br, W - m - bw, 161, bw, 26, TRUE);
    MoveWindow(h_clean, m, 194, W - 2 * m - 300, 22, TRUE);
    MoveWindow(h_open, W - m - 280, 192, 140, 28, TRUE);
    MoveWindow(h_build, W - m - 130, 192, 130, 28, TRUE);
    MoveWindow(g_prog, m, 230, W - 2 * m, 18, TRUE);
    MoveWindow(g_status, m, 252, W - 2 * m, 20, TRUE);
    MoveWindow(g_log, m, 276, W - 2 * m, H - 276 - m, TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_wnd = hwnd;
        mk("STATIC", "Builds hl64.z64 from your own copy of Half-Life. No game data is included in this program.", SS_LEFT, 12, 10, 600, 18, 0);
        mk("STATIC", "Half-Life folder (the one that contains \"valve\"):", SS_LEFT, 12, 34, 500, 18, 0);
        h_valve = mk("EDIT", "", ES_AUTOHSCROLL, 0, 0, 10, 10, ID_VALVE);
        h_valve_br = mk("BUTTON", "Browse...", BS_PUSHBUTTON, 0, 0, 10, 10, ID_VALVE_BR);
        mk("STATIC", "Save the ROM in:", SS_LEFT, 12, 88, 500, 18, 0);
        h_out = mk("EDIT", "", ES_AUTOHSCROLL, 0, 0, 10, 10, ID_OUT);
        h_out_br = mk("BUTTON", "Browse...", BS_PUSHBUTTON, 0, 0, 10, 10, ID_OUT_BR);
        h_temp_on = mk("BUTTON", "Keep the temporary files somewhere else (optional; they take about 1.5 GB):", BS_AUTOCHECKBOX, 12, 138, 600, 22, ID_TEMP_ON);
        h_temp = mk("EDIT", "", ES_AUTOHSCROLL, 0, 0, 10, 10, ID_TEMP);
        h_temp_br = mk("BUTTON", "Browse...", BS_PUSHBUTTON, 0, 0, 10, 10, ID_TEMP_BR);
        h_clean = mk("BUTTON", "Convert everything again (don't reuse the last run's game data)", BS_AUTOCHECKBOX, 0, 0, 10, 10, ID_CLEAN);
        h_open = mk("BUTTON", "Show the ROM", BS_PUSHBUTTON, 0, 0, 10, 10, ID_OPEN);
        ShowWindow(h_open, SW_HIDE);
        h_build = mk("BUTTON", "Build ROM", BS_DEFPUSHBUTTON, 0, 0, 10, 10, ID_BUILD);
        g_prog = CreateWindowExA(0, PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 10, 10, hwnd, NULL, GetModuleHandleA(NULL), NULL);
        SendMessageA(g_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        g_status = mk("STATIC", "Ready.", SS_LEFT | SS_ENDELLIPSIS, 0, 0, 10, 10, 0);
        g_log = mk("EDIT", "", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_HSCROLL, 0, 0, 10, 10, ID_LOG);
        SendMessageA(g_log, WM_SETFONT, (WPARAM)g_mono, TRUE);
        SendMessageA(g_log, EM_SETLIMITTEXT, 0, 0);
        return 0;
    }
    case WM_SIZE: layout(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize.x = 640; ((MINMAXINFO *)lp)->ptMinTrackSize.y = 440; return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_log) { SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW)); return (LRESULT)GetSysColorBrush(COLOR_WINDOW); }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_VALVE_BR: browse(h_valve, "Choose your Half-Life folder (the one that contains \"valve\")"); return 0;
        case ID_OUT_BR: browse(h_out, "Choose where to save hl64.z64"); return 0;
        case ID_TEMP_BR: browse(h_temp, "Choose where to keep the temporary files"); return 0;
        case ID_TEMP_ON: set_busy(0); return 0;
        case ID_BUILD: start_build(); return 0;
        case ID_OPEN: {
            char args[MAX_PATH + 16];
            snprintf(args, sizeof args, "/select,\"%s\"", g_rom);
            ShellExecuteA(hwnd, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
            return 0;
        }
        }
        break;
    case WM_APP + 1:   // finished
        set_busy(0);
        set_status("Done: the ROM is ready.");
        ShowWindow(h_open, SW_SHOW);
        FlashWindow(hwnd, TRUE);
        if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
        return 0;
    case WM_APP + 2:   // failed
        set_busy(0);
        set_progress(0, 0);
        set_status("The build failed: see the messages below.");
        FlashWindow(hwnd, TRUE);
        if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
        MessageBoxA(hwnd, "The build failed. The messages in the window say where.", "Half-Life 64 ROM builder", MB_ICONERROR);
        return 0;
    case WM_CLOSE:
        if (g_thread && MessageBoxA(hwnd, "A build is still running. Stop it and close?", "Half-Life 64 ROM builder", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        save_settings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    SetProcessDPIAware();

    GetModuleFileNameA(NULL, g_exedir, sizeof g_exedir);
    char *sl = strrchr(g_exedir, '\\');
    if (sl) *sl = 0;
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata) != S_OK) snprintf(appdata, sizeof appdata, "%s", g_exedir);
    snprintf(g_default_temp, sizeof g_default_temp, "%s\\HL64Builder", appdata);
    CreateDirectoryA(g_default_temp, NULL);
    snprintf(g_settings, sizeof g_settings, "%s\\settings.ini", g_default_temp);

    NONCLIENTMETRICSA ncm = {sizeof ncm};
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectA(&ncm.lfMessageFont);
    g_mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = "HL64Builder";
    RegisterClassA(&wc);
    char title[96];
    snprintf(title, sizeof title, "Half-Life 64 ROM builder (%s)", HL64_VERSION);
    HWND w = CreateWindowExA(0, "HL64Builder", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 600, NULL, NULL, inst, NULL);

    // what was chosen last time; a folder on the command line (or dropped on the .exe) wins
    char v[MAX_PATH] = "", o[MAX_PATH] = "", t[MAX_PATH] = "", on[8] = "0";
    GetPrivateProfileStringA("builder", "halflife", "", v, sizeof v, g_settings);
    GetPrivateProfileStringA("builder", "output", "", o, sizeof o, g_settings);
    GetPrivateProfileStringA("builder", "temp", "", t, sizeof t, g_settings);
    GetPrivateProfileStringA("builder", "custom_temp", "0", on, sizeof on, g_settings);
    int argc = 0, autostart = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; wargv && i < argc; i++) {
        char a[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, wargv[i], -1, a, sizeof a, NULL, NULL);
        if (!strcmp(a, "--clean")) SendMessageA(h_clean, BM_SETCHECK, BST_CHECKED, 0);
        else if (!strcmp(a, "--build")) autostart = 1;   // start right away (scripts, tests)
        else snprintf(v, sizeof v, "%s", a);
    }
    if (wargv) LocalFree(wargv);
    char found[MAX_PATH];
    if (!v[0] || !resolve_valve(v, found)) { if (find_steam_halflife(found)) snprintf(v, sizeof v, "%s", found); }
    SetWindowTextA(h_valve, v);
    SetWindowTextA(h_out, o[0] ? o : g_exedir);
    SetWindowTextA(h_temp, t[0] ? t : g_default_temp);
    SendMessageA(h_temp_on, BM_SETCHECK, on[0] == '1' ? BST_CHECKED : BST_UNCHECKED, 0);
    set_busy(0);
    logf_("Choose your Half-Life folder and where the ROM goes, then press Build ROM.\r\n");
    if (v[0]) logf_("Half-Life found: %s\r\n", v);
    else logf_("Half-Life wasn't found automatically: use Browse... to pick its folder.\r\n");

    ShowWindow(w, show);
    UpdateWindow(w);
    if (autostart) PostMessageA(w, WM_COMMAND, ID_BUILD, 0);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0)) {
        if (!IsDialogMessageA(w, &m)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
    return 0;
}
