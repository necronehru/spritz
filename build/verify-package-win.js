'use strict';

const fs = require('fs');
const path = require('path');
const { readAsar, missingDependencies } = require('./asar-deps');

const APP = path.resolve('dist/win-unpacked');
const RES = path.join(APP, 'resources');
const MPV = path.join(RES, 'native', 'mpv-win');
const failures = [];

function checkFile(file, label = file) {
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
    failures.push(`Missing: ${label}`);
    return false;
  }
  console.log(`PASS ${label}`);
  return true;
}

function checkX64(file) {
  if (!checkFile(file)) return;
  const b = fs.readFileSync(file);
  if (b.length < 64) {
    failures.push(`Invalid PE file: ${file}`);
    return;
  }
  const offset = b.readUInt32LE(0x3c);
  const valid = offset + 6 <= b.length &&
    b.toString('ascii', 0, 2) === 'MZ' &&
    b.toString('ascii', offset, offset + 4) === 'PE\u0000\u0000' &&
    b.readUInt16LE(offset + 4) === 0x8664;
  if (valid) console.log(`PASS x64 PE ${path.basename(file)}`);
  else failures.push(`Not a valid Windows x64 PE binary: ${file}`);
}

console.log('Checking Windows x64 package:', APP);

checkFile(path.join(APP, 'Spiritz.exe'), 'Spiritz.exe');

for (const name of [
  'ffmpeg.exe',
  'ffprobe.exe',
  'yt-dlp.exe'
]) {
  checkX64(path.join(RES, 'bin', name));
}

for (const name of [
  'mpv_render.node',
  'libmpv-2.dll',
  'libgcc_s_seh-1.dll',
  'libstdc++-6.dll',
  'libwinpthread-1.dll',
  'vulkan-1.dll'
]) {
  checkX64(path.join(MPV, name));
}

const backup = path.join(MPV, 'mpv_render.node.backup-20261031');
if (fs.existsSync(backup)) failures.push('Backup addon was incorrectly packaged');
else console.log('PASS backup addon excluded');

const asarPath = path.join(RES, 'app.asar');
if (checkFile(asarPath, 'resources/app.asar')) {
  try {
    const missing = missingDependencies(readAsar(asarPath));
    if (missing.length) {
      for (const item of missing) {
        failures.push(`Missing ASAR dependency "${item.name}" required by ${item.neededBy}`);
      }
    } else {
      console.log('PASS ASAR dependencies');
    }
  } catch (error) {
    failures.push(`ASAR verification failed: ${error.message}`);
  }
}

if (failures.length) {
  console.error('\nWINDOWS PACKAGE VERIFICATION FAILED');
  for (const failure of failures) console.error('FAIL', failure);
  process.exitCode = 1;
} else {
  console.log('\nWINDOWS PACKAGE VERIFICATION PASSED');
}
