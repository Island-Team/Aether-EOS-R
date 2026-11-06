import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

export function verifyReleaseAssets({ tag, sourceCommit, releaseDir, version }) {
  if (tag !== `v${version}` || !/^[a-f0-9]{40}$/.test(sourceCommit)) throw new Error('invalid release identity');
  const expectedNames = ['local', 'hosted'].map(target => `isosui-${target}-${tag}.tar.gz`);
  const lines = readFileSync(join(releaseDir, 'SHA256SUMS'), 'utf8').trimEnd().split('\n');
  if (lines.length !== expectedNames.length) throw new Error('SHA256SUMS must list exactly two archives');
  const seen = new Set();
  for (const line of lines) {
    const match = /^([a-f0-9]{64})  (isosui-(local|hosted)-v[0-9A-Za-z.-]+\.tar\.gz)$/.exec(line);
    if (!match || !expectedNames.includes(match[2]) || seen.has(match[2])) throw new Error('invalid SHA256SUMS entry');
    seen.add(match[2]);
    const archive = join(releaseDir, match[2]);
    const digest = createHash('sha256').update(readFileSync(archive)).digest('hex');
    if (digest !== match[1]) throw new Error(`${match[2]} checksum mismatch`);
    const members = execFileSync('tar', ['-tzf', archive], { encoding: 'utf8' }).trimEnd().split('\n');
    if (!members.includes('./manifest.json') || !members.includes('./index.html')) throw new Error(`${match[2]} missing entry`);
    if (members.some(name => !name.startsWith('./') || name.split('/').includes('..'))) throw new Error(`${match[2]} unsafe entry`);
    if (new Set(members).size !== members.length) throw new Error(`${match[2]} duplicate entry`);
    const listings = execFileSync('tar', ['-tvzf', archive], { encoding: 'utf8' }).trimEnd().split('\n');
    if (listings.length !== members.length || listings.some(entry => !/^[-d]/.test(entry))) throw new Error(`${match[2]} contains a link or special file`);
    const manifest = JSON.parse(execFileSync('tar', ['-xOzf', archive, './manifest.json'], { encoding: 'utf8' }));
    if (manifest.schema_version !== 1 || manifest.api_schema_version !== '0.1.0' || manifest.version !== version || manifest.source_commit !== sourceCommit || manifest.build_target !== match[3]) {
      throw new Error(`${match[2]} manifest identity mismatch`);
    }
  }
  return expectedNames;
}

if (process.argv[1] && resolve(process.argv[1]) === new URL(import.meta.url).pathname) {
  const tag = process.argv[2];
  const sourceCommit = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
  const { version } = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
  console.log(`Verified ${verifyReleaseAssets({ tag, sourceCommit, version, releaseDir: new URL('../release/', import.meta.url).pathname }).join(', ')}`);
}
