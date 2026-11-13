// First-run setup on Linux, run by the launcher in place of setup.js (the same
// steps the window shows: game files, build tools, recompile, build). The game
// files are found and checked as on Windows (setup.js); the rest is
// build_linux.sh, which checks the packages, builds XenonRecomp, recompiles,
// builds the shaders and the game with clang. Missing packages are installed
// with the distribution's package manager through pkexec (the system asks for
// the password), then the build runs again. Both games: Bad Company is
// build_linux.sh --bfbc1 (its tree, src-bfbc1, with the shared code from src).
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const base = require('./setup');

// Per game: the exe and the build's folder under <root> (the game's folder,
// setup.js's gameDir: game, game-bfbc1).
const kGame = {
  bf1943: { exe: 'bf1943', buildOut: 'build-linux', args: [] },
  bfbc1: { exe: 'bfbc1', buildOut: 'build-linux-bfbc1', args: ['--bfbc1'] },
};

const onPath = (cmd) => (process.env.PATH || '').split(':').some((d) => {
  try { fs.accessSync(path.join(d, cmd), fs.constants.X_OK); return true; } catch { return false; }
});

function findTools() {
  return { python: onPath('python3') ? 'python3' : null, bash: onPath('bash') ? 'bash' : null };
}

// The game's folder (main.js finds the exe there) and the build's own.
function paths(launcherDir, game) {
  const p = kGame[game] && base.paths(launcherDir, game);
  return p && { ...p, linux: kGame[game], out: path.join(p.root, kGame[game].buildOut) };
}

function status(launcherDir, game, searchDir) {
  const p = paths(launcherDir, game);
  if (!p) return { ok: false, error: 'The build files (src folder) are missing next to the launcher.' };
  if (!fs.existsSync(path.join(p.src, 'build_linux.sh'))) return { ok: false, error: 'build_linux.sh is missing from the build files.' };
  const tools = findTools();
  return {
    ok: true,
    gameFiles: fs.existsSync(path.join(p.data, 'default.xex')),
    built: fs.existsSync(path.join(p.gameDir, p.linux.exe)),
    found: base.findGameFiles(searchDir, game),
    tools: { python: !!tools.python, bash: !!tools.bash },
  };
}

// Runs a program; every output line to onLine. Resolves to the exit code.
function run(cmd, args, opts, onLine) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, opts);
    let rest = '';
    const feed = (buf) => {
      const lines = (rest + buf.toString()).split(/\r?\n/);
      rest = lines.pop();
      for (const line of lines) if (line.trim()) onLine(line);
    };
    p.stdout.on('data', feed);
    p.stderr.on('data', feed);
    p.on('error', reject);
    p.on('close', (code) => { if (rest.trim()) onLine(rest); resolve(code); });
  });
}

// build_linux.sh's install command for this system ("  sudo apt install …"),
// made to run without questions: [program, args] or null.
function installCommand(lines) {
  const i = lines.findIndex((l) => /^Install them with:$/.test(l.trim()));
  const cmd = i >= 0 && lines[i + 1] && lines[i + 1].trim();
  if (!cmd || !cmd.startsWith('sudo ')) return null;
  const words = cmd.slice(5).split(/\s+/);
  if (words[0] === 'apt') words.splice(2, 0, '-y');
  else if (words[0] === 'dnf') words.splice(2, 0, '-y');
  else if (words[0] === 'pacman') words.splice(1, 0, '--noconfirm');
  else return null;
  return words;
}

// build_linux.sh's steps on the window's four (its own check of the game files
// is after the packages': shown as part of the tools step).
const kStepOf = { check: 'tools', game: 'tools', tools: 'tools', config: 'recompile', recompile: 'recompile',
  shaders: 'build', configure: 'build', build: 'build', install: 'build' };
const kOrder = ['game', 'tools', 'recompile', 'build'];

