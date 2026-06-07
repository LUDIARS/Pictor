# Pictor Level Editor

Browser-based level editing prototype for Pictor scenes.

It provides a local WebGL2 viewport, hierarchy tree, asset palette, transform inspector, and JSON import/export for level data. The current renderer uses Pictor-style mesh descriptors with built-in preview meshes; the boundary is isolated in `webgl_model_viewer.js` so native Pictor model data can replace the preview mesh registry later.

## Run

```powershell
cd tools\level-editor
node .\serve.mjs
```

Open `http://localhost:8781/`.

## Level JSON

The exported format is intentionally simple:

```json
{
  "version": 1,
  "name": "sample_level",
  "assets": [
    {
      "id": "cover_block",
      "name": "Cover Block",
      "model": "models/cover_block.glb",
      "previewMesh": "cube",
      "color": [0.62, 0.72, 0.46, 1]
    }
  ],
  "nodes": [
    {
      "id": "cover_01",
      "parentId": "root",
      "name": "Cover Block",
      "kind": "model",
      "assetId": "cover_block",
      "transform": {
        "position": [3, 1, 2],
        "rotation": [0, 28, 0],
        "scale": [1.4, 1, 1]
      }
    }
  ]
}
```

## Integration Notes

- `webgl_model_viewer.js` owns WebGL buffer creation, shaders, camera, and mesh registry.
- `app.js` owns editor state, hierarchy, inspector, asset placement, import/export, and JSON preview.
- Asset entries already carry a `model` path so the next step can add GLB/native Pictor mesh loading without changing the editor data model.
