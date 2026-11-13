// Settings rows: [settings key, label, [[button text, value], ...]]
const rows = [
  ['display', 'Display', [['Window', 'windowed'], ['Fullscreen', 'fullscreen']]],
  ['window', 'Resolution', [['480p', '854x480'], ['720p', '1280x720'], ['1080p', '1920x1080'],
    ['1440p', '2560x1440'], ['4K', '3840x2160']]],
  ['resolution', 'Render scale', [['1×', '1'], ['2×', '2'], ['3×', '3']]],
  ['upscaling', 'Upscaling', [['FSR', 'fsr'], ['FSR 3', 'fsr3'], ['DLSS', 'dlss'], ['Auto', 'auto'], ['CAS', 'cas'], ['Off', 'off']]],
  ['upscaleQuality', 'Upscale quality', [['Off', 'off'], ['Quality', 'quality'], ['Balanced', 'balanced'], ['Performance', 'performance']]],
  ['preset', 'Look', [['Stock', 'stock'], ['Enhanced', 'enhanced'], ['Vivid', 'vivid'], ['Toon', 'toon'], ['Noir', 'noir'], ['WW2', 'ww2']]],
  ['frameRate', 'Frame rate', [['60', '60'], ['120', '120'], ['144', '144'], ['Max', '0']]],
  ['shadowDetail', 'Shadow detail', [['Low', 'low'], ['Medium', 'medium'], ['High', 'high'], ['Ultra', 'ultra']]],
];

// The EFFECTS card: the game's PC SETTINGS > EFFECTS, the same values (the
// game reads fx_<key> from these settings; one not set here is the game's
// own). [key, label, [[text, value], ...], the game's default]
const offOn = [['Off', '0'], ['On', '1']];
const fxRows = [
  ['fx_quality', 'Quality preset', [['Low', 'low'], ['Medium', 'medium'], ['High', 'high'], ['Ultra', 'ultra'],
    ['Custom', 'custom']], 'high'],
  ['fx_rt', 'Ray-traced shadows', offOn, '1'],
  ['fx_shadows', 'Screen shadows', [['Off', '0'], ['Low', '0.5'], ['Full', '1']], '1'],
  ['fx_gi', 'Bounce light', [['Off', '0'], ['Low', '0.5'], ['Normal', '1'], ['High', '1.5']], '1'],
  ['fx_ao', 'Ambient occlusion', [['Off', '0'], ['Low', '0.5'], ['Normal', '1'], ['High', '1.5']], '1'],
  ['fx_clouds', 'Clouds', [['Off', '0'], ['Few', '0.3'], ['Normal', '0.45'], ['Many', '0.65']], '0.45'],
  ['fx_cloud_shadows', 'Cloud shadows', [['Off', '0'], ['On', '0.85']], '0.85'],
  ['fx_density', 'Fog', [['Light', '0.0009'], ['Normal', '0.0018'], ['Thick', '0.0036']], '0.0018'],
  ['fx_shafts', 'Light shafts', [['Off', '0'], ['Normal', '0.6'], ['Strong', '0.9']], '0.6'],
  ['fx_reflect', 'Sea reflections', offOn, '1'],
  ['fx_gloss', 'Surface reflections', offOn, '1'],
  ['fx_water_foam', 'Water foam', offOn, '1'],
  ['fx_water_refract', 'Water refraction', offOn, '1'],
  ['fx_water_shallow', 'Shallow water', offOn, '1'],
  ['fx_detail', 'Surface detail', [['Off', '0'], ['On', '1'], ['Strong', '1.5']], '1'],
  ['fx_particle_quality', 'Particle quality', [['Game', '0'], ['High', '1']], '1'],
  ['fx_soft_particles', 'Soft particles', offOn, '1'],
  ['fx_explosion_smoke', 'Explosion smoke', offOn, '0'],
  ['fx_flash', 'Muzzle flash light', offOn, '1'],
];
// The quality presets' effect values (pc_settings.cpp kPresets: low, medium, high, ultra).
const fxPresets = {
  rt: ['0', '0', '1', '1'], shadows: ['0', '1', '1', '1'], gi: ['0', '0', '1', '1'], ao: ['0', '0.5', '1', '1'],
  clouds: ['0.3', '0.45', '0.45', '0.45'], cloud_shadows: ['0', '0.85', '0.85', '0.85'], shafts: ['0', '0.6', '0.6', '0.6'],
  reflect: ['0', '1', '1', '1'], gloss: ['0', '0', '1', '1'], water_foam: ['0', '0', '1', '1'],
  water_refract: ['0', '1', '1', '1'], water_shallow: ['0', '1', '1', '1'], detail: ['0', '1', '1', '1.5'],
  particle_quality: ['0', '1', '1', '1'], soft_particles: ['0', '1', '1', '1'], explosion_smoke: ['0', '0', '0', '1'],
};
function buildFxRows() {
  $('fxrows').textContent = '';
  const value = (key, def) => (settings[key] !== undefined ? String(settings[key]) : def);
  for (const [key, label, options, def] of fxRows) {
    const row = document.createElement('div');
    row.className = 'row';
    row.innerHTML = `<div class="label">${label}</div><div class="seg"></div>`;
    const seg = row.querySelector('.seg');
    for (const [text, v] of options) {
      const b = document.createElement('button');
      b.textContent = text;
      b.classList.toggle('on', value(key, def) === v);
      b.onclick = () => {
        if (key === 'fx_quality') {
          if (v === 'custom') return;  // (shown, not picked: changing an effect makes it custom)
          const i = ['low', 'medium', 'high', 'ultra'].indexOf(v);
          for (const [k, vals] of Object.entries(fxPresets)) settings['fx_' + k] = vals[i];
        } else {
          settings.fx_quality = 'custom';
        }
        settings[key] = v;
        buildFxRows();
      };
      seg.append(b);
    }
    $('fxrows').append(row);
  }
}

