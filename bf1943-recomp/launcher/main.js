// Electron main process: finds the game, keeps the settings and starts bf1943.exe.
const { app, BrowserWindow, ipcMain, dialog } = require('electron');
// (Linux: its own first-run setup, build_linux.sh; the game files found as on Windows.)
const setup = require(process.platform === 'linux' ? './setup_linux' : './setup');
const update = require('./update');
const { spawn } = require('child_process');
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
    dataHint: 'original\v106',
  },
  bfbc1: {
    title: 'Battlefield: Bad Company',
    exe: process.platform === 'win32' ? find('bfbc1.exe', 'game-bfbc1/bfbc1.exe', 'bfbc1-recomp/build/game/bfbc1.exe')
      : find('game-bfbc1/bfbc1', 'build/linux-bfbc1/game/bfbc1'),  // (build_linux.sh --bfbc1's)
    data: findDir('original/bfbc1/default.xex', 'bfbc1/extracted/default.xex'),
    textures: findDir('texpack_bfbc1/install/replacements.txt'),
    dataHint: 'bfbc1\extracted',
  },
});
let games = scanGames();
// setup.js finds the build trees next to this folder (<root>/src, <root>/src-bfbc1).
const launcherDir = installRoot ? path.join(installRoot, 'Launcher')
  : app.isPackaged ? path.dirname(process.execPath) : __dirname;
// The HD texture pack, made on this PC from the player's own game files
// (tools/texpack/texpack.py make: Real-ESRGAN, fetched for this system, and
// the PBR maps; a stopped run carries on). Battlefield 1943's only.
// (A release, installed or a folder: <root>/Launcher next to <root>/src.)
const releaseRoot = (installRoot || app.isPackaged) ? path.dirname(launcherDir) : null;
const texWork = releaseRoot ? path.join(releaseRoot, 'texpack') : path.resolve(__dirname, '..', '..', 'texpack_work');
const texScript = releaseRoot ? path.join(releaseRoot, 'src', 'tools', 'texpack', 'texpack.py')
  : path.resolve(__dirname, '..', 'tools', 'texpack', 'texpack.py');
let texJob = null;
ipcMain.handle('textures-make', () => {
  if (texJob) return { error: 'The texture pack is being made already.' };
  const g = games.bf1943;
  if (!g.data) return { error: 'Set up Battlefield 1943 first: the pack is made from its game files.' };
  const python = process.platform === 'win32' ? setup.findTools().python : 'python3';
  if (!python) return { error: 'Python is needed: set up the game first (it installs Python).' };
  if (!fs.existsSync(texScript)) return { error: 'texpack.py not found.' };
  const job = spawn(python, [texScript, 'make', '--game', g.data, '--work', texWork], { windowsHide: true });
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
    send({ finished: true, code, stopped: job.stopped, error: code === 0 ? '' : error || lastError });
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
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true },
  });
  win.setMenu(null);
  win.loadFile('index.html');
  win.once('ready-to-show', () => win.show());
}

ipcMain.handle('state', () => (games = scanGames(), {
  settings: load(),
  games: Object.fromEntries(Object.entries(games).map(([k, g]) =>
    [k, { title: g.title, error: problem(g), outdated: outdated(g.exe), hasTextures: !!g.textures }])),
}));

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
