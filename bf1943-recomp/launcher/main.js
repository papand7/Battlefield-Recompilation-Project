// Electron main process: finds the game, keeps the settings and starts bf1943.exe.
// Also runs the dedicated server (the SERVER page; --server opens on it).
const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron');
// (Linux: its own first-run setup, build_linux.sh; the game files found as on Windows.)
const setup = require(process.platform === 'linux' ? './setup_linux' : './setup');
const update = require('./update');
const { spawn, execFileSync } = require('child_process');
const { pathToFileURL } = require('url');
const readline = require('readline');
const fs = require('fs');
const path = require('path');

const settingsFile = path.join(app.getPath('appData'), 'BF1943Launcher', 'settings.json');
const defaults = {
  name: process.env.USERNAME || process.env.USER || 'Player',
  resolution: '2',      // --res-scale
  upscaling: 'fsr',     // --sharpen
  frameRate: '120',     // --max-fps (0 = no limit)
  window: '1920x1080',
  display: 'windowed',  // --display (fullscreen: borderless on the monitor)
  upscaleQuality: 'off',  // --upscale-quality (with FSR 3 / DLSS / auto)
  shadowDetail: 'high',   // --shadowmap / --shadow-distance (the game's SHADOW DETAIL)
  textures: true,
  preset: 'enhanced',   // --preset stock|enhanced|vivid|toon|noir|ww2
  blood: false,         // --blood (blood on hits, bodies torn apart by explosions)
  hdr: false,           // --hdr (HDR10 output when Windows HDR is on)
  game: 'bf1943',       // which tab: bf1943 | bfbc1
  autoUpdate: true,     // a newer release on GitHub is installed when the launcher starts
};

// The folder the player put the launcher (and their game files) in: the
// single-exe release (electron-builder portable) runs from a temporary copy.
// (Linux: the AppImage runs mounted elsewhere; $APPIMAGE is the file itself.)
const playerDir = process.env.PORTABLE_EXECUTABLE_DIR || (process.env.APPIMAGE && path.dirname(process.env.APPIMAGE))
  || (app.isPackaged ? path.dirname(process.execPath) : null);
// That exe has the build trees inside (resources/source): the games are built
// and installed in <its folder>/BattlefieldRecomp, where the build trees are
// copied when a new version first starts.
const bundled = app.isPackaged ? path.join(process.resourcesPath, 'source') : null;
const installRoot = bundled && fs.existsSync(path.join(bundled, 'src'))
  ? path.join(playerDir, 'BattlefieldRecomp') : null;
function prepareSources() {
  if (!installRoot) return;
  const stamp = path.join(installRoot, 'src', '.launcher-version');
  if (fs.existsSync(stamp) && fs.readFileSync(stamp, 'utf8') === app.getVersion()) return;
  for (const tree of ['src', 'src-bfbc1'])  // over an older copy: its builds stay
    fs.cpSync(path.join(bundled, tree), path.join(installRoot, tree), { recursive: true, force: true });
  fs.writeFileSync(stamp, app.getVersion());
}
prepareSources();
setup.version = app.getVersion();  // stamped on each game's build (.built-version)
// Started by the previous launcher after an update: it is removed once it quit.
update.removeOld(process.argv, process.env.PORTABLE_EXECUTABLE_FILE);
// A build of an older version in the install folder: rebuilt (UPDATE) before playing.
function outdated(exe) {
  if (!installRoot || !exe || !path.resolve(exe).startsWith(path.resolve(installRoot))) return false;
  try { return fs.readFileSync(path.join(path.dirname(exe), '.built-version'), 'utf8') !== app.getVersion(); }
  catch { return true; }
}