const $ = (id) => document.getElementById(id);
let settings = {};
let games = {};

function setStatus(text, error = false) {
  $('status').textContent = text;
  $('status').classList.toggle('error', error);
}

// Render scale is remembered per game: Bad Company's levels are heavy, so it starts at 1x.
const gameScale = { bf1943: '2', bfbc1: '1' };

function buildRows() {
  $('rows').textContent = '';
  for (const [key, label, options] of rows) {
    const row = document.createElement('div');
    row.className = 'row';
    row.innerHTML = `<div class="label">${label}</div><div class="seg"></div>`;
    const seg = row.querySelector('.seg');
    for (const [text, value] of options) {
      const b = document.createElement('button');
      b.textContent = text;
      b.classList.toggle('on', settings[key] === value);
      b.onclick = () => {
        settings[key] = value;
        seg.querySelectorAll('button').forEach((x) => x.classList.toggle('on', x === b));
      };
      seg.append(b);
    }
    // Unknown saved value: fall back to the first option.
    if (!seg.querySelector('.on')) seg.firstChild.click();
    $('rows').append(row);
  }
}

// The HD textures row: the switch, the hint and MAKE (UPDATE once there is a
// pack: carries on an unfinished one and installs it again).
function showTextures(key) {
  const g = games[key];
  const mine = texBusy && texGame === key;  // (one pack at a time: the other game's tab waits)
  $('textures').checked = settings.textures && g.hasTextures;
  $('textures').disabled = !g.hasTextures;
  if (!mine) $('texhint').textContent = texBusy ? `Making the ${games[texGame].title} pack first…`
    : g.hasTextures ? 'AI-upscaled texture pack' : 'Texture pack not found';
  $('texmake').hidden = false;
  $('texmake').disabled = texBusy && !mine;
  $('texmake').textContent = mine ? 'STOP' : g.hasTextures ? 'UPDATE' : 'MAKE';
}

