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

function buildRows() {
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
  settings.game = key;
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
}

async function init() {
  const state = await window.launcher.state();
  settings = state.settings;
  games = state.games;
  buildRows();

  $('name').value = settings.name;
  $('blood').checked = !!settings.blood;
  $('hdr').checked = !!settings.hdr;
  for (const tab of document.querySelectorAll('.game')) {
    const g = games[tab.dataset.game];
    tab.disabled = !!g.error;       // a port without its exe or game files stays greyed out
    const badge = tab.querySelector('.soon');
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