// The launcher may sit next to the game or anywhere above the build folders
// (or the install folder above, for the single exe).
function parents() {
  const out = [];
  for (const start of [installRoot || (app.isPackaged ? path.dirname(process.execPath) : __dirname)]) {
    for (let d = start; ; d = path.dirname(d)) {
      out.push(d);
      if (path.dirname(d) === d) break;
    }
  }
  return out;
}
function find(...names) {
  for (const p of parents())
    for (const n of names) {
      const f = path.join(p, n);
      if (fs.existsSync(f)) return f;
    }
  return null;
}
const findDir = (...markers) => { const f = find(...markers); return f && path.dirname(f); };

// One entry per port: its exe, the dumped game files and its HD texture pack.
// Looked up again on every state request (the first-run setup makes them).
const scanGames = () => ({
  bf1943: {
    title: 'Battlefield 1943',
    exe: process.platform === 'win32' ? find('bf1943.exe', 'build/game/bf1943.exe', 'game/bf1943.exe')
      : find('game/bf1943', 'build/linux-client/game/bf1943'),  // (build_linux.sh's)
    data: findDir('original/v106/default.xex'),
    textures: findDir('texpack/install/replacements.txt', 'texpack_work/install/replacements.txt',
      'texpack/replacements.txt'),
    dataHint: path.join('original', 'v106'),
  },
  bfbc1: {
    title: 'Battlefield: Bad Company',
    exe: process.platform === 'win32' ? find('bfbc1.exe', 'game-bfbc1/bfbc1.exe', 'bfbc1-recomp/build/game/bfbc1.exe')
      : find('game-bfbc1/bfbc1', 'build/linux-bfbc1/game/bfbc1'),  // (build_linux.sh --bfbc1's)
    data: findDir('original/bfbc1/default.xex', 'bfbc1/extracted/default.xex'),
    textures: findDir('texpack-bfbc1/install/replacements.txt', 'texpack_bfbc1/install/replacements.txt'),
    dataHint: path.join('bfbc1', 'extracted'),
  },
});
let games = scanGames();
// setup.js finds the build trees next to this folder (<root>/src, <root>/src-bfbc1).
const launcherDir = installRoot ? path.join(installRoot, 'Launcher')
  : app.isPackaged ? path.dirname(process.execPath) : __dirname;
// The HD texture pack, made on this PC from the player's own game files
// (tools/texpack/texpack.py make: Real-ESRGAN, fetched for this system, and
// the PBR maps; a stopped run carries on), for either game: its own work folder.
// (A release, installed or a folder: <root>/Launcher next to <root>/src.)
const releaseRoot = (installRoot || app.isPackaged) ? path.dirname(launcherDir) : null;
const texWorks = {
  bf1943: releaseRoot ? path.join(releaseRoot, 'texpack') : path.resolve(__dirname, '..', '..', 'texpack_work'),
  bfbc1: releaseRoot ? path.join(releaseRoot, 'texpack-bfbc1') : path.resolve(__dirname, '..', '..', 'texpack_bfbc1'),
};
const texScript = releaseRoot ? path.join(releaseRoot, 'src', 'tools', 'texpack', 'texpack.py')
  : path.resolve(__dirname, '..', 'tools', 'texpack', 'texpack.py');