// Shows the chosen game's tab, logo and options.
function selectGame(key) {
  const g = games[key];
  if (settings.game && settings.game !== key) {  // keep the old game's scale, load the new one's
    settings.scale = { ...settings.scale, [settings.game]: settings.resolution };
  }
  settings.game = key;
  settings.resolution = (settings.scale && settings.scale[key]) || gameScale[key];
  buildRows();
  buildFxRows();
  document.querySelectorAll('.game').forEach((t) => t.classList.toggle('on', t.dataset.game === key));
  $('year').textContent = key === 'bfbc1' ? 'BAD COMPANY' : '1943';
  $('year').classList.toggle('long', key === 'bfbc1');
  showTextures(key);
  document.title = g.title;
  setStatus(g.error || '', !!g.error);
  $('play').disabled = !!g.error;
  showSetup(key, !!g.error);
  showServer(key);
}

// ---- first-run setup: the game built here from the player's own copy ----------
const setupText = {
  bf1943: ['Set up Battlefield 1943', 'The game is built on this PC from your own copy. Choose your game files: the Xbox 360 '
    + 'package of Battlefield 1943 (version 1.06), or <b>default.xex</b> in a folder you unpacked.'],
  bfbc1: ['Set up Bad Company', 'The game is built on this PC from your own copy. Choose your game files: the disc image '
    + '(<b>.iso</b>) of Battlefield: Bad Company (USA), or <b>default.xex</b> in a folder you unpacked. '
    + 'The game files take about 5 GB.'],
};
const foundHint = ' Put them next to the launcher and they are found by themselves.';
// Linux: build_linux.sh, with the distribution's own packages (installed after asking for the password).
if (window.launcher.platform === 'linux')
  document.querySelector('.steps li[data-step="tools"]').lastChild.textContent =
    'Build tools (your distribution\'s packages: clang, CMake, Vulkan)';
let gamePath = null;
let setupGame = null;
let setupState = null;
let setupBusy = false;
async function showSetup(key, on) {
  $('setupcard').hidden = !on;
  $('graphicscard').hidden = on;
  if (!on || setupBusy) return;
  if (setupGame !== key) {  // another game: its own files to choose
    setupGame = key;
    gamePath = null;
    $('pickpath').textContent = 'No files chosen';
    document.querySelectorAll('.steps li').forEach((li) => (li.className = ''));
    logLines.length = 0;
    $('setuplog').textContent = '';
    $('barfill').style.width = '0%';
    $('install').textContent = 'INSTALL';
  }
  const rebuild = !!(games[key] && games[key].outdated);
  $('setuptitle').textContent = rebuild ? setupText[key][0].replace('Set up', 'Update') : setupText[key][0];
  $('setuphint').innerHTML = rebuild
    ? 'A new version of the port is installed: the game is built again from your own files (they are kept).'
    : setupText[key][1] + foundHint;
  if (rebuild && $('install').textContent === 'INSTALL') $('install').textContent = 'UPDATE';
  $('pick').disabled = false;
  setupState = await window.launcher.setupStatus(key);
  if (!setupState.ok) {
    setStatus(setupState.error, true);
    $('pick').disabled = $('install').disabled = true;
    return;
  }
  setStatus(games[key] && games[key].outdated ? 'Update the game first (right).' : 'Set up the game first (right).');
  // The game's files next to the launcher, unless the player chose others.
  const found = setupState.found;
  if (!gamePath && found && found.path && !setupState.gameFiles) {
    gamePath = found.path;
    $('pickpath').textContent = 'Found next to the launcher: ' + found.path.split(/[\\/]/).slice(-2).join('\\');
    $('pickpath').title = found.path;
  } else if (!gamePath && found && found.wrong && !setupState.gameFiles) {
    $('pickpath').textContent = found.wrong;
  }
  if (setupState.gameFiles) {
    $('pickpath').textContent = 'Game files already in place';
    markStep('game', 'done');
  }
  $('install').disabled = !(gamePath || setupState.gameFiles);
}
function markStep(name, state) {
  const li = document.querySelector(`.steps li[data-step="${name}"]`);
  if (li) li.className = state;
}
const logLines = [];
function addLog(line) {
  logLines.push(line);
  if (logLines.length > 6) logLines.shift();
  $('setuplog').textContent = logLines.join('\n');
}
$('pick').onclick = async () => {
  const p = await window.launcher.setupPick(setupGame);
  if (!p) return;
  gamePath = p;
  $('pickpath').textContent = p;
  $('pickpath').title = p;
  markStep('game', '');
  $('install').disabled = false;
};
$('install').onclick = async () => {
  $('install').disabled = $('pick').disabled = true;
  $('install').textContent = 'INSTALLING…';
  setStatus('Setting up… keep the launcher open.');
  setupBusy = true;
  document.querySelectorAll('.game').forEach((t) => (t.disabled = true));  // one setup at a time
  const r = await window.launcher.setupRun(setupGame, gamePath);
  setupBusy = false;
  document.querySelectorAll('.game').forEach((t) => (t.disabled = false));
  if (r.error) {
    setStatus(r.error, true);
    $('install').textContent = 'TRY AGAIN';
    $('install').disabled = $('pick').disabled = false;
    return;
  }
  $('barfill').style.width = '100%';
  setStatus('Ready. Have fun!');
  await init();
};
window.launcher.onSetupEvent((ev) => {
  if (ev.step) {
    markStep(ev.step, ev.state);
    if (ev.state === 'run' && ev.text) addLog(ev.text);
    if (ev.state === 'fail' && ev.text) addLog('Failed: ' + ev.text);
  }
  if (ev.log) addLog(ev.log);
  if (ev.progress !== undefined) $('barfill').style.width = Math.round(ev.progress * 100) + '%';
});

