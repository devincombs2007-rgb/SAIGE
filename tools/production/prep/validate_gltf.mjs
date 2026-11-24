#!/usr/bin/env node
// validate_gltf.mjs: the Khronos glTF Validator on a file; prints a JSON summary and exits 1 on errors.
//   validate_gltf.mjs FILE.glb|gltf
import fs from 'node:fs';
import path from 'node:path';
import validator from 'gltf-validator';
const file = process.argv[2];
if (!file) { console.error('usage: validate_gltf.mjs FILE'); process.exit(2); }
const data = new Uint8Array(fs.readFileSync(file));
const report = await validator.validateBytes(data, {
  uri: path.basename(file),
  externalResourceFunction: (uri) => new Promise((res, rej) => fs.readFile(path.resolve(path.dirname(file), decodeURIComponent(uri)), (e, b) => e ? rej(e) : res(new Uint8Array(b)))),
  maxIssues: 50,
});
const i = report.issues;
console.log(JSON.stringify({ file: path.basename(file), generator: report.info.generator, version: report.info.version, errors: i.numErrors, warnings: i.numWarnings, infos: i.numInfos, hints: i.numHints,
  first_errors: i.messages.filter(m => m.severity === 0).slice(0, 5).map(m => `${m.code} ${m.pointer}`), stats: { drawCalls: report.info.drawCallCount, triangles: report.info.totalTriangleCount, animations: report.info.animationCount, materials: report.info.materialCount } }));
process.exit(i.numErrors ? 1 : 0);
