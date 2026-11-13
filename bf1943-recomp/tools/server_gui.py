"""A small window for the Battlefield 1943 dedicated server (Windows and Linux).

  python tools/server_gui.py        (or server_gui.bat / server_gui.sh)

The same choices as start_server.bat / .sh: the server's name, the maps it
rotates through, bots, being listed on the internet, the bots' advisor (a
small AI model, installed the first time) and training runs. START runs the
server with them, the log shows what it says, STOP ends it, OPEN REPLAY opens
the newest training run's replay in the browser. The choices are kept in
server_data/server_gui.json. Needs Python's tkinter (Linux: python3-tk).
"""
import glob
import json
import os
import pathlib
import queue
import subprocess
import sys
import threading
import webbrowser

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:
    sys.exit('This needs tkinter: on Debian/Ubuntu "sudo apt install python3-tk", '
             'Fedora "sudo dnf install python3-tkinter", Arch "sudo pacman -S tk".')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # bf1943-recomp (or a release's src)
WINDOWS = os.name == 'nt'
MAPS = [('Wake_island_s', 'Wake Island'), ('Iwo_Jima_s', 'Iwo Jima'), ('Guadal_Canal', 'Guadalcanal'),
        ('Coral_sea', 'Coral Sea (air)')]
USER = os.path.join(ROOT, 'server_data')
SETTINGS = os.path.join(USER, 'server_gui.json')
LLM = os.path.join(ROOT, 'llm')


def first(paths):
    return next((p for p in paths if os.path.isfile(p)), '')


def find_exe():
    """The game (or the Linux server) built here, a release or the launcher's."""
    up = os.path.dirname(ROOT)
    if WINDOWS:
        return first([os.path.join(ROOT, 'build', 'game', 'bf1943.exe'), os.path.join(up, 'game', 'bf1943.exe'),
                      os.path.join(ROOT, 'build', 'install', 'game', 'bf1943.exe')])
    return first([os.path.join(ROOT, 'build', 'linux-server', 'game', 'bf1943_server'),
                  os.path.join(ROOT, 'build', 'linux', 'bf1943_server'),
                  os.path.join(ROOT, 'build', 'linux-client', 'game', 'bf1943'), os.path.join(up, 'game', 'bf1943'),
                  os.path.join(up, 'build-linux', 'game', 'bf1943')])


def find_game():
    for d in (os.path.join(ROOT, 'original', 'v106'), os.path.join(os.path.dirname(ROOT), 'original', 'v106'),
              os.path.join(ROOT, 'build', 'install', 'original', 'v106')):
        if os.path.isfile(os.path.join(d, 'default.xex')):
            return d
    return ''


def load():
    s = {'name': 'My Server', 'maps': [m for m, _ in MAPS], 'bots': 8, 'publish': False, 'llm': False,
         'train': False, 'train_minutes': 30, 'exe': '', 'game': ''}
    try:
        with open(SETTINGS, encoding='utf-8') as f:
            s.update(json.load(f))
    except (OSError, ValueError):
        pass
    return s


