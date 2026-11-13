// Self-update from GitHub releases (no server of our own: GitHub's release API
// and its download links). The release's launcher (BattlefieldLauncher-<v>.exe,
// the single-exe build of tools/make_release.py) replaces this one:
//   1. check: the latest release's tag against this version;
//   2. download it beside this exe (.part), checked against the SHA-256 GitHub
//      keeps for each release file (or a SHA256SUMS.txt file in the release),
//      refused when neither is there or it differs;
//   3. start it with --updated-from <this exe>, and quit: the new one removes
//      the old exe, copies its build trees in (main.js prepareSources), and the
//      games' builds of the old version ask to be rebuilt (UPDATE).
const https = require('https');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const kRepo = 'papand7/Battlefield-Recompilation-Project';
const kAsset = /^BattlefieldLauncher-(\d+\.\d+\.\d+)\.exe$/;

function get(url, onResponse, redirects = 5) {
  return new Promise((resolve, reject) => {
    const req = https.get(url, { headers: { 'User-Agent': 'BattlefieldLauncher', Accept: '*/*' } }, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location && redirects > 0) {
        res.resume();
        resolve(get(new URL(res.headers.location, url).toString(), onResponse, redirects - 1));
        return;
      }
      if (res.statusCode !== 200) {
        res.resume();
        reject(new Error(`GitHub answered ${res.statusCode}`));
        return;
      }
      onResponse(res, resolve, reject);
    });
    req.setTimeout(30000, () => req.destroy(new Error('GitHub did not answer')));
    req.on('error', reject);
  });
}
const getText = (url) => get(url, (res, resolve, reject) => {
  let body = '';
  res.setEncoding('utf8');
  res.on('data', (c) => (body += c));
  res.on('end', () => resolve(body));
  res.on('error', reject);
});

function newer(a, b) {  // "1.2.3" > "1.2.0"
  const x = a.split('.').map(Number), y = b.split('.').map(Number);
  for (let i = 0; i < 3; i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) > (y[i] || 0);
  return false;
}

// The latest release, if it is newer than `version`: {version, name, url, size, sha256 | null, notes}.
async function check(version) {
  const release = JSON.parse(await getText(`https://api.github.com/repos/${kRepo}/releases/latest`));
  const asset = (release.assets || []).find((a) => kAsset.test(a.name));
  if (!asset) return null;
  const v = asset.name.match(kAsset)[1];
  if (!newer(v, version)) return null;
  let sha256 = typeof asset.digest === 'string' && asset.digest.startsWith('sha256:') ? asset.digest.slice(7) : null;
  if (!sha256) {  // a SHA256SUMS.txt in the release: "<hex>  <file name>" lines
    const sums = (release.assets || []).find((a) => /^SHA256SUMS(\.txt)?$/i.test(a.name));
    if (sums) {
      const line = (await getText(sums.browser_download_url)).split(/\r?\n/)
        .find((l) => l.trim().endsWith(asset.name));
      if (line) sha256 = line.trim().split(/\s+/)[0];
    }
  }
  return { version: v, name: asset.name, url: asset.browser_download_url, size: asset.size,
           sha256: sha256 && /^[0-9a-f]{64}$/i.test(sha256) ? sha256.toLowerCase() : null,
           notes: (release.body || '').slice(0, 2000) };
}

// Downloads `update` into `dir`, checks it, and returns the new exe's path.
// progress(0..1) on the way.
async function download(update, dir, progress) {
  if (!update.sha256) throw new Error('The release has no checksum for the launcher: not installed.');
  const final = path.join(dir, update.name), part = final + '.part';
  const hash = crypto.createHash('sha256');
  await get(update.url, (res, resolve, reject) => {
    const total = Number(res.headers['content-length']) || update.size || 0;
    let done = 0;
    const out = fs.createWriteStream(part);
    res.on('data', (c) => {
      hash.update(c);
      done += c.length;
      if (total) progress(done / total);
    });
    res.on('error', reject);
    out.on('error', reject);
    out.on('finish', resolve);
    res.pipe(out);
  });
  const got = hash.digest('hex');
  if (got !== update.sha256) {
    fs.rmSync(part, { force: true });
    throw new Error('The download does not match its checksum: not installed.');
  }
  fs.rmSync(final, { force: true });
  fs.renameSync(part, final);
  return final;
}

// In the new launcher: the old one (named like a release launcher, in the same
// folder) removed once it has quit.
function removeOld(argv, ownExe) {
  const i = argv.indexOf('--updated-from');
  if (i < 0 || !argv[i + 1]) return;
  const old = path.resolve(argv[i + 1]);
  if (!ownExe || path.dirname(old) !== path.dirname(path.resolve(ownExe)) || !kAsset.test(path.basename(old)) ||
      path.resolve(ownExe) === old) return;
  let tries = 0;
  const attempt = () => {
    try { fs.rmSync(old, { force: true }); } catch { if (++tries < 20) setTimeout(attempt, 500); }
  };
  setTimeout(attempt, 1000);
}

module.exports = { check, download, removeOld, newer };
