'use strict';
// Locates the driver payload: an override folder from settings, a bundled
// resources folder, or this checkout's driver/ folder.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

// Exact macOS builds a package may install on. macos_builds lists every audited
// build; macos_build is the single build older manifests and CLIs understand.
function requiredBuilds(requires) {
  if (!requires) return [];
  const builds = Array.isArray(requires.macos_builds) ? requires.macos_builds : requires.macos_build ? [requires.macos_build] : [];
  if (!builds.every((b) => typeof b === 'string' && /^[0-9]{2}[A-Z][0-9]{1,5}[a-z]?$/.test(b))) throw new Error('Invalid macOS build requirement');
  return builds;
}

function readManifest(dir) {
  try {
    const m = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
    if (!/^[A-Za-z0-9_.-]+\.tar\.gz$/.test(m.archive || '') || !/^[0-9a-fA-F-]{36}$/.test(m.uuid || '')) throw new Error('Invalid package identity');
    const archive = path.join(dir, m.archive);
    if (!m.archive || !fs.existsSync(archive)) return { available: false, dir, error: 'manifest.json found but the archive is missing' };
    return {
      available: true, dir, archive, manifest: m, version: m.version || null, uuid: m.uuid || null,
      requiresMacOSMajor: m.requires && m.requires.macos_major ? Number(m.requires.macos_major) : null,
      requiredBuilds: requiredBuilds(m.requires),
      tools: m.tools || {}, sparkTools: m.spark_tools || {}, built: m.built || null
    };
  } catch (e) { return { available: false, dir, error: fs.existsSync(dir) ? 'no manifest.json in this folder' : 'folder not found' }; }
}

function locate({ override, resourcesPath, appPath }) {
  const candidates = [];
  if (override) candidates.push({ dir: override, source: 'chosen folder' });
  if (resourcesPath) candidates.push({ dir: path.join(resourcesPath, 'driver'), source: 'bundled' });
  if (appPath) candidates.push({ dir: path.join(appPath, 'driver'), source: 'this checkout' });
  let firstError = null;
  for (const c of candidates) {
    const m = readManifest(c.dir);
    if (m.available) return { ...m, source: c.source };
    if (!firstError && override && c.dir === override) firstError = m.error;
  }
  return { available: false, error: firstError || 'no driver package found', source: null };
}

function sha256(file) {
  return new Promise((resolve) => {
    const h = crypto.createHash('sha256');
    fs.createReadStream(file).on('data', (d) => h.update(d)).on('end', () => resolve(h.digest('hex'))).on('error', () => resolve(null));
  });
}

module.exports = { locate, readManifest, requiredBuilds, sha256 };
