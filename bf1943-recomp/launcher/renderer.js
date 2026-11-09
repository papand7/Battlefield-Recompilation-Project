// Settings rows: [settings key, label, [[button text, value], ...]]
const rows = [
  ['window', 'Resolution', [['480p', '854x480'], ['720p', '1280x720'], ['1080p', '1920x1080'],
    ['1440p', '2560x1440'], ['4K', '3840x2160']]],
  ['resolution', 'Render scale', [['1×', '1'], ['2×', '2'], ['3×', '3']]],
  ['upscaling', 'Upscaling', [['FSR', 'fsr'], ['CAS', 'cas'], ['Off', 'off']]],
  ['preset', 'Look', [['Stock', 'stock'], ['Enhanced', 'enhanced'], ['Toon', 'toon'], ['Noir', 'noir'], ['WW2', 'ww2']]],
  ['frameRate', 'Frame rate', [['60', '60'], ['120', '120'], ['144', '144'], ['Max', '0']]],
];

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

// Shows the chosen game's tab, logo and options.
function selectGame(key) {
  const g = games[key];
  if (settings.game && settings.game !== key) {  // keep the old game's scale, load the new one's
    settings.scale = { ...settings.scale, [settings.game]: settings.resolution };
  }
  settings.game = key;
  settings.resolution = (settings.scale && settings.scale[key]) || gameScale[key];
  buildRows();
  document.querySelectorAll('.game').forEach((t) => t.classList.toggle('on', t.dataset.game === key));
  $('year').textContent = key === 'bfbc1' ? 'BAD COMPANY' : '1943';
  $('year').classList.toggle('long', key === 'bfbc1');
  $('bloodrow').hidden = key === 'bfbc1';  // BF1943 only
  $('textures').checked = settings.textures && g.hasTextures;
  $('textures').disabled = !g.hasTextures;
  $('texhint').textContent = g.hasTextures ? 'AI-upscaled texture pack' : 'Texture pack not found';
  document.title = g.title;
  setStatus(g.error || '', !!g.error);
  $('play').disabled = !!g.error;
  showSetup(key, !!g.error);
}

// ---- first-run setup: the game built here from the player's own copy ----------
const setupText = {
  bf1943: ['Set up Battlefield 1943', 'The game is built on this PC from your own copy. Choose your game files: the Xbox 360 '
    + 'package of Battlefield 1943 (version 1.06), or <b>default.xex</b> in a folder you unpacked.'],
  bfbc1: ['Set up Bad Company', 'The game is built on this PC from your own copy. Choose your game files: the disc image '
    + '(<b>.iso</b>) of Battlefield: Bad Company (USA), or <b>default.xex</b> in a folder you unpacked. '
    + 'The game files take about 5 GB.'],
};
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
  $('setuptitle').textContent = setupText[key][0];
  $('setuphint').innerHTML = setupText[key][1];
  $('pick').disabled = false;
  setupState = await window.launcher.setupStatus(key);
  if (!setupState.ok) {
    setStatus(setupState.error, true);
    $('pick').disabled = $('install').disabled = true;
    return;
  }
  setStatus('Set up the game first (right).');
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
  selectGame(games[settings.game] && !games[settings.game].error ? settings.game : 'bf1943');
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
$('min').onclick = () => window.launcher.minimize();
$('close').onclick = () => window.launcher.close();
$('name').addEventListener('keydown', (e) => { if (e.key === 'Enter') $('play').click(); });

init();
