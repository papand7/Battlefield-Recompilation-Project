// First-run setup, run by the launcher: builds a game for this PC from the
// player's own copy. Nothing of the games ships with the launcher; this makes
// it here, per game (kGames):
//   1. the game files: Battlefield 1943's Xbox 360 package (STFS) or Bad
//      Company's disc image (.iso) is unpacked, a folder with default.xex is
//      copied, into <root>/original/<game> (one exact build each: the
//      recompiler's addresses are for it);
//   2. the build tools, through winget where missing: LLVM (clang), CMake,
//      Ninja, Python, the Visual Studio C++ build tools, the Vulkan SDK;
//   3. XenonRecomp turns default.xex into C++ (<tree>/generated/ppc);
//   4. CMake + Ninja build the exe, copied to its folder under <root>.
// Layout (release): <root>/Launcher/BF1943Launcher.exe, <root>/src (bf1943-recomp,
// also the code both games share), <root>/src-bfbc1 (bfbc1-recomp).
const { spawn, execFileSync } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');

const kGames = {
  bf1943: {
    title: 'Battlefield 1943',
    sha1: 'b5aaa325ff358d66a116000931b7204da3f14534',  // default.xex v1.06
    wrongSha1: { '1c1f22606fa1eff15bf0409214279118dc805c28':
      'This is version 1.05 of the game. Use the 1.06 package (the game updated on the console).' },
    expected: 'Battlefield 1943 version 1.06',
    unpacker: 'stfs_extract.py',
    data: 'original/v106', gameDir: 'game', exe: 'bf1943.exe', config: 'bf1943_config.toml',
  },
  bfbc1: {
    title: 'Battlefield: Bad Company',
    sha1: 'c1ad8ac5c18b1b0b23a4c3476721462852af1a93',  // default.xex of the US disc
    wrongSha1: {},
    expected: 'Battlefield: Bad Company (USA disc)',
    unpacker: 'xiso_extract.py',
    data: 'original/bfbc1', gameDir: 'game-bfbc1', exe: 'bfbc1.exe', config: 'bfbc1_config.toml',
  },
};

// The build trees: <root>/src and <root>/src-bfbc1 in a release, the repo's
// bf1943-recomp and bfbc1-recomp when run from source (installing under
// bf1943-recomp/build/install). tree = the game's own tree, src = the shared one.
function paths(launcherDir, game = 'bf1943') {
  const g = kGames[game];
  if (!g) return null;
  for (const src of [path.join(path.dirname(launcherDir), 'src'), path.resolve(__dirname, '..')]) {
    if (fs.existsSync(path.join(src, 'tools', 'stfs_extract.py')) && fs.existsSync(path.join(src, 'game', 'CMakeLists.txt'))) {
      const release = path.basename(src) === 'src';
      const root = release ? path.dirname(src) : path.join(src, 'build', 'install');
      const tree = game === 'bf1943' ? src
        : release ? path.join(root, 'src-bfbc1') : path.resolve(src, '..', 'bfbc1-recomp');
      if (!fs.existsSync(path.join(tree, 'game', 'CMakeLists.txt'))) return null;
      return { root, src, tree, game: g, data: path.join(root, g.data), gameDir: path.join(root, g.gameDir) };
    }
  }
  return null;
}

function sha1(file) {
  return crypto.createHash('sha1').update(fs.readFileSync(file)).digest('hex');
}