async function init() {
  const state = await window.launcher.state();
  settings = state.settings;
  games = state.games;
  // The saved render scale belongs to the game that was last played.
  settings.scale = { ...settings.scale, [settings.game || 'bf1943']: settings.resolution };

  $('name').value = settings.name;
  $('blood').checked = !!settings.blood;
  $('hdr').checked = !!settings.hdr;
  for (const tab of document.querySelectorAll('.game')) {
    const g = games[tab.dataset.game];
    const badge = tab.querySelector('.soon');  // not installed yet: its tab opens the setup
    if (badge) badge.hidden = !g.error;
    tab.onclick = () => selectGame(tab.dataset.game);
  }
  serverStatus(await window.launcher.serverStatus());
  selectGame(games[settings.game] && !games[settings.game].error ? settings.game : 'bf1943');
  return state;
}

$('play').onclick = async () => {
  settings.name = $('name').value;
  settings.textures = $('textures').checked;
  settings.blood = $('blood').checked;
  settings.hdr = $('hdr').checked;
  settings.scale = { ...settings.scale, [settings.game]: settings.resolution };
  $('play').disabled = true;
  setStatus('Starting…');
  const result = await window.launcher.play(settings);
  if (result.error) { setStatus(result.error, true); $('play').disabled = false; }
  else setStatus('Running');
};

window.launcher.onGameExited(() => { setStatus(''); $('play').disabled = false; });

// The HD texture pack, made here (texpack.py make, through the main process).
let texBusy = false, texGame = null;  // (the game whose pack is being made)
const texSteps = { fetch: 'Getting the upscaler', extract: 'Reading the game\'s textures', build: 'Upscaling',
  install: 'Installing the pack' };