let texJob = null;
ipcMain.handle('textures-make', (_, game = 'bf1943') => {
  if (texJob) return { error: `The ${games[texJob.game].title} texture pack is being made already.` };
  const g = games[game];
  if (!g || !texWorks[game]) return { error: 'No texture pack for that game.' };
  if (!g.data) return { error: `Set up ${g.title} first: the pack is made from its game files.` };
  const python = process.platform === 'win32' ? setup.findTools().python : 'python3';
  if (!python) return { error: 'Python is needed: set up the game first (it installs Python).' };
  if (!fs.existsSync(texScript)) return { error: 'texpack.py not found.' };
  const job = spawn(python, [texScript, 'make', '--game', g.data, '--work', texWorks[game]], { windowsHide: true });
  job.game = game;
  texJob = job;
  const send = (ev) => { if (win) win.webContents.send('textures-event', ev); };
  let lastError = '', ended = false;
  for (const stream of [job.stdout, job.stderr])
    readline.createInterface({ input: stream }).on('line', (line) => {
      const m = /^progress (\w+) (\d+) (\d+)/.exec(line);
      if (!m && line.trim() && (stream === job.stderr || /^(Could not|Real-ESRGAN download)/.test(line))) lastError = line.trim();
      send(m ? { step: m[1], done: +m[2], total: +m[3] } : { line });
    });
  // 'close': after the last output line (an 'exit' can come before it).
  const end = (code, error) => {
    if (ended) return;
    ended = true;
    if (texJob === job) texJob = null;
    send({ finished: true, game, code, stopped: job.stopped, error: code === 0 ? '' : error || lastError });
  };
  job.on('close', (code) => end(code));
  job.on('error', (e) => end(-1, e.message));
  return { ok: true };
});
ipcMain.handle('textures-cancel', () => {
  if (!texJob) return;
  texJob.stopped = true;
  // Windows: the upscaler it runs too (a kill ends only python).
  if (process.platform === 'win32') spawn('taskkill', ['/PID', String(texJob.pid), '/T', '/F'], { windowsHide: true });
  else texJob.kill();
});

function problem(g) {
  return !g.exe ? `${g.title}: exe not found next to the launcher.`
       : outdated(g.exe) ? `${g.title}: this version needs the game rebuilt (UPDATE, right).`
       : !g.data ? `${g.title}: game files (${g.dataHint}) not found.` : null;
}

function load() {
  try { return { ...defaults, ...JSON.parse(fs.readFileSync(settingsFile, 'utf8')) }; }
  catch { return { ...defaults }; }
}
function save(s) {
  fs.mkdirSync(path.dirname(settingsFile), { recursive: true });
  fs.writeFileSync(settingsFile, JSON.stringify(s, null, 2));
}

let win;
function createWindow() {
  win = new BrowserWindow({
    width: 1100, height: 720, resizable: false, maximizable: false, frame: false,
    backgroundColor: '#0E1013', show: false, title: 'Battlefield 1943 / Bad Company',
    icon: path.join(__dirname, process.platform === 'win32' ? 'icon.ico' : 'icon.png'),  // (the taskbar's; make_icon.py)
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true },
  });
  win.setMenu(null);
  win.loadFile('index.html');
  win.once('ready-to-show', () => win.show());
  // A running server: stopped with the launcher, after asking.
  win.on('close', (e) => {
    if (!server) return;
    const answer = dialog.showMessageBoxSync(win, {
      type: 'question', buttons: ['Stop the server and close', 'Cancel'], defaultId: 0, cancelId: 1,
      title: 'Server', message: `The ${games[server.game].title} server is running.`,
      detail: 'Closing the launcher stops it.',
    });
    if (answer === 1) e.preventDefault();
    else stopServer(true);
  });
}

ipcMain.handle('state', () => (games = scanGames(), {
  settings: load(),
  games: Object.fromEntries(Object.entries(games).map(([k, g]) =>
    [k, { title: g.title, error: problem(g), outdated: outdated(g.exe), hasTextures: !!g.textures }])),
  // (--server: server_gui.bat / .sh; --page settings|server|home)
  page: process.argv.includes('--server') ? 'server'
    : process.argv.includes('--page') ? process.argv[process.argv.indexOf('--page') + 1] || 'home' : 'home',
}));

