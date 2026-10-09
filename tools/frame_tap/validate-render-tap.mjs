#!/usr/bin/env node
// Validate frame tap JSON Lines against the render-tap/1 contract
// (spec/feature/frame-tap.md §9). The schema and golden live in Commentarii; this script only
// reads them. Pictor carries no node dependencies, so Ajv is resolved from a directory given
// with --ajv-from (for example the Commentarii checkout, which has ajv in node_modules).
//
//   node tools/frame_tap/validate-render-tap.mjs \
//     --schema <Commentarii>/schema/render-frame.schema.json \
//     --golden <Commentarii>/tests/fixtures/render-tap/golden-v1.jsonl \
//     --ajv-from <Commentarii> \
//     <build>/frame_tap_fixed_scene.jsonl
//
// Checks, for the golden and for every given file:
//   1. every line parses and conforms to the schema;
//   2. stream shape like the golden: seq starts at 0 and goes up by 1, the last line is the
//      end line, frame strictly increases, t and tick never go back, every frame has observer.
// Exit code 0 when everything passes, 1 otherwise.

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';

function parseArgs(argv) {
  const args = { files: [] };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--schema' || arg === '--golden' || arg === '--ajv-from') {
      args[arg.slice(2).replace('-', '_')] = argv[i + 1];
      i += 1;
    } else {
      args.files.push(arg);
    }
  }
  if (!args.schema || !args.ajv_from) {
    throw new Error('usage: validate-render-tap.mjs --schema <file> --ajv-from <dir> [--golden <file>] <jsonl...>');
  }
  return args;
}

function loadValidator(schemaPath, ajvFrom) {
  const require = createRequire(path.join(path.resolve(ajvFrom), 'package.json'));
  const Ajv2020 = require('ajv/dist/2020').default;
  const ajv = new Ajv2020({ allErrors: true, strict: false });
  return ajv.compile(JSON.parse(readFileSync(schemaPath, 'utf8')));
}

function readLines(file) {
  return readFileSync(file, 'utf8').split('\n').filter((line) => line.length > 0);
}

function checkSchema(validate, lines) {
  const problems = [];
  lines.forEach((text, index) => {
    let line;
    try {
      line = JSON.parse(text);
    } catch (error) {
      problems.push(`line ${index + 1}: not JSON (${error.message})`);
      return;
    }
    if (!validate(line)) {
      for (const error of validate.errors ?? []) {
        problems.push(`line ${index + 1}: ${error.instancePath || '/'} ${error.message}`);
      }
    }
  });
  return problems;
}

function checkStreamShape(lines) {
  const problems = [];
  const parsed = lines.map((text) => JSON.parse(text));
  if (parsed.length === 0) return ['empty stream'];
  parsed.forEach((line, index) => {
    if (line.seq !== index) problems.push(`line ${index + 1}: seq ${line.seq}, expected ${index}`);
  });
  const last = parsed[parsed.length - 1];
  if (last.end === undefined) problems.push('the stream does not close with an end line');
  const frames = parsed.filter((line) => line.end === undefined);
  if (frames.length === 0) problems.push('no frame line');
  if (parsed.slice(0, -1).some((line) => line.end !== undefined)) problems.push('end line before the last line');
  for (let i = 0; i < frames.length; i += 1) {
    const frame = frames[i];
    if (!frame.observer || typeof frame.observer.id !== 'string') problems.push(`seq ${frame.seq}: no observer`);
    if (i === 0) continue;
    const previous = frames[i - 1];
    if (frame.frame <= previous.frame) problems.push(`seq ${frame.seq}: frame does not increase`);
    if (frame.t < previous.t) problems.push(`seq ${frame.seq}: t goes back`);
    if (frame.tick !== undefined && previous.tick !== undefined && frame.tick < previous.tick) {
      problems.push(`seq ${frame.seq}: tick goes back`);
    }
  }
  return problems;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  const validate = loadValidator(args.schema, args.ajv_from);
  const targets = args.golden ? [args.golden, ...args.files] : args.files;
  let failed = false;
  for (const file of targets) {
    const lines = readLines(file);
    const problems = [...checkSchema(validate, lines), ...checkStreamShape(lines)];
    if (problems.length === 0) {
      console.log(`ok   ${file} (${lines.length} lines)`);
    } else {
      failed = true;
      console.log(`FAIL ${file}`);
      for (const problem of problems) console.log(`  - ${problem}`);
    }
  }
  process.exitCode = failed ? 1 : 0;
}

main();
