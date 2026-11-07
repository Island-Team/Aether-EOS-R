import { readFileSync } from 'node:fs';

const [tag, promotion] = process.argv.slice(2);
const { version } = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
if (!/^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(tag ?? '') || tag !== `v${version}`) {
  throw new Error(`release tag must equal v${version}`);
}
if (!['manual', 'dispatch'].includes(promotion)) {
  throw new Error('promotion must be manual or dispatch');
}
if (promotion === 'dispatch' && !process.env.DISPATCH_TOKEN) {
  throw new Error('ISLAND_WEB_DISPATCH_TOKEN is required for dispatch promotion');
}
console.log(`Release input valid: ${tag}, ${promotion} promotion`);
