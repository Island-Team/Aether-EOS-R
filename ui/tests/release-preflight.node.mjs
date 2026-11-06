import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, readFileSync, rmSync, symlinkSync, unlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { verifyReleaseAssets } from '../scripts/verify-release-assets.mjs';

const root = mkdtempSync(join(tmpdir(), 'isosui-release-test-'));
const tag = 'v0.1.0';
const sourceCommit = 'a'.repeat(40);
const releaseDir = join(root, 'release');
mkdirSync(releaseDir);

function makeArchive(target, commit = sourceCommit) {
  const source = join(root, target);
  mkdirSync(source, { recursive: true });
  writeFileSync(join(source, 'index.html'), '<html></html>');
  writeFileSync(join(source, 'manifest.json'), JSON.stringify({
    schema_version: 1, api_schema_version: 1, version: '0.1.0',
    source_commit: commit, build_target: target,
  }));
  const name = `isosui-${target}-${tag}.tar.gz`;
  execFileSync('tar', ['-C', source, '-czf', join(releaseDir, name), '.']);
  return name;
}

function checksums(names) {
  writeFileSync(join(releaseDir, 'SHA256SUMS'), names.map(name => {
    const hash = createHash('sha256').update(readFileSync(join(releaseDir, name))).digest('hex');
    return `${hash}  ${name}\n`;
  }).join(''));
}

const names = ['local', 'hosted'].map(target => makeArchive(target));
checksums(names);

test('accepts two exact archives and manifests', () => {
  assert.deepEqual(verifyReleaseAssets({ tag, sourceCommit, version: '0.1.0', releaseDir }), names);
});

test('rejects digest tampering', () => {
  const archive = join(releaseDir, names[0]);
  const bytes = readFileSync(archive);
  writeFileSync(archive, Buffer.concat([bytes, Buffer.from('tamper')]));
  assert.throws(() => verifyReleaseAssets({ tag, sourceCommit, version: '0.1.0', releaseDir }), /checksum mismatch/);
  writeFileSync(archive, bytes);
});

test('rejects wrong source identity inside archive', () => {
  makeArchive('hosted', 'b'.repeat(40));
  checksums(names);
  assert.throws(() => verifyReleaseAssets({ tag, sourceCommit, version: '0.1.0', releaseDir }), /manifest identity mismatch/);
  makeArchive('hosted');
  checksums(names);
});

test('rejects symlink member', () => {
  const link = join(root, 'hosted', 'outside');
  symlinkSync('/etc/passwd', link);
  makeArchive('hosted');
  checksums(names);
  assert.throws(() => verifyReleaseAssets({ tag, sourceCommit, version: '0.1.0', releaseDir }), /link or special file/);
  unlinkSync(link);
  makeArchive('hosted');
  checksums(names);
});

test('rejects incomplete checksum list and duplicate entries', () => {
  checksums([names[0]]);
  assert.throws(() => verifyReleaseAssets({ tag, sourceCommit, version: '0.1.0', releaseDir }), /exactly two/);
  checksums([names[0], names[0]]);
  assert.throws(() => verifyReleaseAssets({ tag, sourceCommit, version: '0.1.0', releaseDir }), /invalid SHA256SUMS/);
  checksums(names);
});

test('rejects wrong tag, mode and package version', () => {
  const script = new URL('../scripts/verify-release-inputs.mjs', import.meta.url).pathname;
  for (const args of [['v0.2.0', 'manual'], [tag, 'other']]) {
    assert.notEqual(spawnSync(process.execPath, [script, ...args]).status, 0);
  }
  assert.equal(spawnSync(process.execPath, [script, tag, 'manual']).status, 0);
  assert.notEqual(spawnSync(process.execPath, [script, tag, 'dispatch'], { env: { ...process.env, DISPATCH_TOKEN: '' } }).status, 0);
  assert.equal(spawnSync(process.execPath, [script, tag, 'dispatch'], { env: { ...process.env, DISPATCH_TOKEN: 'test-token' } }).status, 0);
});

test('cleanup', () => rmSync(root, { recursive: true, force: true }));