// ---- The dedicated server (the SERVER page) ---------------------------------
// Run from the game's build tree (bf1943-recomp / bfbc1-recomp, a release's src
// / src-bfbc1): its server_data (--user) and llm (the bots' advisor) are there.
// Before it starts: the advisor installed the first time it is chosen
// (tools/setup_bot_llm.py) and the chosen maps' missing nav meshes made
// (tools/make_navs.py). The choices are kept per game in the settings (server).
const trees = require('./setup');  // (paths(): the build trees, Linux's too)
const isWin = process.platform === 'win32';
const pythonExe = () => (isWin ? setup.findTools().python : 'python3');
const isFile = (f) => { try { return fs.statSync(f).isFile(); } catch { return false; } };
function serverPaths(game, chosen = {}) {
  const p = trees.paths(launcherDir, game);
  const src = (p || trees.paths(launcherDir, 'bf1943') || {}).src || path.resolve(__dirname, '..');
  // The program: the one chosen on the page if it is this game's, else the
  // launcher's (Linux: the headless server build first).
  const own = (f) => isFile(f) && path.basename(f).toLowerCase().startsWith(game);
  const linuxServer = isWin || !p ? [] : game === 'bf1943'
    ? [path.join(src, 'build', 'linux-server', 'game', 'bf1943_server'), path.join(src, 'build', 'linux', 'bf1943_server')]
    : [path.join(src, 'build', 'linux-bfbc1-server', 'game', 'bfbc1_server')];
  const exe = [chosen.exe, ...linuxServer, games[game].exe].find((f) => f && own(f)) || null;
  const data = chosen.game && isFile(path.join(chosen.game, 'default.xex')) ? chosen.game : games[game].data;
  const tree = p ? p.tree : exe && path.dirname(exe);
  return { exe, data, tree, src, advisor: !!tree && isFile(path.join(tree, 'llm', 'bot_llm.ini')) };
}

let server = null;  // the running job: { game, child, stopping }
const serverLog = [];  // its last lines (the page gets them again when it reloads)
let serverState = '', logBatch = [], logTimer = null;
const kLogLines = 2000;
const serverSend = (ev) => { if (win && !win.isDestroyed()) win.webContents.send('server-event', ev); };
function flushLog() {
  clearTimeout(logTimer);
  logTimer = null;
  if (logBatch.length) serverSend({ lines: logBatch });
  logBatch = [];
}
function serverLine(line) {
  serverLog.push(line);
  if (serverLog.length > kLogLines) serverLog.splice(0, serverLog.length - kLogLines);
  logBatch.push(line);
  if (!logTimer) logTimer = setTimeout(flushLog, 100);  // (sent ten times a second at most)
  // The state line: the map hosted, a round's end, the training run's replay.
  if (/training: the run|hosting|round over/.test(line)) {
    serverState = line.split('] ').slice(1).join('] ') || line;
    serverSend({ state: serverState });
  }
}
const say = (text) => serverLine('== ' + text);

// Runs one step of the job; its output lines to the log (progress lines to the
// bar, onLine sees every line). Resolves to the exit code.
function runStep(job, cmd, args, cwd, onLine = () => {}) {
  return new Promise((resolve) => {
    let child;
    try {
      child = spawn(cmd, args, {
        cwd, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'], detached: !isWin,  // (Linux: its own group, stopped as one)
        env: { ...process.env, PYTHONUNBUFFERED: '1' },
      });
    } catch (e) {
      say(`Could not start ${path.basename(cmd)}: ${e.message}`);
      resolve(-1);
      return;
    }
    job.child = child;
    let ended = false;
    const end = (code) => { if (!ended) { ended = true; job.child = null; resolve(code); } };
    for (const stream of [child.stdout, child.stderr])
      readline.createInterface({ input: stream }).on('line', (line) => {
        onLine(line);
        const m = /^progress (\S+) (\d+) (\d+)\s*$/.exec(line);
        if (m) serverSend({ progress: { what: m[1], done: +m[2], total: +m[3] } });
        else serverLine(line);
      });
    child.on('close', (code, signal) => end(code === null ? signal || -1 : code));
    child.on('error', (e) => { say(`Could not start ${path.basename(cmd)}: ${e.message}`); end(-1); });
  });
}

