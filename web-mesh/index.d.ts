export type Rgb = [number, number, number];

export interface DirectionalLight {
  /** From the surface towards the light. */
  direction: Rgb;
  /** sRGB 0..1 */
  color: Rgb;
  intensity: number;
}

export interface Lighting {
  sky: Rgb;
  ground: Rgb;
  hemisphereIntensity: number;
  directional: DirectionalLight[];
}

export interface WebMesh {
  readonly id: number;
}

export interface WebMeshInput {
  positions: Float32Array;
  normals: Float32Array;
  colors: Float32Array;
  indices: Uint32Array;
  colorSpace?: "srgb" | "linear";
}

export interface WebFrame {
  /** Drawing-buffer pixels. */
  width: number;
  height: number;
  /** Column-major 4x4. */
  viewProjection: Float32Array;
  cameraPosition: Float32Array;
  /** sRGB 0..1 */
  clearColor?: Rgb;
}

export interface WebMeshRenderer {
  createMesh(mesh: WebMeshInput): WebMesh;
  releaseMesh(mesh: WebMesh): void;
  setLighting(lighting: Lighting): void;
  setSurface(surface: { roughness: number; metalness: number }): void;
  beginFrame(frame: WebFrame): void;
  submit(draw: { mesh: WebMesh; transform: Float32Array }): void;
  endFrame(): void;
  dispose(): void;
}

export const MAX_DIRECTIONAL_LIGHTS: 2;
export function packLighting(lighting: Lighting): Float32Array;
export function hexToRgb(hex: number): Rgb;
export function createWebMeshRenderer(
  canvas: HTMLCanvasElement,
  options?: { antialias?: boolean; moduleFactory?: () => Promise<object> },
): Promise<WebMeshRenderer>;
