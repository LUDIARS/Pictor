import assert from "node:assert/strict";
import { test } from "node:test";
import { createWebMeshRenderer, hexToRgb, packLighting } from "../index.js";
import { createFakeModule, fakeCanvas } from "./fake-module.mjs";

const IDENTITY = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
const TRIANGLE = {
  positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
  normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]),
  colors: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]),
  indices: new Uint32Array([0, 1, 2]),
};

async function setup(options) {
  const module = createFakeModule(options);
  const renderer = await createWebMeshRenderer(fakeCanvas("stage"), { moduleFactory: async () => module });
  return { module, renderer };
}

test("the renderer is created on the canvas selector and every heap allocation is freed", async () => {
  const { module, renderer } = await setup();
  assert.deepEqual(module.calls[0], ["create", "#stage", 1]);
  const mesh = renderer.createMesh(TRIANGLE);
  const [, call] = module.calls.find(([name]) => name === "mesh");
  assert.deepEqual(call.positions, [0, 0, 0, 1, 0, 0, 0, 1, 0]);
  assert.deepEqual(call.indices, [0, 1, 2]);
  assert.equal(call.srgb, 1, "colours are sRGB by default");
  renderer.beginFrame({ width: 4, height: 2, viewProjection: IDENTITY, cameraPosition: new Float32Array([0, 0, 5]), clearColor: [0.1, 0.2, 0.3] });
  renderer.submit({ mesh, transform: IDENTITY });
  assert.equal(module.live.size, 0, "no wasm heap left allocated");
  renderer.releaseMesh(mesh);
  assert.throws(() => renderer.submit({ mesh, transform: IDENTITY }), /released/);
  renderer.dispose();
  renderer.dispose();
  assert.equal(module.calls.filter(([name]) => name === "destroy").length, 1);
  assert.throws(() => renderer.createMesh(TRIANGLE), /disposed/);
});

test("a canvas without id gets one", async () => {
  const module = createFakeModule();
  const canvas = fakeCanvas("");
  await createWebMeshRenderer(canvas, { moduleFactory: async () => module });
  assert.match(canvas.id, /^pictor-web-mesh-\d+$/);
  assert.equal(module.calls[0][1], `#${canvas.id}`);
});

test("wasm-side failures surface as errors with the module's reason", async () => {
  await assert.rejects(
    createWebMeshRenderer(fakeCanvas("x"), { moduleFactory: async () => createFakeModule({ failCreate: true }) }),
    /renderer could not be created: WebGL2 context could not be created/,
  );
  const { module, renderer } = await setup({ failMesh: true });
  assert.throws(() => renderer.createMesh(TRIANGLE), /createMesh failed: invalid mesh: index_out_of_range/);
  assert.equal(module.live.size, 0, "heap is freed on failure too");
  assert.throws(() => renderer.endFrame(), /endFrame failed/);
});

test("inputs are checked before reaching wasm", async () => {
  const { renderer } = await setup();
  assert.throws(() => renderer.createMesh({ ...TRIANGLE, indices: [0, 1, 2] }), /Uint32Array/);
  assert.throws(() => renderer.createMesh({ ...TRIANGLE, colors: new Float32Array(6) }), /same length/);
  assert.throws(() => renderer.createMesh({ ...TRIANGLE, colorSpace: "hsv" }), /colorSpace/);
  assert.throws(() => renderer.beginFrame({ width: 0, height: 1, viewProjection: IDENTITY, cameraPosition: new Float32Array(3) }), /positive/);
  assert.throws(() => renderer.submit({ mesh: { id: 1 }, transform: IDENTITY }), /does not belong/);
  const mesh = renderer.createMesh(TRIANGLE);
  assert.throws(() => renderer.submit({ mesh, transform: new Float32Array(9) }), /Float32Array of 16/);
});

test("lighting is packed in the C ABI layout", async () => {
  const lighting = {
    sky: hexToRgb(0xfff0dc),
    ground: [0, 0, 0],
    hemisphereIntensity: 1.15,
    directional: [{ direction: [3, 5, 4], color: [1, 1, 1], intensity: 1.5 }],
  };
  const packed = packLighting(lighting);
  assert.equal(packed.length, 22);
  assert.equal(packed[7], 1, "light count");
  assert.deepEqual(Array.from(packed.subarray(8, 15)), [3, 5, 4, 1, 1, 1, 1.5]);
  assert.ok(Math.abs(packed[1] - 0xf0 / 255) < 1e-6);
  const { module, renderer } = await setup();
  renderer.setLighting(lighting);
  renderer.setSurface({ roughness: 0.55, metalness: 0.05 });
  assert.equal(module.calls.find(([name]) => name === "lighting")[1][7], 1);
  assert.throws(() => packLighting({ ...lighting, directional: [lighting.directional[0], lighting.directional[0], lighting.directional[0]] }), /at most 2/);
  assert.throws(() => hexToRgb(0x1000000), RangeError);
});
