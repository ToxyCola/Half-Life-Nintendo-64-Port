#!/usr/bin/env python3
"""Half-Life 64 ROM builder for Linux and macOS: the same window as HL64Builder.exe.

It lives inside the builder package (builder/build_unix.sh), next to everything the build needs:

    <root>/engine/   the N64 engine (hl64.elf.stripped, hl64.elf.sym, libdragon.version)
    <root>/tools/    libdragon's tools (mksprite, mkasset, audioconv64, mkdfs, n64tool, ed64romconfig) and navgen
    <root>/python/   a private Python with numpy, Pillow and Tk (this script runs on it)
    <root>/hl64/     the converters (hl64/tools/*.py, this file) and the title screen art
    <root>/VERSION   the release the package was made from

Choose the Half-Life folder (found through Steam when it can be), where the ROM goes and, if you
like, where the temporary files are kept; the choices are remembered. No Valve game data is
included: the maps, textures, models and sounds are converted from your own Half-Life.

    HL64Builder [HALF_LIFE_FOLDER] [--clean] [--build]
"""
import os
import queue
import re
import subprocess
import sys
import threading
import configparser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
IS_MAC = sys.platform == 'darwin'


def read_version():
    try:
        return open(os.path.join(ROOT, 'VERSION')).read().strip() or 'dev'
    except OSError:
        return 'dev'


VERSION = read_version()


def default_temp():
    if IS_MAC:
        return os.path.expanduser('~/Library/Caches/HL64Builder')
    return os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'HL64Builder')


def settings_path():
    if IS_MAC:
        d = os.path.expanduser('~/Library/Application Support/HL64Builder')
    else:
        d = os.path.join(os.environ.get('XDG_CONFIG_HOME') or os.path.expanduser('~/.config'), 'HL64Builder')
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, 'settings.ini')


def default_output():
    # next to the package when that is writable (an extracted folder), else the Desktop / home
    pkg = ROOT
    if IS_MAC and '.app' in pkg:
        pkg = os.path.dirname(pkg[:pkg.index('.app') + 4])
    if os.access(pkg, os.W_OK) and '/Applications' not in pkg and 'AppTranslocation' not in pkg:
        return pkg
    desk = os.path.expanduser('~/Desktop')
    return desk if os.path.isdir(desk) else os.path.expanduser('~')


# ---------------------------------------------------------------- finding Half-Life
def is_valve_dir(d):
    return os.path.isfile(os.path.join(d, 'maps', 'c1a0.bsp'))


def resolve_valve(p):
    """the Half-Life folder or its valve folder -> the valve folder (or None)"""
    if not p:
        return None
    p = os.path.expanduser(p.strip().strip('"').strip("'")).rstrip('/\\')
    if is_valve_dir(p):
        return p
    v = os.path.join(p, 'valve')
    if is_valve_dir(v):
        return v
    return None


def find_steam_halflife():
    if IS_MAC:
        steams = ['~/Library/Application Support/Steam']
    else:
        steams = ['~/.steam/steam', '~/.local/share/Steam', '~/.steam/root',
                  '~/.var/app/com.valvesoftware.Steam/.local/share/Steam', '~/snap/steam/common/.local/share/Steam']
    libs = []
    for s in steams:
        s = os.path.expanduser(s)
        if not os.path.isdir(s):
            continue
        libs.append(s)
        vdf = os.path.join(s, 'steamapps', 'libraryfolders.vdf')
        try:
            for m in re.finditer(r'"path"\s+"([^"]+)"', open(vdf, encoding='utf-8', errors='replace').read()):
                libs.append(m.group(1).replace('\\\\', '\\'))
        except OSError:
            pass
    for lib in libs:
        v = resolve_valve(os.path.join(lib, 'steamapps', 'common', 'Half-Life'))
        if v:
            return os.path.dirname(v)
    return None


