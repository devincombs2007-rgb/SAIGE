#!/usr/bin/env node
// gltf_optimize.mjs: prepares a glTF/GLB for SAIGE with glTF Transform and meshoptimizer.
//
//   gltf_optimize.mjs IN.glb OUT.glb [--profile saige|distribution] [--max-texture 1024] [--lods 1,0.5,0.25] [--simplify-error 0.01] [--weld] [--report report.json]
//
// profile `saige` (default): what the engine's importer accepts today: plain glTF 2.0 (no Draco, meshopt or mesh quantisation, no KTX2), PNG/JPEG textures.
//   Steps: dedup, prune unused, weld near-identical vertices, optionally resize textures (sharp, keeps the image format), optionally LODs (meshoptimizer simplification, one file per ratio).
// profile `distribution`: additionally EXT_meshopt_compression and KTX2 textures: smaller downloads, for a future streaming pipeline (the engine cannot load them yet).
import fs from 'node:fs';
import path from 'node:path';
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS, EXTMeshoptCompression } from '@gltf-transform/extensions';
import { dedup, prune, weld, simplify, textureCompress, getBounds, flatten } from '@gltf-transform/functions';
import { MeshoptSimplifier, MeshoptEncoder, MeshoptDecoder } from 'meshoptimizer';
import sharp from 'sharp';

const args = process.argv.slice(2);
const [input, output] = args;
if (!input || !output) { console.error('usage: gltf_optimize.mjs IN OUT [--profile saige|distribution] [--max-texture N] [--lods r1,r2] [--simplify-error E] [--weld] [--report FILE]'); process.exit(2); }
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const profile = opt('--profile', 'saige');
const maxTexture = parseInt(opt('--max-texture', '0'));
const lods = opt('--lods', '').split(',').filter(Boolean).map(Number);
const simplifyError = parseFloat(opt('--simplify-error', '0.01'));
const doWeld = args.includes('--weld');
const reportPath = opt('--report', '');

await MeshoptSimplifier.ready; await MeshoptEncoder.ready; await MeshoptDecoder.ready;
const io = new NodeIO().registerExtensions(ALL_EXTENSIONS).registerDependencies({ 'meshopt.decoder': MeshoptDecoder, 'meshopt.encoder': MeshoptEncoder });

function stats(doc) {
  let tris = 0, verts = 0, texBytes = 0;
  for (const mesh of doc.getRoot().listMeshes()) for (const p of mesh.listPrimitives()) {
    const idx = p.getIndices(); const pos = p.getAttribute('POSITION');
    if (pos) verts += pos.getCount();
    tris += idx ? idx.getCount() / 3 : (pos ? pos.getCount() / 3 : 0);
  }
  for (const t of doc.getRoot().listTextures()) { const s = t.getSize(); if (s) texBytes += Math.round(s[0] * s[1] * 4 * 4 / 3); }
  return { triangles: tris, vertices: verts, texture_gpu_bytes: texBytes, textures: doc.getRoot().listTextures().length, materials: doc.getRoot().listMaterials().length, nodes: doc.getRoot().listNodes().length };
}

const before = fs.statSync(input).size;
let doc = await io.read(input);
const statsBefore = stats(doc);
const steps = [];
const required = doc.getRoot().listExtensionsRequired().map(e => e.extensionName);
await doc.transform(dedup(), prune()); steps.push('dedup', 'prune');
if (doWeld) { await doc.transform(weld()); steps.push('weld'); }
if (maxTexture > 0) {
  await doc.transform(textureCompress({ encoder: sharp, resize: [maxTexture, maxTexture], resizeFilter: 'lanczos3' }));
  steps.push(`textures<=${maxTexture}`);
}
const outputs = [];
async function write(d, file) {
  if (profile === 'distribution') {
    d.createExtension(EXTMeshoptCompression).setRequired(true).setEncoderOptions({ method: EXTMeshoptCompression.EncoderMethod.QUANTIZE });
  }
  await io.write(file, d);
  outputs.push({ file, bytes: fs.statSync(file).size, ...stats(d) });
}
await write(doc, output);
for (const ratio of lods) {
  const lodDoc = await io.read(input);
  await lodDoc.transform(dedup(), prune());
  if (maxTexture > 0) await lodDoc.transform(textureCompress({ encoder: sharp, resize: [Math.max(64, Math.round(maxTexture * Math.sqrt(ratio)))] .concat([Math.max(64, Math.round(maxTexture * Math.sqrt(ratio)))]), resizeFilter: 'lanczos3' }));
  await lodDoc.transform(weld(), simplify({ simplifier: MeshoptSimplifier, ratio, error: simplifyError, lockBorder: true }), prune());
  const file = output.replace(/\.glb$|\.gltf$/i, '') + `_lod${ratio}.glb`;
  await write(lodDoc, file);
  outputs[outputs.length - 1].lod_ratio = ratio;
}
const report = { input, input_bytes: before, profile, required_extensions: required, steps, before: statsBefore, outputs };
if (reportPath) fs.writeFileSync(reportPath, JSON.stringify(report, null, 1));
console.log(JSON.stringify({ input: path.basename(input), before: statsBefore, outputs: outputs.map(o => ({ file: path.basename(o.file), bytes: o.bytes, triangles: o.triangles, lod: o.lod_ratio })) }));