$('texmake').onclick = async () => {
  if (texBusy) { if (texGame === settings.game) await window.launcher.texturesCancel(); return; }
  // Busy before asking: a job that fails at once can finish before the answer.
  texBusy = true, texGame = settings.game;
  $('texmake').textContent = 'STOP';
  $('texhint').textContent = 'Starting…';
  const r = await window.launcher.texturesMake(texGame);
  if (r.error) { texBusy = false; showTextures(settings.game); $('texhint').textContent = r.error; }
};
window.launcher.onTexturesEvent(async (ev) => {
  if ((ev.step || (ev.line && !ev.finished)) && settings.game !== texGame) return;  // (another tab: shown there)
  if (ev.step) {
    const pct = ev.total ? ` ${Math.floor((100 * ev.done) / ev.total)}%` : '';
    $('texhint').textContent = `${texSteps[ev.step] || ev.step}${pct}`;
  } else if (ev.finished) {
    texBusy = false;
    const s = await window.launcher.state();
    games = s.games;
    showTextures(settings.game);  // (not selectGame: it would undo unsaved choices)
    if (ev.game && ev.game !== settings.game) return;  // (the other game's tab is shown)
    $('texhint').textContent = ev.code === 0 ? 'Texture pack made'
      : ev.stopped ? 'Texture pack stopped: MAKE carries on' : `Texture pack failed: ${ev.error || `exit code ${ev.code}`}`;
  } else if (ev.line && /^(Could not|Real-ESRGAN|Python|texpack)/.test(ev.line)) {
    $('texhint').textContent = ev.line;
  }
});
// Back to the launcher: game files may have been put next to it meanwhile.
window.addEventListener('focus', () => {
  if (!$('setupcard').hidden && setupGame) showSetup(setupGame, true);
});
// The side menu: its pages (home, server, settings).
document.querySelectorAll('#nav > button[data-page]').forEach((b) => {
  b.onclick = () => {
    document.querySelectorAll('#nav > button[data-page]').forEach((x) => x.classList.toggle('active', x === b));
    document.querySelectorAll('.page').forEach((p) => p.classList.toggle('active', p.id === 'page-' + b.dataset.page));
  };
});
const showPage = (name) => document.querySelector(`#nav > button[data-page="${name}"]`).click();

// ---- the dedicated server (SERVER page) ---------------------------------------
// The game chosen at the top; its choices kept in settings.server.<game> (saved
// as they change). START runs it through the main process: the bots' advisor
// installed the first time, the maps' missing nav meshes made, then the server.
const serverMaps = {
  bf1943: [['Wake_island_s', 'Wake Island'], ['Iwo_Jima_s', 'Iwo Jima'], ['Guadal_Canal', 'Guadalcanal'],
    ['Coral_sea', 'Coral Sea (air)']],
  bfbc1: [['mp_harbour_day1', 'Valley Run'], ['mp_harbour_day2', 'Deconstruction'],
    ['mp_ignition_day1', 'Oasis'], ['mp_ignition_day2', 'Final Ignition'],
    ['mp_monastery_day1', 'End of the Line'], ['mp_monastery_day2', 'Ascension'],
    ['mp_beachhead_day1', 'Harvest Day'], ['mp_beachhead_day2', 'Over and Out']],
};
const serverDefaults = (key) => ({ name: 'My Server', maps: serverMaps[key].map(([m]) => m), bots: 8, publish: false,
  llm: false, train: false, trainMinutes: 30, exe: '', game: '' });
let srv = null;          // the choices on the page: { key, c }
let srvInfo = {};        // what the main process found: exe, data, advisor
let srvRunning = null;   // the game whose server runs
let srvPhase = '';       // advisor | navs | server
const kSrvLogLines = 2000;

function showServer(key) {
  if (srvRunning) key = srvRunning;  // (the running server stays on the page)
  if (srv && srv.key === key) return;
  srv = { key, c: { ...serverDefaults(key), ...(settings.server && settings.server[key]) } };
  srvInfo = {};
  const c = srv.c;
  c.maps = c.maps.filter((m) => serverMaps[key].some(([k]) => k === m));
  $('srvgame').textContent = `${games[key].title} · dedicated server`;
  $('srvname').value = c.name;
  $('srvmaps').textContent = '';
  for (const [map, label] of serverMaps[key]) {
    const b = document.createElement('button');
    b.className = 'chip';
    b.dataset.map = map;
    b.innerHTML = '<span><b></b><small></small></span>';
    b.querySelector('b').textContent = label;
    b.querySelector('small').textContent = map;
    b.classList.toggle('on', c.maps.includes(map));
    b.onclick = () => {
      // (in the game's own order)
      const on = !c.maps.includes(map);
      c.maps = serverMaps[key].map(([m]) => m).filter((m) => (m === map ? on : c.maps.includes(m)));
      b.classList.toggle('on', on);
      serverChanged();
    };
    $('srvmaps').append(b);
  }
  $('srvpublish').checked = !!c.publish;
  $('srvllm').checked = !!c.llm;
  $('srvtrain').checked = !!c.train;
  serverControls();
  refreshServerInfo();
}