# ---------------------------------------------------------------- the build (a worker thread)
class Build(threading.Thread):
    def __init__(self, valve, outdir, tempdir, clean, events):
        super().__init__(daemon=True)
        self.valve, self.outdir, self.tempdir, self.clean, self.ev = valve, outdir, tempdir, clean, events
        self.rom = None
        self.proc = None
        self.stop = False

    def log(self, s):
        self.ev.put(('log', s))

    def status(self, s, pct=None, busy=False):
        self.ev.put(('status', s, pct, busy))

    def run_step(self, args, env=None):
        self.log('$ ' + ' '.join(os.path.basename(a) if i == 0 else a for i, a in enumerate(args)) + '\n')
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, bufsize=0)
        buf = b''
        while True:
            ch = self.proc.stdout.read(1024)
            if not ch:
                break
            buf += ch.replace(b'\r\n', b'\n').replace(b'\r', b'\n')
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                self.log(line.decode('utf-8', 'replace') + '\n')
        if buf:
            self.log(buf.decode('utf-8', 'replace') + '\n')
        code = self.proc.wait()
        self.proc = None
        if self.stop:
            raise RuntimeError('stopped')
        return code

    def fail(self, msg):
        raise RuntimeError(msg)

    def run(self):
        try:
            self.build()
            self.ev.put(('done', self.rom))
        except Exception as e:  # noqa
            self.log('\nERROR: %s\n' % e)
            self.ev.put(('failed', str(e)))

    def build(self):
        work = os.path.join(self.tempdir, 'work')
        os.makedirs(work, exist_ok=True)
        os.makedirs(self.outdir, exist_ok=True)
        tools = os.path.join(ROOT, 'tools')
        exe = lambda n: os.path.join(tools, n + ('.exe' if os.name == 'nt' else ''))  # noqa
        env = dict(os.environ)
        env['PATH'] = tools + os.pathsep + env.get('PATH', '')
        env['HL64_NAVGEN'] = exe('navgen')
        env['PYTHONUTF8'] = '1'
        env['PYTHONUNBUFFERED'] = '1'
        env.pop('PYTHONHOME', None)
        env.pop('PYTHONPATH', None)

        fsdir = os.path.join(work, 'filesystem')
        stamp = os.path.join(work, 'converted.txt')
        want = '%s\n%s\n' % (VERSION, self.valve)
        have = ''
        try:
            have = open(stamp).read()
        except OSError:
            pass
        if not self.clean and have == want and os.path.isdir(fsdir):
            self.log('[1/3] Game data already converted by a previous run (tick "Convert everything again" to redo it).\n')
        else:
            if os.path.exists(stamp):
                os.remove(stamp)
            jobs = max(1, min(12, os.cpu_count() or 1))
            self.status('Converting the game data (about 30-90 minutes)...', None, True)
            self.log('[1/3] Converting maps, textures, models, sprites, sounds and music.\n'
                     '      This takes a while (about 30-90 minutes, most of it compressing the maps).\n\n')
            code = self.run_step([sys.executable, os.path.join(ROOT, 'hl64', 'tools', 'build_assets.py'),
                                  self.valve, fsdir, '-j', str(jobs)], env)
            if code != 0:
                self.fail('converting the game data failed (see the messages above).')
            open(stamp, 'w').write(want)

        self.status('Packing the N64 filesystem...', 85)
        self.log('\n[2/3] Packing the N64 filesystem\n')
        dfs = os.path.join(work, 'hl64.dfs')
        if self.run_step([exe('mkdfs'), dfs, fsdir], env) != 0:
            self.fail('mkdfs failed')

        self.status('Writing the ROM...', 95)
        self.log('[3/3] Writing the ROM\n')
        tmp = os.path.join(work, 'hl64.z64.tmp')
        if os.path.exists(tmp):
            os.remove(tmp)
        eng = os.path.join(ROOT, 'engine')
        if self.run_step([exe('n64tool'), '--toc', '--title', 'HALF-LIFE 64', '--category', 'N', '--output', tmp,
                          '--align', '256', os.path.join(eng, 'hl64.elf.stripped'), os.path.join(eng, 'hl64.elf.sym'),
                          dfs, os.path.join(eng, 'libdragon.version')], env) != 0:
            self.fail('n64tool failed')
        if self.run_step([exe('ed64romconfig'), '--savetype', 'sram256k', '--expansionpak', 'unspecified',
                          '--regionfree', tmp], env) != 0:
            self.fail('ed64romconfig failed')
        rom = os.path.join(self.outdir, 'hl64.z64')
        try:
            os.replace(tmp, rom)
        except OSError:
            import shutil
            try:
                shutil.move(tmp, rom)
            except OSError:
                rom = os.path.join(work, 'hl64.z64')
                os.replace(tmp, rom)
                self.log("Couldn't write to the output folder: the ROM was left in the temporary folder.\n")
        self.rom = rom
        mb = os.path.getsize(rom) / 1048576.0
        self.log('\nDone: %s (%.1f MB)\n' % (rom, mb))
        self.log('Play it in ares, gopher64 or simple64 with the Expansion Pak on and the save type set to SRAM,\n'
                 "or copy it to a 64 MB-capable flashcart. Keep it to yourself: it contains Half-Life's game data.\n")
        self.status('Done: the ROM is ready.', 100)