async function serve(job, o) {
  const phase = (name) => { job.phase = name; serverSend({ phase: name }); };
  let args = ['--game', o.data, '--user', 'server_data', '--dedicated', '--name', o.name,
    '--maps', o.maps.join(','), '--bots', String(o.bots)];
  if (o.train) args.push('--train', '--speed', '2', '--train-minutes', String(o.trainMinutes));
  else if (o.publish) args.push('--publish');
  if (o.llm) args.push('--bot-llm', 'llm');
  const python = pythonExe();

  if (o.llm && !isFile(path.join(o.tree, 'llm', 'bot_llm.ini'))) {
    phase('advisor');
    say('Installing the bot advisor (llama.cpp and the model, about 3.5 GB)…');
    const code = !python ? (say('Python is needed for it: set up a game first (that installs Python).'), -1)
      : await runStep(job, python, ['-I', path.join(o.src, 'tools', 'setup_bot_llm.py'), '--dir', 'llm'], o.tree);
    if (job.stopping) return;
    if (code !== 0) {
      say(`The advisor could not be installed (exit code ${code}): the server starts without it.`);
      args = args.filter((a, i) => !(a === '--bot-llm' || args[i - 1] === '--bot-llm'));
    }
  }

  // The nav meshes the bots walk on: only the chosen maps' missing ones are made.
  const navScript = path.join(o.src, 'tools', 'make_navs.py');
  if (!isFile(navScript)) say('Nav meshes: tools/make_navs.py not found, skipped.');
  else if (!python) say('Nav meshes: Python is needed (set up a game first), skipped.');
  else {
    phase('navs');
    say('Nav meshes for the chosen maps…');
    let last = '';
    const code = await runStep(job, python, [navScript, '--title', job.game, '--game', o.data, '--exe', o.exe,
      '--nav-dir', path.join(path.dirname(o.exe), 'nav'), '--maps', o.maps.join(',')], o.src,
    (line) => { if (line.trim() && !line.startsWith('progress ')) last = line.trim(); });
    if (job.stopping) return;
    if (code !== 0 || !/^ok\b/.test(last)) {
      const why = /^error:/.test(last) ? last.slice(6).trim() : `exit code ${code}`;
      say(`Warning: the nav meshes could not all be made (${why}). The server starts anyway; `
        + 'bots may get lost on those maps.');
    }
  }

  phase('server');
  say([o.exe, ...args].map((a) => (/\s/.test(a) ? `"${a}"` : a)).join(' '));
  const code = await runStep(job, o.exe, args, o.tree);
  if (!job.stopping) say(`The server stopped (exit code ${code}).`);
}

// Stops the job's current program and what it started (Windows: the
// llama-server the advisor runs; the whole tree, not only the exe).
function stopServer(now = false) {
  if (!server) return;
  server.stopping = true;
  const child = server.child;
  if (!child || child.exitCode !== null || child.signalCode) return;
  say('Stopping…');
  if (isWin) {
    const args = ['/PID', String(child.pid), '/T', '/F'];
    if (now) { try { execFileSync('taskkill', args, { windowsHide: true, stdio: 'ignore' }); } catch { /* (ended) */ } }
    else spawn('taskkill', args, { windowsHide: true, stdio: 'ignore' });
  } else {
    try { process.kill(-child.pid, 'SIGTERM'); } catch { child.kill('SIGTERM'); }
  }
}