// ---- the player's game next to the launcher ---------------------------------------
function readAt(fd, pos, len) {
  const b = Buffer.alloc(len);
  return b.subarray(0, fs.readSync(fd, b, 0, len, pos));
}
// A disc image (XDVDFS, as tools/xiso_extract.py): the SHA-1 of default.xex in its
// root directory, or null. Tells Bad Company's disc from any other game's.
function isoXexSha1(file) {
  const fd = fs.openSync(file, 'r');
  try {
    for (const base of [0, 0xFD90000, 0x2080000, 0x18300000]) {
      const vd = readAt(fd, base + 32 * 2048, 28);
      if (vd.length < 28 || vd.toString('latin1', 0, 20) !== 'MICROSOFT*XBOX*MEDIA') continue;
      const dir = readAt(fd, base + vd.readUInt32LE(20) * 2048, vd.readUInt32LE(24));
      const stack = [0];
      const seen = new Set();
      while (stack.length) {
        const o = stack.pop() * 4;
        if (seen.has(o) || o + 14 > dir.length || dir.readUInt16LE(o) === 0xFFFF) continue;
        seen.add(o);
        const name = dir.toString('latin1', o + 14, o + 14 + dir[o + 13]);
        if (name.toLowerCase() === 'default.xex' && !(dir[o + 12] & 0x10)) {
          const xex = readAt(fd, base + dir.readUInt32LE(o + 4) * 2048, dir.readUInt32LE(o + 8));
          return crypto.createHash('sha1').update(xex).digest('hex');
        }
        if (dir.readUInt16LE(o)) stack.push(dir.readUInt16LE(o));
        if (dir.readUInt16LE(o + 2)) stack.push(dir.readUInt16LE(o + 2));
      }
      return null;
    }
    return null;
  } finally {
    fs.closeSync(fd);
  }
}
// An Xbox 360 package (STFS): its title ID and version (0x106 = 1.06), or null.
function stfsInfo(file) {
  const fd = fs.openSync(file, 'r');
  try {
    const h = readAt(fd, 0, 0x364);
    if (h.length < 0x364 || !['CON ', 'LIVE', 'PIRS'].includes(h.toString('latin1', 0, 4))) return null;
    return { title: h.readUInt32BE(0x360), version: h.readUInt32BE(0x358) };
  } finally {
    fs.closeSync(fd);
  }
}
const kBf1943Title = 0x5841097E;
const foundCache = new Map();  // path|size|mtime -> what it is
function identify(file, st) {
  const key = `${file}|${st.size}|${st.mtimeMs}`;
  if (!foundCache.has(key)) {
    let what = null;
    try {
      if (/\.iso$/i.test(file)) {
        if (isoXexSha1(file) === kGames.bfbc1.sha1) what = { game: 'bfbc1' };
      } else if (path.basename(file).toLowerCase() === 'default.xex') {
        const h = sha1(file);
        what = h === kGames.bfbc1.sha1 ? { game: 'bfbc1' } : h === kGames.bf1943.sha1 ? { game: 'bf1943' }
          : kGames.bf1943.wrongSha1[h] ? { game: 'bf1943', wrong: kGames.bf1943.wrongSha1[h] } : null;
      } else if (st.size > 1 << 20) {
        const info = stfsInfo(file);
        if (info && info.title === kBf1943Title)
          what = info.version === 0x106 ? { game: 'bf1943' } : { game: 'bf1943', wrong: kGames.bf1943.wrongSha1[
            '1c1f22606fa1eff15bf0409214279118dc805c28'] };
      }
    } catch { /* unreadable: not it */ }
    foundCache.set(key, what);
  }
  return foundCache.get(key);
}
// The game's files in dir (the launcher's folder): a disc image or package
// there, or default.xex in a folder there. { path } or { wrong: why } or null.
function findGameFiles(dir, game) {
  if (!dir) return null;
  let names = [];
  try { names = fs.readdirSync(dir); } catch { return null; }
  let wrong = null;
  for (const n of names) {
    const f = path.join(dir, n);
    let st;
    try { st = fs.statSync(f); } catch { continue; }
    const candidates = st.isDirectory() ? [path.join(f, 'default.xex')] : [f];
    for (const c of candidates) {
      let cs = st;
      if (c !== f) { try { cs = fs.statSync(c); } catch { continue; } }
      const what = identify(c, cs);
      if (!what || what.game !== game) continue;
      if (!what.wrong) return { path: c };
      wrong = wrong || { wrong: `${path.basename(c)}: ${what.wrong}` };
    }
  }
  return wrong;
}

// ---- tools ------------------------------------------------------------------------
const PF = process.env.ProgramFiles || 'C:\\Program Files';
const PF86 = process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)';
const LOCAL = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
const firstExisting = (list) => list.find((p) => p && fs.existsSync(p)) || null;
const globDir = (dir, re) => {
  try { return fs.readdirSync(dir).filter((n) => re.test(n)).sort().reverse().map((n) => path.join(dir, n)); }
  catch { return []; }
};

