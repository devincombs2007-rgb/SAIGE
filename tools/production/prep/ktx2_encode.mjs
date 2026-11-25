#!/usr/bin/env node
// ktx2_encode.mjs: Basis Universal -> KTX2 with the pure-WASM encoder (no native toktx or GitHub release needed).
//   ktx2_encode.mjs IN.png|jpg OUT.ktx2 [--mode etc1s|uastc] [--srgb|--linear] [--no-mips] [--quality 128]
// etc1s: small (about 1.5 bits/texel of GPU data on disk), for base color; uastc: higher quality, bigger, for normals and data textures.
import fs from 'node:fs';
import sharp from 'sharp';
import { encodeToKTX2 } from 'ktx2-encoder';
const args = process.argv.slice(2);
const [input, output] = args;
if (!input || !output) { console.error('usage: ktx2_encode.mjs IN OUT [--mode etc1s|uastc] [--srgb|--linear] [--no-mips] [--quality N]'); process.exit(2); }
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const mode = opt('--mode', 'etc1s');
const out = await encodeToKTX2(new Uint8Array(fs.readFileSync(input)), {
  imageDecoder: async (buf) => { const r = await sharp(Buffer.from(buf)).ensureAlpha().raw().toBuffer({ resolveWithObject: true }); return { width: r.info.width, height: r.info.height, data: new Uint8Array(r.data) }; },
  isUASTC: mode === 'uastc', generateMipmap: !args.includes('--no-mips'), isSetKTX2SRGBTransferFunc: !args.includes('--linear'), qualityLevel: parseInt(opt('--quality', '128')), enableDebug: false,
});
fs.writeFileSync(output, out);
console.log(JSON.stringify({ input, output, mode, bytes: out.length }));
