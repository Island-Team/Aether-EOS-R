import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

const commit = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
const packageVersion = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8')).version;
for (const target of ['local', 'hosted']) {
  const manifest = JSON.parse(readFileSync(new URL(`../dist/${target}/manifest.json`, import.meta.url), 'utf8'));
  const html = readFileSync(new URL(`../dist/${target}/index.html`, import.meta.url), 'utf8');
  const base = target === 'hosted' ? '/isosui/' : '/';
  if (manifest.schema_version !== 1 || manifest.version !== packageVersion || manifest.api_schema_version !== '0.1.0' || manifest.build_target !== target || manifest.source_commit !== commit) {
    throw new Error(`${target} manifest does not match this source and IsOS API v1`);
  }
  if (!html.includes(`src=${base}assets/`) || !html.includes(`href=${base}IslandTeam.png`)) {
    throw new Error(`${target} HTML does not use ${base} assets`);
  }
}
console.log('Both IsosUI bundles and manifests are valid.');
