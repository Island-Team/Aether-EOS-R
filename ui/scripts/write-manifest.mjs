import { execFileSync } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { readFileSync } from 'node:fs';

const target = process.argv[2];
if (target !== 'local' && target !== 'hosted') throw new Error('Expected local or hosted target.');
const pkg = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
const sourceCommit = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
const manifest = {
  schema_version: 1,
  version: pkg.version,
  source_commit: sourceCommit,
  api_schema_version: '0.1.0',
  build_target: target,
};
await mkdir(new URL(`../dist/${target}/`, import.meta.url), { recursive: true });
await writeFile(new URL(`../dist/${target}/manifest.json`, import.meta.url), `${JSON.stringify(manifest, null, 2)}\n`);
