// First-run setup, run by the launcher: builds Battlefield 1943 for this PC
// from the player's own copy of the game. Nothing of the game ships with the
// launcher; this makes it here:
//   1. the game files: an Xbox 360 package (STFS) is unpacked, a folder with
//      default.xex is copied, into <root>/original/v106 (v1.06 only: the
//      recompiler's addresses are for that build);
//   2. the build tools, through winget where missing: LLVM (clang), CMake,
//      Ninja, Python, the Visual Studio C++ build tools, the Vulkan SDK;
//   3. XenonRecomp turns default.xex into C++ (src/generated/ppc);
//   4. CMake + Ninja build bf1943.exe, copied to <root>/game.
// Layout (release): <root>/Launcher/BF1943Launcher.exe, <root>/src (this repo).
const { spawn, execFileSync } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');

const kXexSha1 = 'b5aaa325ff358d66a116000931b7204da3f14534';  // default.xex v1.06
const kXexSha1V105 = '1c1f22606fa1eff15bf0409214279118dc805c28';

// The build tree: <root>/src in a release, the repo itself when run from source.
function paths(launcherDir) {
  for (const src of [path.join(path.dirname(launcherDir), 'src'), path.resolve(__dirname, '..')]) {
    if (fs.existsSync(path.join(src, 'tools', 'stfs_extract.py')) && fs.existsSync(path.join(src, 'game', 'CMakeLists.txt'))) {
      // From source (no release layout): installs under build/install.
      const root = path.basename(src) === 'src' ? path.dirname(src) : path.join(src, 'build', 'install');
      return { root, src, data: path.join(root, 'original', 'v106'), gameDir: path.join(root, 'game') };
    }
  }
  return null;
}

function sha1(file) {
  return crypto.createHash('sha1').update(fs.readFileSync(file)).digest('hex');
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
function status(launcherDir) {
  const p = paths(launcherDir);
  if (!p) return { ok: false, error: 'The build files (src folder) are missing next to the launcher.' };
  const tools = findTools();
  return {
    ok: true,
    gameFiles: fs.existsSync(path.join(p.data, 'default.xex')),
    built: fs.existsSync(path.join(p.gameDir, 'bf1943.exe')),
    tools: Object.fromEntries(Object.keys(kToolNames).map((k) => [k, !!tools[k]])),
  };
}

// send({step, state: 'run'|'done'|'fail', text}) and send({log}), send({progress: 0-1})
async function setup(launcherDir, gamePath, send) {
  const p = paths(launcherDir);
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
        // An Xbox 360 package: unpack it (needs Python).
        if (!tools.python) {
          log('Installing Python to unpack the package…');
          await run('winget', ['install', '-e', '--accept-package-agreements', '--accept-source-agreements', ...kWinget.python], {}, log);
          tools = findTools();
        }
        log(`Unpacking ${gamePath}`);
        await run(tools.python, ['-I', path.join(p.src, 'tools', 'stfs_extract.py'), gamePath, p.data], {}, log);
      }
    }
    if (!fs.existsSync(xex)) throw new Error('No default.xex found in the game files.');
    const h = sha1(xex);
    if (h === kXexSha1V105) throw new Error('This is version 1.05 of the game. Use the 1.06 package (the game updated on the console).');
    if (h !== kXexSha1) throw new Error('This default.xex is not Battlefield 1943 version 1.06 (SHA-1 ' + h + ').');
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
    const gen = path.join(p.src, 'generated');
    const cfgIn = path.join(gen, 'bf1943_config.toml');
    const cfg = fs.readFileSync(cfgIn, 'utf8').replace(/^file_path = ".*"$/m,
      `file_path = "${path.join(p.data, 'default.xex').replace(/\\/g, '/')}"`);
    const cfgOut = path.join(gen, 'bf1943_config.local.toml');
    fs.writeFileSync(cfgOut, cfg);
    const recomp = firstExisting([path.join(p.src, 'tools', 'bin', 'XenonRecomp.exe'),
      path.join(p.src, 'build', 'xenonrecomp-clang', 'XenonRecomp', 'XenonRecomp.exe')]);
    if (!recomp) throw new Error('XenonRecomp.exe is missing from the build files.');
    fs.mkdirSync(path.join(gen, 'ppc'), { recursive: true });
    let n = 0;
    // Paths relative to generated/: XenonRecomp finds the config's folder by
    // '/' only and crashes on an absolute Windows path.
    await run(recomp, [path.basename(cfgOut), '../thirdparty/XenonRecomp/XenonUtils/ppc_context.h'], { cwd: gen },
      (line) => { if (++n % 200 === 0) log(line); });
  });

  // 4. Build.
  await step('build', 'Building the game (about 20-60 minutes)…', async () => {
    const bat = path.join(p.src, 'build', 'launcher_build.bat');
    fs.mkdirSync(path.dirname(bat), { recursive: true });
    const q = (s) => `"${s}"`;
    fs.writeFileSync(bat, [
      '@echo off',
      `call ${q(tools.vs)} >nul 2>&1 || exit /b 1`,
      `set "VULKAN_SDK=${tools.vulkan}"`,
      `set "PATH=${path.dirname(tools.ninja)};${path.dirname(tools.cmake)};${path.dirname(tools.python)};%PATH%"`,
      `cd /d ${q(p.src)}`,
      `cmake -S game -B build\\game -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${tools.clang.replace(/\\/g, '/')}" "-DCMAKE_CXX_COMPILER=${tools.clang.replace(/\\/g, '/')}" || exit /b 1`,
      'cmake --build build\\game || exit /b 1',
    ].join('\r\n'));
    await run('cmd.exe', ['/d', '/c', bat], {}, (line) => {
      const m = /^\[(\d+)\/(\d+)\]/.exec(line);
      if (m) send({ progress: Number(m[1]) / Number(m[2]) });
      if (!m || Number(m[1]) % 25 === 0) log(line.length > 160 ? line.slice(0, 160) + '…' : line);
    });
    const built = path.join(p.src, 'build', 'game');
    fs.mkdirSync(p.gameDir, { recursive: true });
    fs.copyFileSync(path.join(built, 'bf1943.exe'), path.join(p.gameDir, 'bf1943.exe'));
    for (const f of ['pbr_shaders.txt']) {
      if (fs.existsSync(path.join(built, f))) fs.copyFileSync(path.join(built, f), path.join(p.gameDir, f));
    }
    if (fs.existsSync(path.join(built, 'fonts'))) fs.cpSync(path.join(built, 'fonts'), path.join(p.gameDir, 'fonts'), { recursive: true });
  });
}

module.exports = { status, setup, kToolNames };