function findTools() {
  const t = {};
  t.clang = firstExisting([path.join(PF, 'LLVM', 'bin', 'clang-cl.exe')]);
  t.cmake = firstExisting([path.join(PF, 'CMake', 'bin', 'cmake.exe')]);
  t.ninja = firstExisting([
    path.join(LOCAL, 'Microsoft', 'WinGet', 'Links', 'ninja.exe'),
    ...globDir(path.join(LOCAL, 'Microsoft', 'WinGet', 'Packages'), /^Ninja-build\.Ninja/).map((d) => path.join(d, 'ninja.exe')),
    path.join(PF, 'CMake', 'bin', 'ninja.exe'),
  ]);
  t.python = firstExisting([
    ...globDir(path.join(LOCAL, 'Programs', 'Python'), /^Python3\d+$/).map((d) => path.join(d, 'python.exe')),
    ...globDir(PF, /^Python3\d+$/).map((d) => path.join(d, 'python.exe')),
    ...globDir('C:/', /^Python3\d+$/).map((d) => path.join(d, 'python.exe')),
    // On PATH (not the Microsoft Store placeholder in WindowsApps).
    ...(process.env.PATH || '').split(';').filter((d) => d && !/WindowsApps/i.test(d))
      .map((d) => path.join(d, 'python.exe')),
  ]);
  const vswhere = path.join(PF86, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
  t.vs = null;
  if (fs.existsSync(vswhere)) {
    try {
      const out = execFileSync(vswhere, ['-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' }).trim();
      const vcvars = out && path.join(out.split(/\r?\n/)[0], 'VC', 'Auxiliary', 'Build', 'vcvars64.bat');
      if (vcvars && fs.existsSync(vcvars)) t.vs = vcvars;
    } catch { /* not installed */ }
  }
  t.vulkan = firstExisting([process.env.VULKAN_SDK, ...globDir('C:\\VulkanSDK', /^\d/)]);
  if (t.vulkan && !fs.existsSync(path.join(t.vulkan, 'Include', 'vulkan', 'vulkan.h'))) t.vulkan = null;
  return t;
}

const kWinget = {
  clang: ['--id', 'LLVM.LLVM'],
  cmake: ['--id', 'Kitware.CMake'],
  ninja: ['--id', 'Ninja-build.Ninja'],
  python: ['--id', 'Python.Python.3.12'],
  vs: ['--id', 'Microsoft.VisualStudio.2022.BuildTools', '--override',
       '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'],
  vulkan: ['--id', 'KhronosGroup.VulkanSDK'],
};
const kToolNames = {
  clang: 'LLVM (clang)', cmake: 'CMake', ninja: 'Ninja', python: 'Python',
  vs: 'Visual Studio C++ build tools', vulkan: 'Vulkan SDK',
};

// ---- running things -------------------------------------------------------------------
function run(cmd, args, opts, onLine) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { windowsHide: true, ...opts });
    let tail = [];
    const feed = (buf) => {
      for (const line of buf.toString().split(/\r?\n/)) {
        if (!line.trim()) continue;
        tail.push(line);
        if (tail.length > 30) tail.shift();
        onLine(line);
      }
    };
    p.stdout && p.stdout.on('data', feed);
    p.stderr && p.stderr.on('data', feed);
    p.on('error', reject);
    p.on('exit', (code) => (code === 0 ? resolve() : reject(new Error(`${path.basename(cmd)} failed (exit ${code}):\n` + tail.slice(-8).join('\n')))));
  });
}

// ---- the steps ----------------------------------------------------------------------
function status(launcherDir, game, searchDir) {
  const p = paths(launcherDir, game);
  if (!p) return { ok: false, error: 'The build files (src folder) are missing next to the launcher.' };
  const tools = findTools();
  return {
    ok: true,
    gameFiles: fs.existsSync(path.join(p.data, 'default.xex')),
    built: fs.existsSync(path.join(p.gameDir, p.game.exe)),
    found: findGameFiles(searchDir, game),
    tools: Object.fromEntries(Object.keys(kToolNames).map((k) => [k, !!tools[k]])),
  };
}

