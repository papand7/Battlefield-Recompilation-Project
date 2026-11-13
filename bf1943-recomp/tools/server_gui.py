"""A small window for the dedicated servers of Battlefield 1943 and Battlefield:
Bad Company (Windows and Linux).

  python tools/server_gui.py        (or server_gui.bat / server_gui.sh)

The game at the top, then the same choices as start_server.bat / .sh: the
server's name, the maps it rotates through, bots, being listed on the
internet, the bots' advisor (a small AI model, installed the first time) and
training runs. START runs the server with them, the log shows what it says,
STOP ends it, OPEN REPLAY opens the newest training run's replay in the
browser. Each game's choices are kept in its server_data/server_gui.json.
Needs Python's tkinter (Linux: python3-tk).
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
UP = os.path.dirname(ROOT)  # the repo (or a release's root)
WINDOWS = os.name == 'nt'
# Bad Company's tree: next to this one in the repo, src-bfbc1 in a release.
BC_TREE = next((d for d in (os.path.join(UP, 'bfbc1-recomp'), os.path.join(UP, 'src-bfbc1')) if os.path.isdir(d)),
               os.path.join(UP, 'bfbc1-recomp'))


def first(paths):
    return next((p for p in paths if os.path.isfile(p)), '')


GAMES = {
    'bf1943': {
        'title': 'Battlefield 1943',
        'tree': ROOT,
        'maps': [('Wake_island_s', 'Wake Island'), ('Iwo_Jima_s', 'Iwo Jima'), ('Guadal_Canal', 'Guadalcanal'),
                 ('Coral_sea', 'Coral Sea (air)')],
        'exes': ([os.path.join(ROOT, 'build', 'game', 'bf1943.exe'), os.path.join(UP, 'game', 'bf1943.exe'),
                  os.path.join(ROOT, 'build', 'install', 'game', 'bf1943.exe')] if WINDOWS else
                 [os.path.join(ROOT, 'build', 'linux-server', 'game', 'bf1943_server'),
                  os.path.join(ROOT, 'build', 'linux', 'bf1943_server'),
                  os.path.join(ROOT, 'build', 'linux-client', 'game', 'bf1943'), os.path.join(UP, 'game', 'bf1943'),
                  os.path.join(UP, 'build-linux', 'game', 'bf1943')]),
        'data': [os.path.join(ROOT, 'original', 'v106'), os.path.join(UP, 'original', 'v106'),
                 os.path.join(ROOT, 'build', 'install', 'original', 'v106')],
    },
    'bfbc1': {
        'title': 'Battlefield: Bad Company',
        'tree': BC_TREE,
        'maps': [('mp_harbour_day1', 'Valley Run'), ('mp_harbour_day2', 'Deconstruction'),
                 ('mp_ignition_day1', 'Oasis'), ('mp_ignition_day2', 'Final Ignition'),
                 ('mp_monastery_day1', 'End of the Line'), ('mp_monastery_day2', 'Ascension'),
                 ('mp_beachhead_day1', 'Harvest Day'), ('mp_beachhead_day2', 'Over and Out')],
        'exes': ([os.path.join(BC_TREE, 'build', 'game', 'bfbc1.exe'), os.path.join(UP, 'game-bfbc1', 'bfbc1.exe'),
                  os.path.join(ROOT, 'build', 'install', 'game-bfbc1', 'bfbc1.exe')] if WINDOWS else
                 [os.path.join(ROOT, 'build', 'linux-bfbc1-server', 'game', 'bfbc1_server'),
                  os.path.join(ROOT, 'build', 'linux-bfbc1', 'game', 'bfbc1'), os.path.join(UP, 'game-bfbc1', 'bfbc1'),
                  os.path.join(UP, 'game', 'bfbc1')]),
        'data': [os.path.join(UP, 'original', 'bfbc1'), os.path.join(UP, 'bfbc1', 'extracted'),
                 os.path.join(ROOT, 'build', 'install', 'original', 'bfbc1'), os.path.join(BC_TREE, 'original', 'bfbc1')],
    },
}


def user_dir(game):
    return os.path.join(GAMES[game]['tree'], 'server_data')


def load(game):
    g = GAMES[game]
    s = {'name': 'My Server', 'maps': [m for m, _ in g['maps']], 'bots': 8, 'publish': False, 'llm': False,
         'train': False, 'train_minutes': 30, 'exe': '', 'game': ''}
    try:
        with open(os.path.join(user_dir(game), 'server_gui.json'), encoding='utf-8') as f:
            s.update(json.load(f))
    except (OSError, ValueError):
        pass
    # (the other game's program kept by mistake: this game's found again)
    if not os.path.isfile(s['exe']) or not os.path.basename(s['exe']).lower().startswith(game):
        s['exe'] = first(g['exes'])
    if not os.path.isfile(os.path.join(s['game'], 'default.xex')):
        s['game'] = next((d for d in g['data'] if os.path.isfile(os.path.join(d, 'default.xex'))), '')
    return s


class App:
    def __init__(self, root):
        self.root, self.proc, self.lines, self.stopping = root, None, queue.Queue(), False
        self.game_key = 'bf1943'
        try:  # (the game last served)
            with open(os.path.join(user_dir('bf1943'), 'server_gui_game.txt')) as f:
                last = f.read().strip()
            if last in GAMES:
                self.game_key = last
        except OSError:
            pass
        root.title('Battlefield dedicated server')
        root.minsize(760, 560)
        f = ttk.Frame(root, padding=10)
        f.pack(fill='both', expand=True)
        f.columnconfigure(1, weight=1)
        self.frame = f

        r = 0
        ttk.Label(f, text='Game').grid(row=r, column=0, sticky='w')
        gf = ttk.Frame(f)
        gf.grid(row=r, column=1, columnspan=3, sticky='w')
        self.game_var = tk.StringVar(value=self.game_key)
        for key, g in GAMES.items():
            ttk.Radiobutton(gf, text=g['title'], value=key, variable=self.game_var,
                            command=self.switch_game).pack(side='left', padx=(0, 14))
        r += 1
        ttk.Label(f, text='Server name').grid(row=r, column=0, sticky='w')
        self.name = tk.StringVar()
        ttk.Entry(f, textvariable=self.name).grid(row=r, column=1, columnspan=3, sticky='we', pady=2)
        r += 1
        ttk.Label(f, text='Maps').grid(row=r, column=0, sticky='nw')
        self.map_frame = ttk.Frame(f)
        self.map_frame.grid(row=r, column=1, columnspan=3, sticky='w')
        self.maps = {}
        r += 1
        ttk.Label(f, text='Bots').grid(row=r, column=0, sticky='w')
        self.bots = tk.IntVar()
        ttk.Spinbox(f, from_=0, to=16, width=5, textvariable=self.bots).grid(row=r, column=1, sticky='w', pady=2)
        r += 1
        self.publish = tk.BooleanVar()
        ttk.Checkbutton(f, text='List it on the internet (your router must pass the game\'s UDP ports to this PC)',
                        variable=self.publish).grid(row=r, column=0, columnspan=4, sticky='w')
        r += 1
        self.llm = tk.BooleanVar()
        ttk.Checkbutton(f, text='Bot advisor: a small AI model on this PC tells stuck or losing bots where to go '
                        '(installed the first time, about 3.5 GB)', variable=self.llm).grid(
            row=r, column=0, columnspan=4, sticky='w')
        r += 1
        tf = ttk.Frame(f)
        tf.grid(row=r, column=0, columnspan=4, sticky='w')
        self.train = tk.BooleanVar()
        ttk.Checkbutton(tf, text='Training run (nobody joins, 16 bots, twice as fast; they tune how they play) for',
                        variable=self.train).pack(side='left')
        self.minutes = tk.IntVar()
        ttk.Spinbox(tf, from_=5, to=1440, increment=5, width=6, textvariable=self.minutes).pack(side='left', padx=4)
        ttk.Label(tf, text='minutes').pack(side='left')
        r += 1
        self.exe, self.game = tk.StringVar(), tk.StringVar()
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
        ttk.Button(bf, text='SERVER FOLDER', command=lambda: self.open_path(user_dir(self.game_key))).pack(
            side='left', padx=6)
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
        self.show(self.game_key)
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.pump)

    # ---- choices -------------------------------------------------------------
    def show(self, key):
        """The choices of game `key` in the window."""
        self.game_key = key
        self.s = load(key)
        for w in self.map_frame.winfo_children():
            w.destroy()
        self.maps = {}
        for i, (m, label) in enumerate(GAMES[key]['maps']):
            v = tk.BooleanVar(value=m in self.s['maps'])
            ttk.Checkbutton(self.map_frame, text=label, variable=v).grid(row=i // 4, column=i % 4, sticky='w',
                                                                       padx=(0, 12))
            self.maps[m] = v
        self.name.set(self.s['name'])
        self.bots.set(self.s['bots'])
        self.publish.set(self.s['publish'])
        self.llm.set(self.s['llm'])
        self.train.set(self.s['train'])
        self.minutes.set(self.s['train_minutes'])
        self.exe.set(self.s['exe'])
        self.game.set(self.s['game'])

    def switch_game(self):
        key = self.game_var.get()
        if key == self.game_key:
            return
        if self.proc and self.proc.poll() is None:
            messagebox.showinfo('Server', 'Stop the server first.')
            self.game_var.set(self.game_key)
            return
        try:
            self.save()
        except tk.TclError:
            pass
        self.show(key)

    def pick_exe(self):
        p = filedialog.askopenfilename(title='The game program (bf1943.exe / bfbc1.exe, or the Linux server)')
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
        os.makedirs(user_dir(self.game_key), exist_ok=True)
        with open(os.path.join(user_dir(self.game_key), 'server_gui.json'), 'w', encoding='utf-8') as f:
            json.dump(self.s, f, indent=2)
        os.makedirs(user_dir('bf1943'), exist_ok=True)
        with open(os.path.join(user_dir('bf1943'), 'server_gui_game.txt'), 'w') as f:
            f.write(self.game_key)

    # ---- running ---------------------------------------------------------------
    def say(self, line):
        self.lines.put(line.rstrip('\n'))

    def run(self, cmd, cwd):
        """Runs cmd (in cwd), every line of its output to the log; its exit code."""
        flags = subprocess.CREATE_NO_WINDOW if WINDOWS else 0
        self.proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
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
        threading.Thread(target=self.serve, args=(args, GAMES[self.game_key]['tree']), daemon=True).start()

    def serve(self, args, tree):
        if self.s['llm'] and not os.path.isfile(os.path.join(tree, 'llm', 'bot_llm.ini')):
            self.say('== Installing the bot advisor (llama.cpp and the model, about 3.5 GB)…')
            code = self.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'setup_bot_llm.py'), '--dir', 'llm'],
                            tree)
            if code != 0 or self.stopping:
                self.say('== The advisor could not be installed (%s): the server starts without it.' % code)
                args = [a for a in args if a not in ('--bot-llm', 'llm')]
            if self.stopping:
                self.lines.put(None)
                return
        self.say('== ' + ' '.join('"%s"' % a if ' ' in a else a for a in args))
        code = self.run(args, tree)
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
        runs = sorted(glob.glob(os.path.join(user_dir(self.game_key), 'bot_advice', 'runs', '*', 'index.html')),
                      key=os.path.getmtime)
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