ipcMain.handle('server-info', (_, game, chosen) => {
  if (!games[game]) return { error: 'Unknown game.' };
  games = scanGames();
  const p = serverPaths(game, chosen || {});
  return { exe: p.exe, data: p.data, tree: p.tree, advisor: p.advisor };
});
ipcMain.handle('server-status', () => ({
  running: !!server, game: server && server.game, phase: server && server.phase, state: serverState,
  log: serverLog.slice(),
}));
ipcMain.handle('server-start', (_, game, c) => {
  if (server) return { error: 'The server is running already.' };
  if (!games[game]) return { error: 'Unknown game.' };
  games = scanGames();
  const p = serverPaths(game, c);
  const maps = (c.maps || []).filter((m) => /^[\w-]+$/.test(m));
  if (!maps.length) return { error: 'Choose at least one map.' };
  if (!p.exe) return { error: `${games[game].title}: the game program was not found. Set up the game (BATTLE) or choose it below.` };
  if (!p.data) return { error: `${games[game].title}: the game files (a folder with default.xex) were not found. Choose them below.` };
  const o = {
    ...p, maps, name: String(c.name || '').trim() || 'My Server',
    bots: Math.max(0, Math.min(16, Math.round(+c.bots) || 0)),
    trainMinutes: Math.max(5, Math.min(1440, Math.round(+c.trainMinutes) || 30)),
    publish: !!c.publish, llm: !!c.llm, train: !!c.train,
  };
  const s = load();
  save({ ...s, server: { ...s.server, [game]: { ...c, name: o.name, bots: o.bots, trainMinutes: o.trainMinutes } } });
  fs.mkdirSync(path.join(o.tree, 'server_data'), { recursive: true });
  const job = server = { game, child: null, stopping: false };
  serverLog.length = 0;
  serverState = '';
  serverSend({ started: game });
  serve(job, o).catch((e) => say('Error: ' + e.message)).finally(() => {
    if (job.stopping) say('The server was stopped.');
    if (server === job) server = null;
    flushLog();
    serverSend({ finished: true, game });
  });
  return {};
});
ipcMain.handle('server-stop', () => stopServer());
ipcMain.handle('server-pick', async (_, kind) => {
  const r = await dialog.showOpenDialog(win, kind === 'exe'
    ? { title: 'The game program (bf1943 / bfbc1, or the Linux server build)', properties: ['openFile'] }
    : { title: 'The game files: the folder with default.xex', properties: ['openDirectory'] });
  return r.canceled ? null : r.filePaths[0];
});
ipcMain.handle('server-folder', async (_, game) => {
  const { tree } = serverPaths(game);
  if (!tree) return { error: 'The server\'s folder was not found.' };
  const dir = path.join(tree, 'server_data');
  fs.mkdirSync(dir, { recursive: true });
  const err = await shell.openPath(dir);
  return err ? { error: err } : {};
});
// The newest training run's replay (server_data/bot_advice/runs/<map>_<date>/index.html).
ipcMain.handle('server-replay', async (_, game) => {
  const { tree } = serverPaths(game);
  const runs = tree ? path.join(tree, 'server_data', 'bot_advice', 'runs') : '';
  let newest = null;
  try {
    for (const d of fs.readdirSync(runs)) {
      const f = path.join(runs, d, 'index.html');
      try {
        const t = fs.statSync(f).mtimeMs;
        if (!newest || t > newest.t) newest = { f, t };
      } catch { /* (a run without its replay yet) */ }
    }
  } catch { /* (no runs yet) */ }
  if (!newest) return { error: 'No training run yet: turn on Training run and START.' };
  await shell.openExternal(pathToFileURL(newest.f).href);
  return {};
});