// send({step, state: 'run'|'done'|'fail', text}) and send({log}), send({progress: 0-1})
async function setup(launcherDir, game, gamePath, send) {
  const p = paths(launcherDir, game);
  if (!p) throw new Error('The build files (src folder) are missing next to the launcher.');
  const log = (line) => send({ log: line });
  const step = async (name, text, fn) => {
    send({ step: name, state: 'run', text });
    try {
      await fn();
      send({ step: name, state: 'done' });
    } catch (e) {
      send({ step: name, state: 'fail', text: e.message });
      throw e;
    }
  };
  let tools = findTools();

  // 1. Game files.
  await step('game', 'Copying your game files…', async () => {
    const xex = path.join(p.data, 'default.xex');
    if (gamePath) {
      fs.mkdirSync(p.data, { recursive: true });
      if (path.basename(gamePath).toLowerCase() === 'default.xex') {
        log(`Copying ${path.dirname(gamePath)}`);
        fs.cpSync(path.dirname(gamePath), p.data, { recursive: true });
      } else {
        // An Xbox 360 package (BF1943) or disc image (Bad Company): unpack it (needs Python).
        if (!tools.python) {
          log('Installing Python to unpack the game…');
          await run('winget', ['install', '-e', '--accept-package-agreements', '--accept-source-agreements', ...kWinget.python], {}, log);
          tools = findTools();
        }
        log(`Unpacking ${gamePath}`);
        let n = 0;  // a disc has thousands of files: every 50th is shown
        await run(tools.python, ['-I', path.join(p.src, 'tools', p.game.unpacker), gamePath, p.data], {},
          (line) => { if (game !== 'bfbc1' || ++n % 50 === 0) log(line); });
      }
    }
    if (!fs.existsSync(xex)) throw new Error('No default.xex found in the game files.');
    const h = sha1(xex);
    if (p.game.wrongSha1[h]) throw new Error(p.game.wrongSha1[h]);
    if (h !== p.game.sha1) throw new Error(`This default.xex is not ${p.game.expected} (SHA-1 ${h}).`);
  });

  // 2. Build tools.
  await step('tools', 'Installing the build tools (this can take a while)…', async () => {
    for (const k of Object.keys(kToolNames)) {
      if (tools[k]) continue;
      log(`Installing ${kToolNames[k]}… (Windows may ask for permission)`);
      await run('winget', ['install', '-e', '--accept-package-agreements', '--accept-source-agreements', ...kWinget[k]], {}, log)
        .catch((e) => { throw new Error(`Could not install ${kToolNames[k]}: ${e.message}`); });
      tools = findTools();
    }
    const missing = Object.keys(kToolNames).filter((k) => !tools[k]);
    if (missing.length) throw new Error('Still missing: ' + missing.map((k) => kToolNames[k]).join(', ') + '. Restart the launcher and try again.');
  });

  // 3. Recompile: the config with this PC's path to default.xex.
  await step('recompile', 'Recompiling the game code…', async () => {
    const gen = path.join(p.tree, 'generated');
    const cfgIn = path.join(gen, p.game.config);
    const cfg = fs.readFileSync(cfgIn, 'utf8').replace(/^file_path = ".*"$/m,
      `file_path = "${path.join(p.data, 'default.xex').replace(/\\/g, '/')}"`);
    const cfgOut = path.join(gen, p.game.config.replace('.toml', '.local.toml'));
    fs.writeFileSync(cfgOut, cfg);
    const recomp = firstExisting([path.join(p.src, 'tools', 'bin', 'XenonRecomp.exe'),
      path.join(p.src, 'build', 'xenonrecomp-clang', 'XenonRecomp', 'XenonRecomp.exe')]);
    if (!recomp) throw new Error('XenonRecomp.exe is missing from the build files.');
    fs.mkdirSync(path.join(gen, 'ppc'), { recursive: true });
    let n = 0;
    // Paths relative to generated/: XenonRecomp finds the config's folder by
    // '/' only and crashes on an absolute Windows path.
    const header = path.relative(gen, path.join(p.src, 'thirdparty', 'XenonRecomp', 'XenonUtils', 'ppc_context.h'));
    await run(recomp, [path.basename(cfgOut), header.split(path.sep).join('/')], { cwd: gen },
      (line) => { if (++n % 200 === 0) log(line); });
  });

  // 4. Build.
  await step('build', 'Building the game (about 20-60 minutes)…', async () => {
    const bat = path.join(p.tree, 'build', 'launcher_build.bat');
    fs.mkdirSync(path.dirname(bat), { recursive: true });
    const q = (s) => `"${s}"`;
    fs.writeFileSync(bat, [
      '@echo off',
      `call ${q(tools.vs)} >nul 2>&1 || exit /b 1`,
      `set "VULKAN_SDK=${tools.vulkan}"`,
      `set "PATH=${path.dirname(tools.ninja)};${path.dirname(tools.cmake)};${path.dirname(tools.python)};%PATH%"`,
      `cd /d ${q(p.tree)}`,
      `cmake -S game -B build\\game -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${tools.clang.replace(/\\/g, '/')}" "-DCMAKE_CXX_COMPILER=${tools.clang.replace(/\\/g, '/')}"` +
        // Bad Company's tree builds with the shared code from src.
        (p.tree !== p.src ? ` "-DROOT=${p.src.replace(/\\/g, '/')}"` : '') + ' || exit /b 1',
      'cmake --build build\\game || exit /b 1',
    ].join('\r\n'));
    await run('cmd.exe', ['/d', '/c', bat], {}, (line) => {
      const m = /^\[(\d+)\/(\d+)\]/.exec(line);
      if (m) send({ progress: Number(m[1]) / Number(m[2]) });
      if (!m || Number(m[1]) % 25 === 0) log(line.length > 160 ? line.slice(0, 160) + '…' : line);
    });
    const built = path.join(p.tree, 'build', 'game');
    fs.mkdirSync(p.gameDir, { recursive: true });
    fs.copyFileSync(path.join(built, p.game.exe), path.join(p.gameDir, p.game.exe));
    for (const f of ['pbr_shaders.txt']) {
      if (fs.existsSync(path.join(built, f))) fs.copyFileSync(path.join(built, f), path.join(p.gameDir, f));
    }
    if (fs.existsSync(path.join(built, 'fonts'))) fs.cpSync(path.join(built, 'fonts'), path.join(p.gameDir, 'fonts'), { recursive: true });
  });
}

module.exports = { status, setup, findTools, kToolNames, kGames };