// Training minutes: finer steps for short runs.
const minuteStep = (m, up) => (up ? (m < 60 ? 5 : m < 180 ? 15 : 60) : (m <= 60 ? 5 : m <= 180 ? 15 : 60));
const minuteText = (m) => (m < 60 ? `${m} min` : `${Math.floor(m / 60)} h${m % 60 ? ` ${m % 60}` : ''}`);

// The controls from the choices (and locked while a server runs).
function serverControls() {
  const c = srv.c, busy = !!srvRunning;
  $('bots').textContent = c.bots;
  $('botsdown').disabled = busy || c.bots <= 0;
  $('botsup').disabled = busy || c.bots >= 16;
  $('botshint').textContent = c.train ? 'A training run plays with 16' : 'Computer players, half on each team';
  $('minutes').textContent = minuteText(c.trainMinutes);
  // (the length can be set before the run is turned on)
  $('mindown').disabled = busy || c.trainMinutes <= 5;
  $('minup').disabled = busy || c.trainMinutes >= 1440;
  $('minutesbox').classList.toggle('dim', !c.train);
  $('srvlocknote').hidden = !busy;
  $('srvpublish').disabled = busy || c.train;
  $('publishrow').classList.toggle('off', c.train);
  $('publishhint').textContent = c.train ? 'Not with a training run: nobody joins it'
    : 'Your router must pass the game\'s UDP ports (1003, 19430) to this PC';
  $('mapcount').textContent = `${c.maps.length} of ${serverMaps[srv.key].length} in the rotation`;
  for (const id of ['srvname', 'srvllm', 'srvtrain', 'srvexepick', 'srvdatapick', 'srvauto']) $(id).disabled = busy;
  $('srvmaps').querySelectorAll('.chip').forEach((b) => (b.disabled = busy));
  $('srvchoices').classList.toggle('locked', busy);
  $('srvstart').disabled = busy || !srvInfo.exe || !srvInfo.data || !c.maps.length;
  $('srvstop').disabled = !busy;
}

let srvSaveTimer = null;
function serverChanged() {
  settings.server = { ...settings.server, [srv.key]: srv.c };
  serverControls();
  clearTimeout(srvSaveTimer);
  srvSaveTimer = setTimeout(() => window.launcher.saveSettings({ server: settings.server }), 500);
}

async function refreshServerInfo() {
  const key = srv.key;
  const info = await window.launcher.serverInfo(key, { exe: srv.c.exe, game: srv.c.game });
  if (!srv || srv.key !== key) return;
  srvInfo = info;
  const show = (id, p, missing) => {
    $(id).textContent = p || missing;
    $(id).title = p || '';
    $(id).classList.toggle('missing', !p);
  };
  show('srvexe', info.exe, 'Not found: set up the game (BATTLE), or choose it');
  show('srvdata', info.data, 'Not found: choose the folder with default.xex');
  $('srvauto').hidden = !srv.c.exe && !srv.c.game;
  $('llmhint').textContent = 'A small AI model on this PC tells stuck or losing bots where to go'
    + (info.advisor ? ' (installed)' : ' (installed the first time, about 3.5 GB)');
  if (!srvRunning) {
    const missing = !info.exe ? 'The game program was not found (Program, below).'
      : !info.data ? 'The game files were not found (Program, below).' : '';
    serverStateText(missing || 'Stopped', !!missing);
  }
  serverControls();
}