ipcMain.handle('play', async (_, s) => {
  s = { ...defaults, ...s, name: (s.name || '').trim() || 'Player' };
  const g = games[s.game] || games.bf1943;
  if (problem(g)) return { error: problem(g) };
  const { exe, data: gameData, textures: texturePack } = g;
  if (!texturePack) s.textures = false;
  save(s);

  const exeDir = path.dirname(exe);
  const run = path.join(path.dirname(exeDir), 'run');
  const workDir = fs.existsSync(run) ? run : exeDir;
  fs.mkdirSync(path.join(workDir, 'userdata'), { recursive: true });
  // Bad Company: its main menu's PC SETTINGS row (made once; the game runs
  // without it, with the stock menu, if it cannot be made).
  if (s.game === 'bfbc1') {
    const err = await setup.patchMenu(launcherDir, gameData, path.join(workDir, 'userdata'));
    if (err) console.warn(err);
  }

  const args = ['--game', gameData, '--user', 'userdata', '--gamertag', s.name,
    '--res-scale', s.resolution, '--sharpen', s.upscaling,
    '--preset', s.preset, '--max-fps', s.frameRate, '--window', s.window,
    '--display', s.display || 'windowed', '--upscale-quality', s.upscaleQuality || 'off'];
  if (s.textures) args.push('--textures', texturePack);
  if (s.blood) args.push('--blood');
  if (s.hdr) args.push('--hdr');
  // The sun's shadows: the map's size (1600 at most: the EDRAM) and reach.
  // (high: the game's own 1600 at 300 m; not in the stock look.)
  const shadows = { low: ['960', '150'], medium: ['1280', '220'], ultra: ['1600', '450'] }[s.shadowDetail];
  if (shadows && s.preset !== 'stock') args.push('--shadowmap', shadows[0], '--shadow-distance', shadows[1]);

  return new Promise((resolve) => {
    const game = spawn(exe, args, { cwd: workDir, windowsHide: true, stdio: 'ignore' });
    game.once('error', (e) => resolve({ error: 'Could not start the game: ' + e.message }));
    game.once('spawn', () => { win.minimize(); resolve({}); });
    game.once('exit', () => { win.restore(); win.focus(); win.webContents.send('game-exited'); });
  });
});

// First-run setup (setup.js), per game: status, choosing the game files, running it.
// The game's disc image, package or unpacked folder is looked for next to the launcher.
ipcMain.handle('setup-status', (_, game) => setup.status(launcherDir, game, playerDir));
ipcMain.handle('setup-pick', async (_, game) => {
  const r = await dialog.showOpenDialog(win, game === 'bfbc1' ? {
    title: 'Your Battlefield: Bad Company disc: the disc image (.iso), or default.xex in an unpacked folder',
    properties: ['openFile'],
    filters: [{ name: 'Disc image or default.xex', extensions: ['iso', 'xex'] }, { name: 'All files', extensions: ['*'] }],
  } : {
    title: 'Your Battlefield 1943 game: the Xbox 360 package file, or default.xex in an unpacked folder',
    properties: ['openFile'],
    filters: [{ name: 'Game package or default.xex', extensions: ['*'] }],
  });
  return r.canceled ? null : r.filePaths[0];
});
let setupRunning = false;
ipcMain.handle('setup-run', async (_, game, gamePath) => {
  if (setupRunning) return { error: 'Setup is already running.' };
  setupRunning = true;
  try {
    await setup.setup(launcherDir, game, gamePath, (ev) => win.webContents.send('setup-event', ev));
    games = scanGames();
    return {};
  } catch (e) {
    return { error: e.message };
  } finally {
    setupRunning = false;
  }
});

ipcMain.handle('save-settings', (_, s) => (save({ ...load(), ...s }), {}));

// Self-update (update.js): only the single-exe release can replace itself.
let pendingUpdate = null;
ipcMain.handle('update-check', async () => {
  const exe = process.env.PORTABLE_EXECUTABLE_FILE;
  if (!app.isPackaged || !exe) return { available: false };
  try {
    pendingUpdate = await update.check(app.getVersion());
    return pendingUpdate ? { available: true, version: pendingUpdate.version, notes: pendingUpdate.notes }
                         : { available: false };
  } catch (e) {
    return { available: false, error: e.message };
  }
});
ipcMain.handle('update-run', async () => {
  const exe = process.env.PORTABLE_EXECUTABLE_FILE;
  if (!pendingUpdate || !exe) return { error: 'No update to install.' };
  if (setupRunning) return { error: 'Setup is running: update after it.' };
  try {
    const next = await update.download(pendingUpdate, path.dirname(exe),
      (p) => win.webContents.send('update-event', { progress: p }));
    spawn(next, ['--updated-from', exe], { detached: true, stdio: 'ignore', cwd: path.dirname(exe) }).unref();
    setTimeout(() => app.quit(), 300);
    return {};
  } catch (e) {
    return { error: e.message };
  }
});

ipcMain.on('minimize', () => win.minimize());
ipcMain.on('close', () => win.close());

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());