// send({step, state: 'run'|'done'|'fail', text}) and send({log}), send({progress: 0-1})
async function setup(launcherDir, game, gamePath, send) {
  const p = paths(launcherDir, game);
  if (!p) throw new Error('The build files (src folder) are missing next to the launcher.');
  const log = (line) => send({ log: line });
  const tools = findTools();
  if (!tools.bash || !tools.python) throw new Error('bash and python3 are needed: install them with your package manager.');

  // 1. Game files (as setup.js): a folder with default.xex copied, a package unpacked.
  send({ step: 'game', state: 'run', text: 'Copying your game files…' });
  try {
    if (gamePath) {
      fs.mkdirSync(p.data, { recursive: true });
      if (path.basename(gamePath).toLowerCase() === 'default.xex') {
        log(`Copying ${path.dirname(gamePath)}`);
        fs.cpSync(path.dirname(gamePath), p.data, { recursive: true });
      } else {
        log(`Unpacking ${gamePath}`);
        let n = 0;  // (a disc has thousands of files: every 50th is shown)
        const code = await run(tools.python, ['-I', path.join(p.src, 'tools', p.game.unpacker), gamePath, p.data], {},
          (line) => { if (game !== 'bfbc1' || ++n % 50 === 0) log(line); });
        if (code !== 0) throw new Error(`Unpacking the game failed (exit ${code}).`);
      }
    }
    if (!fs.existsSync(path.join(p.data, 'default.xex'))) throw new Error('No default.xex found in the game files.');
  } catch (e) {
    send({ step: 'game', state: 'fail', text: e.message });
    throw e;
  }
  send({ step: 'game', state: 'done' });

  // 2-4. build_linux.sh (again once after installing missing packages).
  for (let attempt = 0; ; ++attempt) {
    const lines = [];
    let current = null, lastError = '';
    const enter = (step) => {
      if (step === current || kOrder.indexOf(step) < kOrder.indexOf(current)) return;  // (forward only)
      for (const s of kOrder.slice(kOrder.indexOf(current) + 1, kOrder.indexOf(step))) send({ step: s, state: 'done' });
      if (current) send({ step: current, state: 'done' });
      current = step;
      send({ step, state: 'run', text: { tools: 'Checking and building the build tools…', recompile: 'Recompiling the game code…',
        build: 'Building the game (about 10-60 minutes)…' }[step] });
    };
    const code = await run('bash', [path.join(p.src, 'build_linux.sh'), '--game', p.data, '--out', p.out,
      ...p.linux.args, ...(game === 'bfbc1' ? ['--bc-tree', p.tree] : [])], { cwd: p.src },
      (line) => {
        lines.push(line);
        if (lines.length > 200) lines.shift();
        const m = /^progress (\w+) (\d+) (\d+)$/.exec(line);
        if (m) {
          if (kStepOf[m[1]]) enter(kStepOf[m[1]]);
          if (m[1] === 'build' && +m[3] > 1) send({ progress: +m[2] / +m[3] });
          return;
        }
        if (line.startsWith('error: ')) lastError = line.slice(7);
        log(line.length > 160 ? line.slice(0, 160) + '…' : line);
      });
    if (code === 0) {
      if (current) send({ step: current, state: 'done' });
      break;
    }
    const install = attempt === 0 && current === 'tools' && installCommand(lines);
    if (install) {
      log(`Installing the missing packages: ${install.join(' ')}`);
      log('(your system asks for your password)');
      const rc = await run('pkexec', install, {}, log).catch((e) => { throw new Error(`pkexec: ${e.message}`); });
      if (rc === 0) continue;
      const why = rc === 126 || rc === 127 ? 'not allowed (the password was not given)' : `exit ${rc}`;
      const e = new Error(`Installing the packages failed (${why}). Run this, then INSTALL again:\n  sudo ${install.join(' ')}`);
      send({ step: 'tools', state: 'fail', text: e.message });
      throw e;
    }
    const e = new Error(lastError || `build_linux.sh failed (exit ${code}); see ${path.join(p.out, 'logs')}`);
    send({ step: current || 'tools', state: 'fail', text: e.message });
    throw e;
  }

  // The game next to the launcher's folders, with the version it was built for.
  fs.mkdirSync(p.gameDir, { recursive: true });
  fs.cpSync(path.join(p.out, 'game'), p.gameDir, { recursive: true, force: true });
  if (module.exports.version) fs.writeFileSync(path.join(p.gameDir, '.built-version'), module.exports.version);
}

// Bad Company's PC SETTINGS menu patch (as setup.js, with python3).
function patchMenu(launcherDir, gameData, userDir) {
  const out = path.join(userDir, 'override', 'dist', 'xenon', 'menu', 'main', 'mainmenu-00.fbrb');
  if (fs.existsSync(out)) return Promise.resolve(null);
  const p = paths(launcherDir, 'bfbc1');
  const script = p && path.join(p.tree, 'tools', 'bc_menu_patch.py');
  if (!script || !fs.existsSync(script) || !findTools().python) return Promise.resolve('The PC SETTINGS menu patch needs python3 and bc_menu_patch.py.');
  return new Promise((resolve) => {
    const r = spawn('python3', ['-I', script, '--game', gameData, '--user', userDir], { stdio: 'ignore' });
    r.once('error', (e) => resolve(e.message));
    r.once('exit', (code) => resolve(code === 0 ? null : `bc_menu_patch.py failed (${code}).`));
  });
}

module.exports = { status, setup, patchMenu, findTools, kToolNames: { python: 'Python 3', bash: 'bash' },
  kGames: base.kGames };