function serverStateText(text, error = false, ok = false) {
  $('srvstate').textContent = text;
  $('srvstate').title = text;
  $('srvstate').classList.toggle('error', error);
  $('srvstate').classList.toggle('ok', ok);
}
function serverBadge() {
  const b = $('srvbadge');
  b.className = 'srv-badge' + (!srvRunning ? '' : srvPhase === 'server' ? ' run' : ' busy');
  b.textContent = !srvRunning ? 'STOPPED' : srvPhase === 'advisor' ? 'INSTALLING' : srvPhase === 'navs' ? 'NAV MESHES'
    : srvPhase === 'server' ? 'RUNNING' : 'STARTING';
  $('navsrvdot').hidden = !srvRunning;
}

// The log: the last lines, coloured (the launcher's own lines, warnings), kept
// at the bottom unless scrolled up (also while the page is hidden).
let srvFollow = true;
$('srvlog').addEventListener('scroll', () => {
  const log = $('srvlog');
  if (log.clientHeight) srvFollow = log.scrollHeight - log.scrollTop - log.clientHeight < 24;
});
const followLog = () => { if (srvFollow) $('srvlog').scrollTop = $('srvlog').scrollHeight; };
document.querySelector('#nav > button[data-page="server"]').addEventListener('click', followLog);
function addServerLines(lines) {
  const log = $('srvlog');
  const frag = document.createDocumentFragment();
  for (const line of lines) {
    const s = document.createElement('span');
    s.textContent = line + '\n';
    if (/^== /.test(line)) s.className = /^== (Warning|Error|Could not|The advisor could not)/.test(line) ? 'warn' : 'note';
    else if (/\b(error|failed|fatal)\b/i.test(line)) s.className = 'warn';
    frag.append(s);
  }
  log.append(frag);
  while (log.childElementCount > kSrvLogLines) log.firstChild.remove();
  followLog();
}

// The main process's state (the launcher's page loaded again, or at the start).
function serverStatus(st) {
  srvRunning = st.running ? st.game : null;
  srvPhase = (st.running && st.phase) || '';
  $('srvlog').textContent = '';
  addServerLines(st.log || []);
  if (srvRunning) {
    srv = null;
    showServer(srvRunning);
    serverStateText(st.state || (srvPhase === 'server' ? 'Running' : 'Starting…'), false, !!st.state);
  }
  serverBadge();
}

window.launcher.onServerEvent((ev) => {
  if (ev.started) {
    srvRunning = ev.started;
    srvPhase = '';
    $('srvlog').textContent = '';
    srvFollow = true;
    serverStateText('Starting…');
    serverControls();
  }
  if (ev.lines) addServerLines(ev.lines);
  if (ev.phase) {
    srvPhase = ev.phase;
    $('srvbar').hidden = ev.phase === 'server';
    $('srvbarfill').style.width = '0%';
    serverStateText(ev.phase === 'advisor' ? 'Installing the bot advisor…'
      : ev.phase === 'navs' ? 'Making the nav meshes…' : 'Starting the server…');
  }
  if (ev.progress) {
    const { what, done, total } = ev.progress;
    $('srvbar').hidden = false;
    $('srvbarfill').style.width = total ? `${Math.min(100, (100 * done) / total)}%` : '0%';
    serverStateText(what === 'nav' ? `Making the nav meshes: ${done} of ${total}`
      : `Downloading ${what}: ${(done / 1e9).toFixed(2)} of ${(total / 1e9).toFixed(2)} GB`);
  }
  if (ev.state) serverStateText(ev.state, false, true);
  if (ev.finished) {
    srvRunning = null;
    srvPhase = '';
    $('srvbar').hidden = true;
    serverStateText('Stopped');
    // Back to the game chosen at the top (another one may have been picked meanwhile).
    srv = null;
    showServer(settings.game);
  }
  serverBadge();
  if (ev.started || ev.finished) serverControls();
});