class App:
    def __init__(self, root):
        self.root, self.proc, self.lines = root, None, queue.Queue()
        self.s = load()
        root.title('Battlefield 1943 dedicated server')
        root.minsize(720, 520)
        f = ttk.Frame(root, padding=10)
        f.pack(fill='both', expand=True)
        f.columnconfigure(1, weight=1)

        r = 0
        ttk.Label(f, text='Server name').grid(row=r, column=0, sticky='w')
        self.name = tk.StringVar(value=self.s['name'])
        ttk.Entry(f, textvariable=self.name).grid(row=r, column=1, columnspan=3, sticky='we', pady=2)
        r += 1
        ttk.Label(f, text='Maps').grid(row=r, column=0, sticky='nw')
        mf = ttk.Frame(f)
        mf.grid(row=r, column=1, columnspan=3, sticky='w')
        self.maps = {}
        for i, (key, label) in enumerate(MAPS):
            v = tk.BooleanVar(value=key in self.s['maps'])
            ttk.Checkbutton(mf, text=label, variable=v).grid(row=0, column=i, padx=(0, 12))
            self.maps[key] = v
        r += 1
        ttk.Label(f, text='Bots').grid(row=r, column=0, sticky='w')
        self.bots = tk.IntVar(value=self.s['bots'])
        ttk.Spinbox(f, from_=0, to=16, width=5, textvariable=self.bots).grid(row=r, column=1, sticky='w', pady=2)
        r += 1
        self.publish = tk.BooleanVar(value=self.s['publish'])
        ttk.Checkbutton(f, text='List it on the internet (your router must pass UDP ports 1003 and 19430 to this PC)',
                        variable=self.publish).grid(row=r, column=0, columnspan=4, sticky='w')
        r += 1
        self.llm = tk.BooleanVar(value=self.s['llm'])
        ttk.Checkbutton(f, text='Bot advisor: a small AI model on this PC tells stuck or losing bots where to go '
                        '(installed the first time, about 3.5 GB)', variable=self.llm).grid(
            row=r, column=0, columnspan=4, sticky='w')
        r += 1
        tf = ttk.Frame(f)
        tf.grid(row=r, column=0, columnspan=4, sticky='w')
        self.train = tk.BooleanVar(value=self.s['train'])
        ttk.Checkbutton(tf, text='Training run (nobody joins, 16 bots, twice as fast; they tune how they play) for',
                        variable=self.train).pack(side='left')
        self.minutes = tk.IntVar(value=self.s['train_minutes'])
        ttk.Spinbox(tf, from_=5, to=1440, increment=5, width=6, textvariable=self.minutes).pack(side='left', padx=4)
        ttk.Label(tf, text='minutes').pack(side='left')
        r += 1
        self.exe = tk.StringVar(value=self.s['exe'] if os.path.isfile(self.s['exe']) else find_exe())
        self.game = tk.StringVar(value=self.s['game'] if os.path.isdir(self.s['game']) else find_game())
        for label, var, pick in (('Game program', self.exe, self.pick_exe), ('Game files', self.game, self.pick_game)):
            ttk.Label(f, text=label).grid(row=r, column=0, sticky='w')
            ttk.Entry(f, textvariable=var).grid(row=r, column=1, columnspan=2, sticky='we', pady=2)
            ttk.Button(f, text='Browse…', command=pick).grid(row=r, column=3, padx=(4, 0))
            r += 1

        bf = ttk.Frame(f)
        bf.grid(row=r, column=0, columnspan=4, sticky='we', pady=(8, 4))
        self.start_b = ttk.Button(bf, text='START', command=self.start)
        self.start_b.pack(side='left')
        self.stop_b = ttk.Button(bf, text='STOP', command=self.stop, state='disabled')
        self.stop_b.pack(side='left', padx=6)
        ttk.Button(bf, text='OPEN REPLAY', command=self.open_replay).pack(side='left')
        ttk.Button(bf, text='SERVER FOLDER', command=lambda: self.open_path(USER)).pack(side='left', padx=6)
        self.status = ttk.Label(bf, text='Stopped')
        self.status.pack(side='right')
        r += 1
        self.log = tk.Text(f, height=16, wrap='none', bg='#111', fg='#ccc', insertbackground='#ccc',
                           font=('Consolas' if WINDOWS else 'monospace', 9))
        self.log.grid(row=r, column=0, columnspan=4, sticky='nsew')
        sb = ttk.Scrollbar(f, command=self.log.yview)
        sb.grid(row=r, column=4, sticky='ns')
        self.log['yscrollcommand'] = sb.set
        f.rowconfigure(r, weight=1)
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.pump)

    # ---- choices -------------------------------------------------------------
    def pick_exe(self):
        p = filedialog.askopenfilename(title='The game program (bf1943.exe, or bf1943_server on Linux)')
        if p:
            self.exe.set(p)

    def pick_game(self):
        p = filedialog.askdirectory(title='The game files (the folder with default.xex)')
        if p:
            self.game.set(p)

    def save(self):
        self.s.update(name=self.name.get().strip() or 'My Server', bots=self.bots.get(),
                      maps=[k for k, v in self.maps.items() if v.get()], publish=self.publish.get(),
                      llm=self.llm.get(), train=self.train.get(), train_minutes=self.minutes.get(),
                      exe=self.exe.get(), game=self.game.get())
        os.makedirs(USER, exist_ok=True)
        with open(SETTINGS, 'w', encoding='utf-8') as f:
            json.dump(self.s, f, indent=2)

    # ---- running ---------------------------------------------------------------
    def say(self, line):
        self.lines.put(line.rstrip('\n'))

    def run(self, cmd):
        """Runs cmd (in ROOT), every line of its output to the log; its exit code."""
        flags = subprocess.CREATE_NO_WINDOW if WINDOWS else 0
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, creationflags=flags, text=True, errors='replace')
        for line in self.proc.stdout:
            self.say(line)
        return self.proc.wait()

    def start(self):
        try:
            self.bots.get(), self.minutes.get()
        except tk.TclError:
            messagebox.showerror('Server', 'Bots and minutes must be numbers.')
            return
        maps = [k for k, v in self.maps.items() if v.get()]
        if not maps:
            messagebox.showerror('Server', 'Choose at least one map.')
            return
        if not os.path.isfile(self.exe.get()):
            messagebox.showerror('Server', 'The game program was not found: build the game first (the launcher), '
                                           'or choose it with Browse.')
            return
        if not os.path.isfile(os.path.join(self.game.get(), 'default.xex')):
            messagebox.showerror('Server', 'The game files (a folder with default.xex) were not found: choose them '
                                           'with Browse.')
            return
        self.save()
        args = [self.exe.get(), '--game', self.game.get(), '--user', 'server_data', '--dedicated',
                '--name', self.s['name'], '--maps', ','.join(maps), '--bots', str(self.s['bots'])]
        if self.s['train']:
            args += ['--train', '--speed', '2', '--train-minutes', str(self.s['train_minutes'])]
        elif self.s['publish']:
            args.append('--publish')
        if self.s['llm']:
            args += ['--bot-llm', 'llm']
        self.start_b['state'], self.stop_b['state'] = 'disabled', 'normal'
        self.stopping = False
        threading.Thread(target=self.serve, args=(args,), daemon=True).start()

    def serve(self, args):
        if self.s['llm'] and not os.path.isfile(os.path.join(LLM, 'bot_llm.ini')):
            self.say('== Installing the bot advisor (llama.cpp and the model, about 3.5 GB)…')
            code = self.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'setup_bot_llm.py'), '--dir', 'llm'])
            if code != 0 or self.stopping:
                self.say('== The advisor could not be installed (%s): the server starts without it.' % code)
                args = [a for a in args if a not in ('--bot-llm', 'llm')]
            if self.stopping:
                self.lines.put(None)
                return
        self.say('== ' + ' '.join('"%s"' % a if ' ' in a else a for a in args))
        code = self.run(args)
        self.say('== The server stopped (exit code %s).' % code)
        self.lines.put(None)  # (the buttons back)

    def stop(self):
        self.stopping = True
        if self.proc and self.proc.poll() is None:
            self.say('== Stopping…')
            self.proc.terminate()

    def pump(self):
        try:
            while True:
                line = self.lines.get_nowait()
                if line is None:
                    self.start_b['state'], self.stop_b['state'] = 'normal', 'disabled'
                    self.status['text'] = 'Stopped'
                    continue
                if 'training: the run' in line or 'hosting' in line or 'round over' in line:
                    self.status['text'] = line.split('] ', 1)[-1][:90]
                elif str(self.stop_b['state']) == 'normal' and self.status['text'] == 'Stopped':
                    self.status['text'] = 'Running'
                self.log.insert('end', line + '\n')
                if int(self.log.index('end-1c').split('.')[0]) > 3000:
                    self.log.delete('1.0', '500.0')
                self.log.see('end')
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    # ---- folders ---------------------------------------------------------------
    def open_path(self, p):
        os.makedirs(p, exist_ok=True)
        if WINDOWS:
            os.startfile(p)  # noqa
        else:
            subprocess.Popen(['xdg-open', p], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def open_replay(self):
        runs = sorted(glob.glob(os.path.join(USER, 'bot_advice', 'runs', '*', 'index.html')), key=os.path.getmtime)
        if not runs:
            messagebox.showinfo('Replay', 'No training run yet: tick "Training run" and START.')
            return
        webbrowser.open(pathlib.Path(runs[-1]).resolve().as_uri())

    def close(self):
        if self.proc and self.proc.poll() is None:
            if not messagebox.askyesno('Server', 'The server is running. Stop it and close?'):
                return
            self.stop()
        self.root.destroy()


if __name__ == '__main__':
    root = tk.Tk()
    App(root)
    root.mainloop()
