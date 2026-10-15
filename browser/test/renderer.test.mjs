import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { PictorRenderer } from '../index.js';
import { createFakeGL, fakeCanvas } from './fake-webgl.mjs';

const IDENTITY = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
const TRIANGLE = new Float32Array([0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1]);
const FRAME = { width: 8, height: 4, viewProjection: IDENTITY, clearColor: [0.1, 0.2, 0.3] };

function setup(options) {
  const gl = createFakeGL(options);
  return { gl, renderer: new PictorRenderer(fakeCanvas(gl)) };
}

const buffers = (gl) => [...gl.live].filter((o) => o.kind === 'buffer').length;

test('the renderer owns the program and mesh buffers and releases them all on dispose', () => {
  const { gl, renderer } = setup();
  assert.deepEqual([...gl.live].map((o) => o.kind), ['program'], 'temporary shaders are deleted after link');
  renderer.createMesh(TRIANGLE);
  renderer.createMesh(TRIANGLE);
  assert.equal(buffers(gl), 2);
  renderer.dispose();
  renderer.dispose();
  assert.equal(gl.live.size, 0, 'no GPU object outlives dispose');
  assert.equal(gl.loseCalls, 1, 'the context is released once');
  assert.throws(() => renderer.createMesh(TRIANGLE), /disposed/);
});

test('mesh handles are opaque and expose no GPU state', () => {
  const { renderer } = setup();
  const mesh = renderer.createMesh(TRIANGLE);
  assert.deepEqual(Reflect.ownKeys(mesh), []);
  assert.ok(Object.isFrozen(mesh));
  renderer.beginFrame(FRAME);
  assert.throws(() => renderer.submit({ mesh: { slot: 0 }, transform: IDENTITY }), /unknown mesh/);
});

test('a frame draws submitted meshes with model and view-projection matrices', () => {
  const { gl, renderer } = setup();
  const mesh = renderer.createMesh(TRIANGLE);
  renderer.beginFrame(FRAME);
  renderer.submit({ mesh, transform: IDENTITY });
  renderer.endFrame();
  assert.equal(renderer.canvas.width, 8);
  assert.equal(renderer.canvas.height, 4);
  assert.deepEqual(gl.calls.map((c) => c.join(':')),
    ['viewport:0:0:8:4', 'clear', 'uniform:viewProjection', 'uniform:model', 'draw:0:3', 'flush']);
});

test('frame boundaries are enforced', () => {
  const { renderer } = setup();
  const mesh = renderer.createMesh(TRIANGLE);
  assert.throws(() => renderer.submit({ mesh, transform: IDENTITY }), /outside frame/);
  assert.throws(() => renderer.endFrame(), /outside frame/);
  renderer.beginFrame(FRAME);
  assert.throws(() => renderer.beginFrame(FRAME), /already begun/);
  renderer.endFrame();
});

test('invalid geometry, matrices and frame parameters are rejected', () => {
  const { renderer } = setup();
  assert.throws(() => renderer.createMesh(new Float32Array(17)), /triangle vertices/);
  assert.throws(() => renderer.createMesh([0, 0, 0]), /triangle vertices/);
  assert.throws(() => renderer.createMesh(new Float32Array(18).fill(NaN)), /triangle vertices/);
  assert.throws(() => renderer.beginFrame({ ...FRAME, width: 0 }), /dimensions/);
  assert.throws(() => renderer.beginFrame({ ...FRAME, height: 5000 }), /dimensions/);
  assert.throws(() => renderer.beginFrame({ ...FRAME, viewProjection: new Float32Array(15) }), /frame parameters/);
  assert.throws(() => renderer.beginFrame({ ...FRAME, clearColor: [0, 0] }), /frame parameters/);
  const mesh = renderer.createMesh(TRIANGLE);
  renderer.beginFrame(FRAME);
  assert.throws(() => renderer.submit({ mesh, transform: new Float32Array(16).fill(Infinity) }), /invalid transform/);
});

test('released and foreign meshes are rejected and slots get fresh handles', () => {
  const { gl, renderer } = setup();
  const foreign = new PictorRenderer(fakeCanvas(createFakeGL())).createMesh(TRIANGLE);
  const mesh = renderer.createMesh(TRIANGLE);
  renderer.releaseMesh(mesh);
  assert.equal(buffers(gl), 0);
  assert.throws(() => renderer.releaseMesh(mesh), /released/);
  renderer.beginFrame(FRAME);
  assert.throws(() => renderer.submit({ mesh, transform: IDENTITY }), /unknown mesh/);
  assert.throws(() => renderer.submit({ mesh: foreign, transform: IDENTITY }), /unknown mesh/);
  renderer.endFrame();
  const reused = renderer.createMesh(TRIANGLE);
  assert.notEqual(reused, mesh);
  assert.throws(() => renderer.releaseMesh(mesh), /released/);
  renderer.releaseMesh(reused);
});

test('construction failures release every partially allocated resource', () => {
  for (const options of [{ failCompile: true }, { failLink: true }, { missingUniform: true }]) {
    const gl = createFakeGL(options);
    assert.throws(() => new PictorRenderer(fakeCanvas(gl)));
    assert.equal(gl.live.size, 0, JSON.stringify(options));
  }
  assert.throws(() => new PictorRenderer({}), /HTML canvas/);
  assert.throws(() => new PictorRenderer({ getContext: () => null }), /WebGL2/);
});

test('context loss is reported instead of silently drawing', () => {
  const { gl, renderer } = setup();
  gl.lost = true;
  assert.throws(() => renderer.createMesh(TRIANGLE), /context is lost/);
  assert.throws(() => renderer.beginFrame(FRAME), /context is lost/);
});

test('the package is standalone, versioned and does not load a wasm demo', () => {
  const pkg = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
  assert.match(pkg.version, /^\d+\.\d+\.\d+$/);
  assert.equal(pkg.dependencies, undefined);
  assert.equal(pkg.peerDependencies, undefined);
  for (const file of pkg.files.filter((f) => /\.(js|ts)$/.test(f))) {
    const source = readFileSync(new URL(`../${file}`, import.meta.url), 'utf8');
    assert.doesNotMatch(source, /pagus|wasm|emscripten/i, file);
    for (const [, spec] of source.matchAll(/from\s+['"]([^'"]+)['"]/g)) assert.match(spec, /^\.\/[\w-]+\.js$/, `${file} imports ${spec}`);
  }
});