$('srvname').addEventListener('input', () => { srv.c.name = $('srvname').value; serverChanged(); });
$('srvname').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !$('srvstart').disabled) $('srvstart').click(); });
$('botsdown').onclick = () => { srv.c.bots = Math.max(0, srv.c.bots - 1); serverChanged(); };
$('botsup').onclick = () => { srv.c.bots = Math.min(16, srv.c.bots + 1); serverChanged(); };
$('mindown').onclick = () => { srv.c.trainMinutes = Math.max(5, srv.c.trainMinutes - minuteStep(srv.c.trainMinutes, false)); serverChanged(); };
$('minup').onclick = () => { srv.c.trainMinutes = Math.min(1440, srv.c.trainMinutes + minuteStep(srv.c.trainMinutes, true)); serverChanged(); };
for (const [id, key] of [['srvpublish', 'publish'], ['srvllm', 'llm'], ['srvtrain', 'train']])
  $(id).onchange = () => { srv.c[key] = $(id).checked; serverChanged(); };
for (const [id, kind] of [['srvexepick', 'exe'], ['srvdatapick', 'game']])
  $(id).onclick = async () => {
    const p = await window.launcher.serverPick(kind);
    if (!p) return;
    srv.c[kind] = p;
    serverChanged();
    refreshServerInfo();
  };
$('srvauto').onclick = () => { srv.c.exe = srv.c.game = ''; serverChanged(); refreshServerInfo(); };
$('srvstart').onclick = async () => {
  srv.c.name = $('srvname').value.trim() || 'My Server';
  $('srvname').value = srv.c.name;
  serverChanged();
  $('srvstart').disabled = true;
  const r = await window.launcher.serverStart(srv.key, srv.c);
  if (r.error) { serverStateText(r.error, true); serverControls(); return; }
  $('srvlog').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
};
$('srvstop').onclick = () => { $('srvstop').disabled = true; window.launcher.serverStop(); };
$('srvreplay').onclick = async () => {
  const r = await window.launcher.serverReplay(srv.key);
  if (r.error && !srvRunning) serverStateText(r.error, true);
  else if (r.error) addServerLines([`== ${r.error}`]);
};
$('srvfolder').onclick = async () => {
  const r = await window.launcher.serverFolder(srv.key);
  if (r.error) serverStateText(r.error, true);
};
$('min').onclick = () => window.launcher.minimize();
$('close').onclick = () => window.launcher.close();
$('name').addEventListener('keydown', (e) => { if (e.key === 'Enter') $('play').click(); });

// Updates: the newest release on GitHub, installed by itself (Automatic
// updates) or with the UPDATE button; the launcher then restarts as the new one.
async function runUpdate(version) {
  $('updatebtn').disabled = true;
  setStatus(`Downloading version ${version}…`);
  const r = await window.launcher.updateRun();
  if (r.error) {
    setStatus(r.error, true);
    $('updatebtn').disabled = false;
  } else {
    setStatus(`Starting version ${version}…`);
  }
}
window.launcher.onUpdateEvent((ev) => {
  if (ev.progress !== undefined) setStatus(`Downloading the update… ${Math.round(ev.progress * 100)}%`);
});
async function checkUpdate() {
  const u = await window.launcher.updateCheck();
  if (!u.available) return;
  $('updatebtn').textContent = `UPDATE TO ${u.version}`;
  $('updatebtn').title = u.notes || '';
  $('updatebtn').hidden = false;
  $('updatebtn').onclick = () => runUpdate(u.version);
  if (settings.autoUpdate !== false && !setupBusy) runUpdate(u.version);
}
$('autoupdate').onchange = () => {
  settings.autoUpdate = $('autoupdate').checked;
  window.launcher.saveSettings(settings);
};

init().then((state) => {
  if (state.page === 'server' || state.page === 'settings') showPage(state.page);  // (--server, --page)
  if (state.page === 'effects') showPage('settings'), $('effectscard').scrollIntoView();
  $('autoupdate').checked = settings.autoUpdate !== false;
  checkUpdate();
});
