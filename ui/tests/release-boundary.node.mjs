import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

const script = new URL('../scripts/publish-release.sh', import.meta.url).pathname;

function git(cwd, ...args) {
  return execFileSync('git', args, { cwd, encoding: 'utf8' }).trim();
}

function fixture(t, annotated = false) {
  const root = mkdtempSync(join(tmpdir(), 'isosui-release-boundary-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const remote = join(root, 'remote.git');
  const checkout = join(root, 'checkout');
  const bin = join(root, 'bin');
  git(root, 'init', '--bare', remote);
  git(root, 'init', '-b', 'master', checkout);
  git(checkout, 'config', 'user.email', 'release-test@example.invalid');
  git(checkout, 'config', 'user.name', 'Release test');
  git(checkout, 'remote', 'add', 'origin', remote);
  writeFileSync(join(checkout, 'source.txt'), 'prepared source\n');
  git(checkout, 'add', '.');
  git(checkout, 'commit', '-m', 'prepare');
  git(checkout, 'tag', ...(annotated ? ['-a', '-m', 'release fixture'] : []), 'v0.1.0');
  git(checkout, 'push', 'origin', 'master', 'v0.1.0');
  mkdirSync(join(checkout, 'release'));
  writeFileSync(join(checkout, 'release', 'SHA256SUMS'), 'fixture');
  mkdirSync(bin);
  const calls = join(root, 'gh-calls');
  writeFileSync(join(bin, 'gh'), '#!/usr/bin/env bash\nprintf "%s\\n" "$*" >> "$MOCK_GH_CALLS"\nif [[ "$1 $2" == "release view" ]]; then [[ "${MOCK_RELEASE_EXISTS:-}" == yes ]]; exit; fi\n');
  execFileSync('chmod', ['+x', join(bin, 'gh')]);
  const run = (extraEnv = {}) => spawnSync('bash', [script], {
    cwd: checkout,
    encoding: 'utf8',
    env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, ISOSUI_TAG: 'v0.1.0', MOCK_GH_CALLS: calls, ...extraEnv },
  });
  return { checkout, calls, run };
}

test('publishes only after checking the exact remote master and absent release', t => {
  const { calls, run } = fixture(t);
  const result = run();
  assert.equal(result.status, 0, result.stderr);
  assert.match(readFileSync(calls, 'utf8'), /release view v0\.1\.0\nrelease create v0\.1\.0 release\/SHA256SUMS --verify-tag --generate-notes/);
});

test('accepts an annotated tag peeled to the exact remote master', t => {
  const { calls, run } = fixture(t, true);
  const result = run();
  assert.equal(result.status, 0, result.stderr);
  assert.match(readFileSync(calls, 'utf8'), /release create v0\.1\.0/);
});

test('rejects a remote master move before release creation', t => {
  const { checkout, calls, run } = fixture(t);
  writeFileSync(join(checkout, 'source.txt'), 'new source\n');
  git(checkout, 'add', '.');
  git(checkout, 'commit', '-m', 'advance master');
  git(checkout, 'push', 'origin', 'master');
  git(checkout, 'checkout', '--detach', 'v0.1.0');
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /Remote master moved/);
  assert.throws(() => readFileSync(calls));
});

test('rejects an existing release before release creation', t => {
  const { calls, run } = fixture(t);
  const result = run({ MOCK_RELEASE_EXISTS: 'yes' });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /already exists/);
  assert.equal(readFileSync(calls, 'utf8'), 'release view v0.1.0\n');
});

test('rejects a retargeted remote tag even when the local tag and master are unchanged', t => {
  const { checkout, calls, run } = fixture(t);
  const source = git(checkout, 'rev-parse', 'HEAD');
  writeFileSync(join(checkout, 'source.txt'), 'different tag target\n');
  git(checkout, 'add', '.');
  git(checkout, 'commit', '-m', 'retarget tag');
  git(checkout, 'tag', '-f', 'v0.1.0');
  git(checkout, 'push', '--force', 'origin', 'v0.1.0');
  git(checkout, 'reset', '--hard', source);
  git(checkout, 'tag', '-f', 'v0.1.0', source);
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /Remote tag v0\.1\.0 moved/);
  assert.throws(() => readFileSync(calls));
});
