#!/usr/bin/env python3
"""The installer window (the fallback of scripts/bootstrap.ps1, run by hand): asks for your own game files, then runs scripts/install.py and shows
its steps, a progress bar and the log. Only the Python standard library (tkinter). Nothing is sent anywhere."""
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_location  # noqa: E402  (the install location; this old window has no chooser: it uses the remembered one)
STEPS = ['Python packages', 'Game files (dump)', 'Tools', 'Build the game', 'Ready to play']
# setup.py's own steps ("[n/11] name ...") make up step 4; the share of each in the time of a first build (measured order)
SETUP_W = {'deps': 1, 'stage': 1, 'convert': 1, 'modules': 2, 'tables': 1, 'lift': 15, 'sdk': 8, 'compile': 55, 'nro': 12, 'link': 3, 'check': 1}
# --passive: Microsoft's installer shows its progress (--quiet showed nothing and could wait on a hidden prompt)
WINGET = ['winget', 'install', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '-e', '--accept-package-agreements',
          '--accept-source-agreements', '--override',
          '--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended']


class App:
    def __init__(self, root):
        self.root, self.proc, self.q = root, None, queue.Queue()
        root.title('Tomodachi Life: Living the Dream - native PC installer')
        k = root.winfo_fpixels('1i') / 96.0          # the display's scale (1.0 at 100 %, 1.5 at 150 %)
        w, h = min(900 * k, root.winfo_screenwidth() * 0.9), min(700 * k, root.winfo_screenheight() * 0.85)
        root.geometry('%dx%d' % (w, h))
        root.minsize(int(min(760 * k, w)), int(min(560 * k, h)))
        pad = {'padx': 8, 'pady': 3}
        tk.Label(root, text='Build the game on this PC from your own dump', font=('Segoe UI', 13, 'bold')).pack(anchor='w', padx=10, pady=(10, 0))
        tk.Label(root, text='Nothing of the game is in this download: everything is made here, from the files you choose below.',
                 fg='#555').pack(anchor='w', padx=10)

        src = ttk.LabelFrame(root, text='1. Your game files'); src.pack(fill='x', padx=10, pady=8)
        self.mode = tk.StringVar(value='packages')
        ttk.Radiobutton(src, text='Package files (.nsp / .xci / .nsz) and my keys', variable=self.mode, value='packages',
                        command=self.sync).grid(row=0, column=0, columnspan=3, sticky='w', **pad)
        self.base, self.update, self.keys, self.fw, self.dump = (tk.StringVar() for _ in range(5))
        kdef = os.path.join(os.path.expanduser('~'), '.switch', 'prod.keys')
        if os.path.exists(kdef):
            self.keys.set(kdef)
        self.rows = []
        for r, (label, var, kind, tip) in enumerate((
                ('Base game', self.base, 'pkg', '.nsp, .xci or .nsz of the game itself (v0)'),
                ('Update 1.0.4 (optional)', self.update, 'pkg', '.nsp or .nsz of the update'),
                ('prod.keys (packages only)', self.keys, 'keys', 'your console\'s keys, needed only to read package files'),
                ('Firmware (optional)', self.fw, 'dir', 'optional: your firmware\'s own fonts; without it open-source fonts are used')), start=1):
            self.rows.append(self.field(src, r, label, var, kind, tip))
        ttk.Radiobutton(src, text='A dump folder I already made (exefs\\ and romfs\\ inside)', variable=self.mode, value='dump',
                        command=self.sync).grid(row=5, column=0, columnspan=3, sticky='w', **pad)
        self.dumprow = self.field(src, 6, 'Dump folder', self.dump, 'dir', '')
        src.columnconfigure(1, weight=1)

        opt = ttk.Frame(root); opt.pack(fill='x', padx=10)
        tk.Label(opt, text='Compiler jobs:').pack(side='left')
        self.jobs = tk.IntVar(value=max(1, min(24, (os.cpu_count() or 4) - 2)))
        ttk.Spinbox(opt, from_=1, to=64, textvariable=self.jobs, width=4).pack(side='left', padx=4)
        tk.Label(opt, text='(fewer = the PC stays usable while it builds)', fg='#555').pack(side='left')

        prog = ttk.LabelFrame(root, text='2. Progress'); prog.pack(fill='both', expand=True, padx=10, pady=8)
        self.step_lbl = []
        sf = ttk.Frame(prog); sf.pack(fill='x', padx=6, pady=4)
        for i, s in enumerate(STEPS):
            l = tk.Label(sf, text='○ ' + s, fg='#777'); l.grid(row=0, column=i, padx=6, sticky='w')
            self.step_lbl.append(l)
        self.bar = ttk.Progressbar(prog, maximum=100); self.bar.pack(fill='x', padx=6, pady=4)
        self.status = tk.Label(prog, text='Choose your files, then press Install.', anchor='w'); self.status.pack(fill='x', padx=6)
        self.log = tk.Text(prog, height=6, font=('Consolas', 9), state='disabled', wrap='none')
        self.log.pack(fill='both', expand=True, padx=6, pady=4)

        bf = ttk.Frame(root); bf.pack(fill='x', padx=10, pady=(0, 10))
        self.btn = ttk.Button(bf, text='Install', command=self.start); self.btn.pack(side='right')
        self.play = ttk.Button(bf, text='Play', command=self.run_game, state='disabled'); self.play.pack(side='right', padx=6)
        ttk.Button(bf, text='Close', command=self.close).pack(side='left')
        self.sync()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.pump)
        if os.path.exists(os.path.join(build_location.get(), 'work', 'tomodachi_native.exe')):
            self.play.config(state='normal')

    def field(self, parent, r, label, var, kind, tip):
        lab = tk.Label(parent, text=label); lab.grid(row=r, column=0, sticky='w', padx=(24, 4), pady=2)
        ent = ttk.Entry(parent, textvariable=var); ent.grid(row=r, column=1, sticky='we', pady=2)
        def browse():
            if kind == 'dir':
                p = filedialog.askdirectory(title=label)
            else:
                ft = [('Switch packages', '*.nsp *.xci *.nsz')] if kind == 'pkg' else [('keys', '*.keys')]
                p = filedialog.askopenfilename(title=label + (' - ' + tip if tip else ''), filetypes=ft + [('All files', '*.*')])
            if p:
                var.set(os.path.normpath(p))
        b = ttk.Button(parent, text='Browse...', command=browse); b.grid(row=r, column=2, padx=6, pady=2)
        return (lab, ent, b)

    def sync(self):
        pk = self.mode.get() == 'packages'
        for w in [x for row in self.rows for x in row]:
            w.configure(state='normal' if pk else 'disabled')
        for w in self.dumprow:
            w.configure(state='disabled' if pk else 'normal')

    def say(self, line):
        self.log.configure(state='normal'); self.log.insert('end', line + '\n'); self.log.see('end'); self.log.configure(state='disabled')

    def mark(self, i, state):
        sym = {'run': '▶ ', 'ok': '✔ ', 'bad': '✖ '}[state]
        col = {'run': '#0a58ca', 'ok': '#198754', 'bad': '#dc3545'}[state]
        for k in range(i):
            if self.step_lbl[k].cget('fg') != '#198754':
                self.step_lbl[k].config(text='✔ ' + STEPS[k], fg='#198754')
        self.step_lbl[i].config(text=sym + STEPS[i], fg=col)

    def start(self):
        args = [sys.executable, '-u', os.path.join(REPO, 'scripts', 'install.py'), '--no-gui', '--jobs', str(self.jobs.get())]
        if self.mode.get() == 'dump':
            if not self.dump.get():
                return messagebox.showwarning('Installer', 'Choose your dump folder.')
            args += ['--dump', self.dump.get()]
        else:
            if not self.base.get():
                return messagebox.showwarning('Installer', 'Choose the base game (the update 1.0.4 is optional).')
            have = os.path.exists(os.path.join(os.path.expanduser('~'), '.switch', 'prod.keys'))
            if not self.keys.get() and not have:
                return messagebox.showwarning('Installer', 'Package files are encrypted: choose your prod.keys.\n'
                                              '(A dump folder you made on the console needs no keys: use the second option.)')
            args += ['--base', self.base.get()] + (['--update', self.update.get()] if self.update.get() else [])
            if self.keys.get():
                args += ['--keys', self.keys.get()]
            if self.fw.get():
                args += ['--firmware', self.fw.get()]
        self.btn.config(state='disabled')
        self.status.config(text='Working... this can take from 30 minutes to a few hours. You can close the window: the next Install goes on from where it stopped.')
        self.mark(0, 'run')
        env = dict(os.environ, PYTHONIOENCODING='utf-8')
        self.proc = subprocess.Popen(args, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                     errors='replace', env=env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        for line in self.proc.stdout:
            self.q.put(line.rstrip('\n'))
        self.q.put(('END', self.proc.wait()))

    def pump(self):
        try:
            while True:
                item = self.q.get_nowait()
                if isinstance(item, tuple):
                    self.finish(item[1])
                else:
                    self.line(item)
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def line(self, s):
        self.say(s)
        m = re.match(r'([1-5])\. ', s)
        if m:
            i = int(m.group(1)) - 1
            self.mark(i, 'ok' if i == 4 else 'run')
            self.bar['value'] = max(self.bar['value'], [2, 8, 12, 15, 100][i])
            self.status.config(text=s)
        m = re.match(r'\[(\d+)/11\] (\w+)', s)
        if m:
            done = sum(w for k, w in SETUP_W.items() if list(SETUP_W).index(k) < int(m.group(1)) - 1)
            self.bar['value'] = 15 + 85 * done / sum(SETUP_W.values())
            self.mark(3, 'run')
            self.status.config(text='Build: ' + m.group(2) + ' (step %s of 11)' % m.group(1))
        if 'C++ Build Tools, which are missing' in s or 'MSVC Build Tools are missing' in s:
            if messagebox.askyesno('Installer', 'The Microsoft C++ Build Tools are needed and missing (read the reason in the log: '
                                   'often the prebuilt runtime only needs the latest copy of the project).\n\nInstall them now? '
                                   '(Yes accepts the Microsoft licence terms for the Build Tools and the winget source; Windows asks for '
                                   'administrator rights; about 3 GB, 5-15 minutes, Microsoft\'s installer shows its progress; when '
                                   'the console window says it is finished, press Install again.)'):
                try:
                    if not shutil.which('winget'):
                        raise OSError('no winget')
                    subprocess.Popen(['cmd', '/k'] + WINGET, creationflags=subprocess.CREATE_NEW_CONSOLE)   # /k: the console stays open with winget's result
                except OSError:                 # no winget (an old Windows 10): Microsoft's own installer page
                    messagebox.showinfo('Installer', 'winget is not on this PC. Install "Build Tools for Visual Studio" '
                                        '(workload "Desktop development with C++") from https://aka.ms/vs/17/release/vs_BuildTools.exe '
                                        'and press Install again.')

    def finish(self, code):
        self.btn.config(state='normal')
        if code == 0:
            self.bar['value'] = 100
            self.mark(4, 'ok')
            self.status.config(text='Done. Press Play (or start "Tomodachi Life.exe").')
            self.play.config(state='normal')
        else:
            cur = next((i for i, l in enumerate(self.step_lbl) if l.cget('fg') == '#0a58ca'), 0)
            self.mark(cur, 'bad')
            self.status.config(text='Stopped (code %d): see the log above. Press Install again to go on from there.' % code)

    def run_game(self):
        subprocess.Popen([sys.executable, os.path.join(REPO, 'scripts', 'run.py')], cwd=REPO)      # the launcher of the install location

    def close(self):
        if self.proc and self.proc.poll() is None:
            if not messagebox.askyesno('Installer', 'The installation is running. Stop it? (The next Install goes on from where it stopped.)'):
                return
            self.proc.terminate()
        self.root.destroy()


def dpi_aware():
    try:                                        # sharp text on scaled displays (125 %, 150 % ...)
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass


def main():
    dpi_aware()
    root = tk.Tk()
    try:
        ttk.Style().theme_use('vista')
    except tk.TclError:
        pass
    App(root)
    root.mainloop()


if __name__ == '__main__':
    main()
