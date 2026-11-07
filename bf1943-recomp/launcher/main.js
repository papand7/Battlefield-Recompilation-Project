// Electron main process: finds the game, keeps the settings and starts bf1943.exe.
const { app, BrowserWindow, ipcMain } = require('electron');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const settingsFile = path.join(app.getPath('appData'), 'BF1943Launcher', 'settings.json');
const defaults = {
  name: process.env.USERNAME || 'Player',
  resolution: '2',      // --res-scale
  upscaling: 'fsr',     // --sharpen
  frameRate: '120',     // --max-fps (0 = no limit)
  window: '1920x1080',
  textures: true,
  preset: 'enhanced',   // --preset stock|enhanced|toon|noir|ww2
  blood: false,         // --blood (blood on hits, bodies torn apart by explosions)
  hdr: false,           // --hdr (HDR10 output when Windows HDR is on)
  game: 'bf1943',       // which tab: bf1943 | bfbc1
};

// The launcher may sit next to the game or anywhere above the build folders.
function parents() {
  const out = [];
  for (const start of [app.isPackaged ? path.dirname(process.execPath) : __dirname]) {
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
const games = {
  bf1943: {
    title: 'Battlefield 1943',
    exe: find('bf1943.exe', 'build/game/bf1943.exe', 'game/bf1943.exe'),
    data: findDir('original/v106/default.xex'),
    textures: findDir('texpack_work/install/replacements.txt', 'texpack/replacements.txt'),
    dataHint: 'original\v106',
  },
  bfbc1: {
    title: 'Battlefield: Bad Company',
    exe: find('bfbc1.exe', 'bfbc1-recomp/build/game/bfbc1.exe'),
    data: findDir('bfbc1/extracted/default.xex'),
    textures: findDir('texpack_bfbc1/install/replacements.txt'),
    dataHint: 'bfbc1\extracted',
  },
};
function problem(g) {
  return !g.exe ? `${g.title}: exe not found next to the launcher.`
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
    width: 920, height: 640, resizable: false, maximizable: false, frame: false,
    backgroundColor: '#0E1013', show: false, title: 'Battlefield 1943 / Bad Company',
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true },
  });
  win.setMenu(null);
  win.loadFile('index.html');
  win.once('ready-to-show', () => win.show());
}

ipcMain.handle('state', () => ({
  settings: load(),
  games: Object.fromEntries(Object.entries(games).map(([k, g]) =>
    [k, { title: g.title, error: problem(g), hasTextures: !!g.textures }])),
}));

ipcMain.handle('play', (_, s) => {
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

  const args = ['--game', gameData, '--user', 'userdata', '--gamertag', s.name,
    '--res-scale', s.resolution, '--sharpen', s.upscaling,
    '--preset', s.preset, '--max-fps', s.frameRate, '--window', s.window];
  if (s.textures) args.push('--textures', texturePack);
  if (s.blood && s.game !== 'bfbc1') args.push('--blood');  // BF1943 only
  if (s.hdr) args.push('--hdr');

  return new Promise((resolve) => {
    const game = spawn(exe, args, { cwd: workDir, windowsHide: true, stdio: 'ignore' });
    game.once('error', (e) => resolve({ error: 'Could not start the game: ' + e.message }));
    game.once('spawn', () => { win.minimize(); resolve({}); });
    game.once('exit', () => { win.restore(); win.focus(); win.webContents.send('game-exited'); });
  });
});

ipcMain.on('minimize', () => win.minimize());
ipcMain.on('close', () => win.close());

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());
