import { createMeshProgram } from './mesh-program.js';

/** Browser-native Pictor backend. All GPU state remains private to this owner. */
export class PictorRenderer {
  #gl;
  #program;
  #model;
  #viewProjection;
  #maxViewport;
  #buffers = [];
  #counts = [];
  #handles = [];
  #free = [];
  #disposed = false;
  #frame = false;
  constructor(canvas) {
    if (!canvas || typeof canvas.getContext !== 'function') throw new Error('Pictor requires an HTML canvas');
    Object.defineProperty(this, 'canvas', { value: canvas, enumerable: true });
    const gl = canvas.getContext('webgl2', { antialias: true, alpha: false });
    if (!gl) throw new Error('Pictor requires WebGL2');
    this.#gl = gl;
    const maxViewport = gl.getParameter(gl.MAX_VIEWPORT_DIMS);
    if (!maxViewport || maxViewport.length !== 2) throw new Error('Pictor: failed to query WebGL2 limits');
    this.#maxViewport = maxViewport;
    const program = createMeshProgram(gl);
    try {
      this.#model = gl.getUniformLocation(program, 'model');
      this.#viewProjection = gl.getUniformLocation(program, 'viewProjection');
      if (this.#model === null || this.#viewProjection === null) throw new Error('Pictor: missing matrix uniforms');
      this.#program = program;
    } catch (error) { gl.deleteProgram(program); throw error; }
  }
  #alive() {
    if (this.#disposed) throw new Error('Pictor renderer is disposed');
    if (this.#gl.isContextLost()) throw new Error('Pictor rendering context is lost');
  }
  createMesh(vertices) {
    this.#alive();
    if (!(vertices instanceof Float32Array) || !vertices.length || vertices.length % 18 || !vertices.every(Number.isFinite))
      throw new Error('Pictor meshes require finite triangle vertices (xyz + rgb)');
    const gl = this.#gl, buffer = gl.createBuffer();
    if (!buffer) throw new Error('Pictor: mesh allocation failed');
    try {
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);
      if (gl.getError() !== gl.NO_ERROR) throw new Error('Pictor: mesh upload failed');
      const slot = this.#free.pop() ?? this.#buffers.length;
      const mesh = Object.freeze({ slot });
      this.#buffers[slot] = buffer;
      this.#counts[slot] = vertices.length / 6;
      this.#handles[slot] = mesh;
      return mesh;
    } catch (error) { gl.deleteBuffer(buffer); throw error; }
  }
  releaseMesh(mesh) {
    const slot = mesh?.slot;
    if (!mesh || this.#handles[slot] !== mesh) throw new Error('Pictor: mesh does not belong to this renderer or was released');
    this.#gl.deleteBuffer(this.#buffers[slot]);
    this.#buffers[slot] = null; this.#counts[slot] = 0; this.#handles[slot] = null; this.#free.push(slot);
  }
  beginFrame(frame) {
    this.#alive();
    if (this.#frame) throw new Error('Pictor: frame already begun');
    if (!frame || typeof frame !== 'object') throw new Error('Pictor: invalid frame parameters');
    const { width, height, viewProjection, clearColor } = frame;
    if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width < 1 || height < 1 ||
        width > this.#maxViewport[0] || height > this.#maxViewport[1])
      throw new Error('Pictor: invalid frame dimensions');
    if (!(viewProjection instanceof Float32Array) || viewProjection.length !== 16 || !viewProjection.every(Number.isFinite) ||
        !Array.isArray(clearColor) || clearColor.length !== 3 || !clearColor.every(Number.isFinite))
      throw new Error('Pictor: invalid frame parameters');
    const gl = this.#gl;
    if (this.canvas.width !== width) this.canvas.width = width;
    if (this.canvas.height !== height) this.canvas.height = height;
    gl.viewport(0, 0, width, height); gl.clearColor(...clearColor, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT); gl.enable(gl.DEPTH_TEST);
    gl.useProgram(this.#program); gl.uniformMatrix4fv(this.#viewProjection, false, viewProjection);
    this.#frame = true;
  }
  submit(object) {
    this.#alive();
    if (!this.#frame) throw new Error('Pictor: submit outside frame');
    if (!object || typeof object !== 'object') throw new Error('Pictor: invalid object descriptor');
    const { mesh, transform } = object;
    const slot = mesh?.slot;
    if (!mesh || this.#handles[slot] !== mesh) throw new Error('Pictor: unknown mesh');
    if (!(transform instanceof Float32Array) || transform.length !== 16 || !transform.every(Number.isFinite)) throw new Error('Pictor: invalid transform');
    const gl = this.#gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.#buffers[slot]);
    gl.enableVertexAttribArray(0); gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 24, 0); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 24, 12);
    gl.uniformMatrix4fv(this.#model, false, transform); gl.drawArrays(gl.TRIANGLES, 0, this.#counts[slot]);
  }
  endFrame() {
    this.#alive();
    if (!this.#frame) throw new Error('Pictor: endFrame outside frame');
    this.#frame = false;
    this.#gl.flush();
  }
  dispose() {
    if (this.#disposed) return;
    for (const mesh of this.#handles) if (mesh) this.releaseMesh(mesh);
    this.#buffers.length = 0; this.#counts.length = 0; this.#handles.length = 0; this.#free.length = 0;
    this.#gl.deleteProgram(this.#program);
    this.#gl.getExtension('WEBGL_lose_context')?.loseContext();
    this.#disposed = true; this.#frame = false;
  }
}