# ---------------------------------------------------------------- the window
def main():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    args = sys.argv[1:]
    autostart = '--build' in args
    clean_arg = '--clean' in args
    given = next((a for a in args if not a.startswith('--')), None)

    cfg = configparser.ConfigParser()
    ini = settings_path()
    cfg.read(ini)
    if 'builder' not in cfg:
        cfg['builder'] = {}
    c = cfg['builder']

    root = tk.Tk()
    root.title('Half-Life 64 ROM builder (%s)' % VERSION)
    root.minsize(640, 460)
    root.geometry('780x600')
    try:
        style = ttk.Style()
        style.theme_use('aqua' if IS_MAC else 'clam')
        if not IS_MAC:
            style.configure('Horizontal.TProgressbar', background='#3c8de0', troughcolor='#e4e2de')
    except tk.TclError:
        pass

    v_valve = tk.StringVar()
    v_out = tk.StringVar(value=c.get('output') or default_output())
    v_temp_on = tk.BooleanVar(value=c.get('custom_temp', '0') == '1')
    v_temp = tk.StringVar(value=c.get('temp') or default_temp())
    v_clean = tk.BooleanVar(value=clean_arg)
    v_status = tk.StringVar(value='Ready.')

    hl = given or c.get('halflife', '')
    if not resolve_valve(hl):
        hl = find_steam_halflife() or hl
    v_valve.set(hl)

    frm = ttk.Frame(root, padding=12)
    frm.pack(fill='both', expand=True)
    frm.columnconfigure(0, weight=1)

    ttk.Label(frm, text='Builds hl64.z64 from your own copy of Half-Life. No game data is included in this program.').grid(
        row=0, column=0, columnspan=2, sticky='w', pady=(0, 8))

    def row(r, label, var, title, widget_list):
        ttk.Label(frm, text=label).grid(row=r, column=0, columnspan=2, sticky='w')
        e = ttk.Entry(frm, textvariable=var)
        e.grid(row=r + 1, column=0, sticky='ew', pady=(2, 8))

        def pick():
            start = var.get() if os.path.isdir(var.get()) else os.path.expanduser('~')
            d = filedialog.askdirectory(parent=root, title=title, initialdir=start, mustexist=False)
            if d:
                var.set(d)
        b = ttk.Button(frm, text='Browse...', command=pick)
        b.grid(row=r + 1, column=1, sticky='e', padx=(8, 0), pady=(2, 8))
        widget_list += [e, b]
        return e, b

    controls = []
    row(1, 'Half-Life folder (the one that contains "valve"):', v_valve, 'Choose your Half-Life folder', controls)
    row(3, 'Save the ROM in:', v_out, 'Choose where to save hl64.z64', controls)
    cb_temp = ttk.Checkbutton(frm, text='Keep the temporary files somewhere else (optional; they take about 1.5 GB):', variable=v_temp_on)
    cb_temp.grid(row=5, column=0, columnspan=2, sticky='w')
    temp_entry = ttk.Entry(frm, textvariable=v_temp)
    temp_entry.grid(row=6, column=0, sticky='ew', pady=(2, 8))

    def pick_temp():
        d = filedialog.askdirectory(parent=root, title='Choose where to keep the temporary files',
                                    initialdir=v_temp.get() if os.path.isdir(v_temp.get()) else os.path.expanduser('~'))
        if d:
            v_temp.set(d)
    temp_btn = ttk.Button(frm, text='Browse...', command=pick_temp)
    temp_btn.grid(row=6, column=1, sticky='e', padx=(8, 0), pady=(2, 8))

    bar = ttk.Frame(frm)
    bar.grid(row=7, column=0, columnspan=2, sticky='ew')
    bar.columnconfigure(0, weight=1)
    cb_clean = ttk.Checkbutton(bar, text="Convert everything again (don't reuse the last run's game data)", variable=v_clean)
    cb_clean.grid(row=0, column=0, sticky='w')
    btn_show = ttk.Button(bar, text='Show the ROM')
    btn_build = ttk.Button(bar, text='Build ROM')
    btn_build.grid(row=0, column=2, sticky='e')

    prog = ttk.Progressbar(frm, mode='determinate', maximum=100)
    prog.grid(row=8, column=0, columnspan=2, sticky='ew', pady=(10, 2))
    ttk.Label(frm, textvariable=v_status).grid(row=9, column=0, columnspan=2, sticky='w')

    logf = ttk.Frame(frm)
    logf.grid(row=10, column=0, columnspan=2, sticky='nsew', pady=(6, 0))
    frm.rowconfigure(10, weight=1)
    logf.columnconfigure(0, weight=1)
    logf.rowconfigure(0, weight=1)
    log = tk.Text(logf, wrap='none', height=12, font=('Menlo' if IS_MAC else 'DejaVu Sans Mono', 10), state='disabled',
                  bg='white', fg='black', relief='sunken', borderwidth=1)
    log.grid(row=0, column=0, sticky='nsew')
    sy = ttk.Scrollbar(logf, orient='vertical', command=log.yview)
    sy.grid(row=0, column=1, sticky='ns')
    sx = ttk.Scrollbar(logf, orient='horizontal', command=log.xview)
    sx.grid(row=1, column=0, sticky='ew')
    log.configure(yscrollcommand=sy.set, xscrollcommand=sx.set)

    def write_log(s):
        log.configure(state='normal')
        log.insert('end', s)
        if int(log.index('end-1c').split('.')[0]) > 20000:
            log.delete('1.0', '5000.0')
        log.see('end')
        log.configure(state='disabled')

    def save_settings():
        c['halflife'] = v_valve.get()
        c['output'] = v_out.get()
        c['temp'] = v_temp.get()
        c['custom_temp'] = '1' if v_temp_on.get() else '0'
        try:
            with open(ini, 'w') as f:
                cfg.write(f)
        except OSError:
            pass

    state = {'build': None, 'rom': None}

    def set_busy(busy):
        for w in controls + [cb_temp, cb_clean, btn_build]:
            w.state(['disabled'] if busy else ['!disabled'])
        custom = v_temp_on.get() and not busy
        temp_entry.state(['!disabled'] if custom else ['disabled'])
        temp_btn.state(['!disabled'] if custom else ['disabled'])
        btn_build.configure(text='Building...' if busy else 'Build ROM')

    v_temp_on.trace_add('write', lambda *a: set_busy(False))

    events = queue.Queue()

    def start():
        valve = resolve_valve(v_valve.get())
        if not valve:
            messagebox.showwarning('Half-Life 64 ROM builder',
                                   "That folder isn't a Half-Life install: it needs valve/maps/c1a0.bsp.\n\n"
                                   'Choose the Half-Life folder (the one that contains "valve").', parent=root)
            return
        outdir = os.path.expanduser(v_out.get().strip()) or default_output()
        tempdir = os.path.expanduser(v_temp.get().strip()) if v_temp_on.get() else default_temp()
        tempdir = tempdir or default_temp()
        save_settings()
        log.configure(state='normal')
        log.delete('1.0', 'end')
        log.configure(state='disabled')
        write_log('Half-Life 64 ROM builder (%s)\nHalf-Life: %s\nROM to: %s\nTemporary files: %s\n\n' % (VERSION, valve, outdir, tempdir))
        btn_show.grid_remove()
        set_busy(True)
        prog.configure(mode='determinate', value=10)
        b = Build(valve, outdir, tempdir, v_clean.get(), events)
        state['build'] = b
        b.start()

    btn_build.configure(command=start)

    def show_rom():
        rom = state['rom']
        if not rom:
            return
        if IS_MAC:
            subprocess.Popen(['open', '-R', rom])
        else:
            subprocess.Popen(['xdg-open', os.path.dirname(rom)])
    btn_show.configure(command=show_rom)

    def poll():
        try:
            while True:
                e = events.get_nowait()
                if e[0] == 'log':
                    write_log(e[1])
                elif e[0] == 'status':
                    v_status.set(e[1])
                    if e[3]:
                        prog.configure(mode='indeterminate')
                        prog.start(15)
                    else:
                        prog.stop()
                        prog.configure(mode='determinate')
                        if e[2] is not None:
                            prog.configure(value=e[2])
                elif e[0] == 'done':
                    state['build'] = None
                    state['rom'] = e[1]
                    prog.stop()
                    prog.configure(mode='determinate', value=100)
                    set_busy(False)
                    btn_show.grid(row=0, column=1, sticky='e', padx=(0, 8))
                    root.bell()
                elif e[0] == 'failed':
                    state['build'] = None
                    prog.stop()
                    prog.configure(mode='determinate', value=0)
                    set_busy(False)
                    v_status.set('The build failed: see the messages below.')
                    messagebox.showerror('Half-Life 64 ROM builder', 'The build failed. The messages in the window say where.', parent=root)
        except queue.Empty:
            pass
        root.after(100, poll)

    def on_close():
        b = state['build']
        if b and b.is_alive():
            if not messagebox.askyesno('Half-Life 64 ROM builder', 'A build is still running. Stop it and close?', parent=root):
                return
            b.stop = True
            if b.proc:
                try:
                    b.proc.terminate()
                except OSError:
                    pass
        save_settings()
        root.destroy()

    root.protocol('WM_DELETE_WINDOW', on_close)
    set_busy(False)
    write_log('Choose your Half-Life folder and where the ROM goes, then press Build ROM.\n')
    if resolve_valve(v_valve.get()):
        write_log('Half-Life found: %s\n' % v_valve.get())
    else:
        write_log("Half-Life wasn't found automatically: use Browse... to pick its folder.\n")
    root.after(100, poll)
    if autostart:
        root.after(300, start)
    root.mainloop()


if __name__ == '__main__':
    main()
