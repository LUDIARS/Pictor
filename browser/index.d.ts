declare const meshBrand: unique symbol;
export interface PictorMesh { readonly [meshBrand]: true }
export interface PictorFrame {
  width: number;
  height: number;
  viewProjection: Float32Array;
  clearColor: readonly [number, number, number];
}
export interface ObjectDescriptor { mesh: PictorMesh; transform: Float32Array }
export class PictorRenderer {
  readonly canvas: HTMLCanvasElement;
  constructor(canvas: HTMLCanvasElement);
  createMesh(vertices: Float32Array): PictorMesh;
  releaseMesh(mesh: PictorMesh): void;
  beginFrame(frame: PictorFrame): void;
  submit(object: ObjectDescriptor): void;
  endFrame(): void;
  dispose(): void;
}
