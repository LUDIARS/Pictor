// @ludiars/pictor-web-mesh — JavaScript face of the Pictor Web mesh module (SPEC-PC-WEB-MESH).
// The renderer runs in WebAssembly (C++ WebMeshRenderer + WebGL2). The host owns the canvas,
// the camera maths and the frame loop:
//
//   const renderer = await createWebMeshRenderer(canvas);
//   const mesh = renderer.createMesh({ positions, normals, colors, indices });
//   renderer.beginFrame({ width, height, viewProjection, cameraPosition, clearColor });
//   renderer.submit({ mesh, transform });
//   renderer.endFrame();
//   renderer.releaseMesh(mesh); renderer.dispose();

import { readCString, withHeap } from "./heap.js";
import { packLighting } from "./lighting.js";

export { hexToRgb, MAX_DIRECTIONAL_LIGHTS, packLighting } from "./lighting.js";

let canvasSerial = 0;

/** Emscripten looks the canvas up by CSS selector, so the canvas needs an id. */
function canvasSelector(canvas) {
  if (!canvas.isConnected) throw new Error("pictor web mesh: the canvas must be attached to the document");
  if (!canvas.id) {
    canvasSerial += 1;
    canvas.id = `pictor-web-mesh-${canvasSerial}`;
  }
  return `#${CSS.escape(canvas.id)}`;
}

function assertFloats(value, length, name) {
  if (!(value instanceof Float32Array) || value.length !== length) {
    throw new TypeError(`${name} must be a Float32Array of ${length}`);
  }
}

class WebMeshRenderer {
  #module;
  #id;
  #meshes = new Set();
  #disposed = false;

  constructor(module, id) {
    this.#module = module;
    this.#id = id;
  }

  #fail(operation) {
    const reason = readCString(this.#module, this.#module._pictor_web_last_error());
    throw new Error(`pictor web mesh: ${operation} failed: ${reason}`);
  }

  #assertLive() {
    if (this.#disposed) throw new Error("pictor web mesh: renderer is disposed");
  }

  /**
   * @param {{ positions: Float32Array, normals: Float32Array, colors: Float32Array, indices: Uint32Array, colorSpace?: "srgb" | "linear" }} mesh
   * @returns {{ readonly id: number }}
   */
  createMesh({ positions, normals, colors, indices, colorSpace = "srgb" }) {
    this.#assertLive();
    if (!(positions instanceof Float32Array) || !(normals instanceof Float32Array) || !(colors instanceof Float32Array)) {
      throw new TypeError("positions, normals and colors must be Float32Array");
    }
    if (!(indices instanceof Uint32Array)) throw new TypeError("indices must be a Uint32Array");
    if (positions.length % 3 !== 0 || normals.length !== positions.length || colors.length !== positions.length) {
      throw new RangeError("positions, normals and colors must hold 3 values per vertex, same length");
    }
    if (colorSpace !== "srgb" && colorSpace !== "linear") throw new RangeError("colorSpace must be 'srgb' or 'linear'");
    const module = this.#module;
    const id = withHeap(module, (heap) => module._pictor_web_mesh_create(
      this.#id, heap.floats(positions), heap.floats(normals), heap.floats(colors), positions.length / 3,
      heap.uints(indices), indices.length, colorSpace === "srgb" ? 1 : 0,
    ));
    if (!id) this.#fail("createMesh");
    const mesh = Object.freeze({ id });
    this.#meshes.add(mesh);
    return mesh;
  }

  releaseMesh(mesh) {
    this.#assertLive();
    if (!this.#meshes.has(mesh)) throw new Error("pictor web mesh: mesh does not belong to this renderer or is released");
    if (!this.#module._pictor_web_mesh_release(this.#id, mesh.id)) this.#fail("releaseMesh");
    this.#meshes.delete(mesh);
  }

  /** @param {import("./lighting.js").Lighting} lighting */
  setLighting(lighting) {
    this.#assertLive();
    const packed = packLighting(lighting);
    const ok = withHeap(this.#module, (heap) => this.#module._pictor_web_set_lighting(this.#id, heap.floats(packed)));
    if (!ok) this.#fail("setLighting");
  }

  setSurface({ roughness, metalness }) {
    this.#assertLive();
    if (!this.#module._pictor_web_set_surface(this.#id, roughness, metalness)) this.#fail("setSurface");
  }

  /**
   * @param {{ width: number, height: number, viewProjection: Float32Array, cameraPosition: Float32Array, clearColor?: [number, number, number] }} frame
   *   width/height are drawing-buffer pixels; matrices are column-major; clearColor is sRGB 0..1
   */
  beginFrame({ width, height, viewProjection, cameraPosition, clearColor = [0, 0, 0] }) {
    this.#assertLive();
    assertFloats(viewProjection, 16, "viewProjection");
    assertFloats(cameraPosition, 3, "cameraPosition");
    if (!Number.isInteger(width) || !Number.isInteger(height) || width <= 0 || height <= 0) {
      throw new RangeError("width and height must be positive integers (drawing-buffer pixels)");
    }
    const module = this.#module;
    const ok = withHeap(module, (heap) => module._pictor_web_begin_frame(
      this.#id, width, height, heap.floats(viewProjection), heap.floats(cameraPosition),
      clearColor[0], clearColor[1], clearColor[2],
    ));
    if (!ok) this.#fail("beginFrame");
  }

  /** @param {{ mesh: { readonly id: number }, transform: Float32Array }} draw column-major model matrix */
  submit({ mesh, transform }) {
    this.#assertLive();
    if (!this.#meshes.has(mesh)) throw new Error("pictor web mesh: mesh does not belong to this renderer or is released");
    assertFloats(transform, 16, "transform");
    const module = this.#module;
    const ok = withHeap(module, (heap) => module._pictor_web_submit(this.#id, mesh.id, heap.floats(transform)));
    if (!ok) this.#fail("submit");
  }

  endFrame() {
    this.#assertLive();
    if (!this.#module._pictor_web_end_frame(this.#id)) this.#fail("endFrame");
  }

  /** Frees every mesh and the WebGL context. Safe to call twice. */
  dispose() {
    if (this.#disposed) return;
    this.#disposed = true;
    this.#meshes.clear();
    this.#module._pictor_web_renderer_destroy(this.#id);
  }
}

/**
 * @param {HTMLCanvasElement} canvas attached to the document
 * @param {{ antialias?: boolean, moduleFactory?: () => Promise<object> }} [options]
 *   moduleFactory defaults to the bundled wasm build (pictor_web_mesh.mjs next to this file)
 */
export async function createWebMeshRenderer(canvas, { antialias = true, moduleFactory } = {}) {
  const factory = moduleFactory ?? (await import("./pictor_web_mesh.mjs")).default;
  const module = await factory();
  const id = withHeap(module, (heap) => module._pictor_web_renderer_create(heap.string(canvasSelector(canvas)), antialias ? 1 : 0));
  if (!id) {
    const reason = readCString(module, module._pictor_web_last_error());
    throw new Error(`pictor web mesh: renderer could not be created: ${reason}`);
  }
  return new WebMeshRenderer(module, id);
}
