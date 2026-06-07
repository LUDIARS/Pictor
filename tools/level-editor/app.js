import { WebGLModelViewer, defaultAssetCatalog } from "./webgl_model_viewer.js";

const $ = (id) => document.getElementById(id);

const defaultTransform = () => ({
  position: [0, 0, 0],
  rotation: [0, 0, 0],
  scale: [1, 1, 1],
});

const state = {
  selectedId: "root",
  tool: "select",
  level: {
    version: 1,
    name: "sample_level",
    assets: defaultAssetCatalog(),
    nodes: [
      { id: "root", parentId: null, name: "Level", kind: "group", transform: defaultTransform() },
      { id: "floor_01", parentId: "root", name: "Stage Floor", kind: "model", assetId: "stage_floor", transform: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [12, 1, 12] } },
      { id: "spawn_01", parentId: "root", name: "Player Spawn", kind: "model", assetId: "spawn_marker", transform: { position: [0, 0.25, 0], rotation: [0, 0, 0], scale: [0.45, 0.45, 0.45] } },
      { id: "enemy_01", parentId: "root", name: "Enemy Anchor A", kind: "model", assetId: "enemy_anchor", transform: { position: [-4, 0.65, -2], rotation: [0, 0, 0], scale: [0.65, 0.65, 0.65] } },
      { id: "cover_01", parentId: "root", name: "Cover Block", kind: "model", assetId: "cover_block", transform: { position: [3, 1, 2], rotation: [0, 28, 0], scale: [1.4, 1, 1] } },
    ],
  },
};

const viewer = new WebGLModelViewer($("viewport"));

function uid(prefix) {
  return `${prefix}_${Math.random().toString(36).slice(2, 8)}`;
}

function selectedNode() {
  return state.level.nodes.find((node) => node.id === state.selectedId) || null;
}

function childrenOf(parentId) {
  return state.level.nodes.filter((node) => node.parentId === parentId);
}

function setStatus(text, tone = "") {
  const status = $("status");
  status.textContent = text;
  status.className = `status ${tone}`.trim();
}

function serializeLevel() {
  return JSON.stringify(state.level, null, 2);
}

function renderHierarchy() {
  const tree = $("hierarchy");
  tree.innerHTML = "";
  const walk = (parentId, depth) => {
    for (const node of childrenOf(parentId)) {
      const row = document.createElement("div");
      row.className = `treeNode${node.id === state.selectedId ? " selected" : ""}`;
      row.style.setProperty("--indent", `${depth * 14}px`);
      row.role = "treeitem";
      row.innerHTML = `<span class="nodeIcon">${node.kind === "group" ? "◆" : "●"}</span><span class="nodeName"><span class="treeIndent"></span>${node.name}</span>`;
      row.addEventListener("click", () => {
        state.selectedId = node.id;
        renderAll();
      });
      tree.appendChild(row);
      walk(node.id, depth + 1);
    }
  };
  walk(null, 0);
}

function renderAssets() {
  $("assetCount").textContent = String(state.level.assets.length);
  const list = $("assetList");
  list.innerHTML = "";
  for (const asset of state.level.assets) {
    const item = document.createElement("div");
    item.className = "assetItem";
    item.innerHTML = `
      <div class="swatch" style="background: rgba(${asset.color.slice(0, 3).map((v) => Math.round(v * 255)).join(",")},1)"></div>
      <div>
        <div class="assetName">${asset.name}</div>
        <div class="assetMeta">${asset.previewMesh} · ${asset.model}</div>
      </div>`;
    item.addEventListener("click", () => addAssetNode(asset));
    list.appendChild(item);
  }
}

function field(label, input) {
  const row = document.createElement("div");
  row.className = "field";
  const l = document.createElement("label");
  l.textContent = label;
  row.append(l, input);
  return row;
}

function textInput(value, onInput) {
  const input = document.createElement("input");
  input.type = "text";
  input.value = value ?? "";
  input.addEventListener("input", () => onInput(input.value));
  return input;
}

function selectInput(value, options, onInput) {
  const select = document.createElement("select");
  for (const option of options) {
    const el = document.createElement("option");
    el.value = option.value;
    el.textContent = option.label;
    select.appendChild(el);
  }
  select.value = value ?? "";
  select.addEventListener("change", () => onInput(select.value));
  return select;
}

function vectorInput(values, onInput) {
  const wrap = document.createElement("div");
  wrap.className = "triple";
  values.forEach((value, index) => {
    const input = document.createElement("input");
    input.type = "number";
    input.step = "0.1";
    input.value = Number(value).toFixed(2);
    input.addEventListener("input", () => {
      const next = [...values];
      const parsed = parseFloat(input.value);
      next[index] = Number.isFinite(parsed) ? parsed : 0;
      onInput(next);
    });
    wrap.appendChild(input);
  });
  return wrap;
}

function renderInspector() {
  const panel = $("inspector");
  panel.innerHTML = "";
  const node = selectedNode();
  $("selectedType").textContent = node ? node.kind : "none";
  if (!node) {
    panel.appendChild(Object.assign(document.createElement("div"), { className: "emptyState", textContent: "No node selected" }));
    return;
  }

  panel.appendChild(field("Name", textInput(node.name, (value) => {
    node.name = value || node.id;
    renderAll();
  })));
  panel.appendChild(field("Kind", selectInput(node.kind, [
    { value: "group", label: "Group" },
    { value: "model", label: "Model" },
  ], (value) => {
    node.kind = value;
    if (value === "model" && !node.assetId) node.assetId = state.level.assets[0]?.id;
    renderAll();
  })));
  if (node.kind === "model") {
    panel.appendChild(field("Asset", selectInput(node.assetId, state.level.assets.map((asset) => ({
      value: asset.id,
      label: asset.name,
    })), (value) => {
      node.assetId = value;
      renderAll();
    })));
  }
  panel.appendChild(field("Position", vectorInput(node.transform.position, (next) => {
    node.transform.position = next;
    renderAll();
  })));
  panel.appendChild(field("Rotation", vectorInput(node.transform.rotation, (next) => {
    node.transform.rotation = next;
    renderAll();
  })));
  panel.appendChild(field("Scale", vectorInput(node.transform.scale, (next) => {
    node.transform.scale = next.map((v) => Math.max(0.01, v));
    renderAll();
  })));
}

function addAssetNode(asset) {
  const parent = selectedNode();
  const parentId = parent?.kind === "group" ? parent.id : (parent?.parentId ?? "root");
  const node = {
    id: uid(asset.id),
    parentId,
    name: asset.name,
    kind: "model",
    assetId: asset.id,
    transform: defaultTransform(),
  };
  const siblings = childrenOf(parentId).length;
  node.transform.position = [(siblings % 5 - 2) * 1.8, asset.previewMesh === "plane" ? 0 : 0.8, Math.floor(siblings / 5) * 1.8];
  if (asset.previewMesh === "plane") node.transform.scale = [4, 1, 4];
  state.level.nodes.push(node);
  state.selectedId = node.id;
  renderAll();
  setStatus(`added ${asset.name}`, "ok");
}

function addGroup() {
  const node = {
    id: uid("group"),
    parentId: selectedNode()?.kind === "group" ? state.selectedId : "root",
    name: "Group",
    kind: "group",
    transform: defaultTransform(),
  };
  state.level.nodes.push(node);
  state.selectedId = node.id;
  renderAll();
}

function duplicateSelected() {
  const node = selectedNode();
  if (!node || node.id === "root") return;
  const copy = structuredClone(node);
  copy.id = uid(node.id);
  copy.name = `${node.name} Copy`;
  copy.transform.position = [
    copy.transform.position[0] + 1,
    copy.transform.position[1],
    copy.transform.position[2] + 1,
  ];
  state.level.nodes.push(copy);
  state.selectedId = copy.id;
  renderAll();
}

function deleteSelected() {
  const node = selectedNode();
  if (!node || node.id === "root") return;
  const remove = new Set([node.id]);
  let changed = true;
  while (changed) {
    changed = false;
    for (const candidate of state.level.nodes) {
      if (candidate.parentId && remove.has(candidate.parentId) && !remove.has(candidate.id)) {
        remove.add(candidate.id);
        changed = true;
      }
    }
  }
  state.level.nodes = state.level.nodes.filter((candidate) => !remove.has(candidate.id));
  state.selectedId = "root";
  renderAll();
}

function exportLevel() {
  const blob = new Blob([serializeLevel()], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = `${state.level.name || "level"}.pictor-level.json`;
  a.click();
  URL.revokeObjectURL(url);
}

async function importLevel(file) {
  const text = await file.text();
  const parsed = JSON.parse(text);
  if (!Array.isArray(parsed.nodes) || !Array.isArray(parsed.assets)) {
    throw new Error("Invalid level JSON");
  }
  state.level = parsed;
  state.selectedId = parsed.nodes[0]?.id ?? null;
  renderAll();
}

function renderJson() {
  $("jsonPreview").textContent = serializeLevel();
}

function renderViewport() {
  viewer.render(state.level, state.selectedId);
  $("nodeMeta").textContent = `${state.level.nodes.length} nodes`;
  $("drawMeta").textContent = `${viewer.draws} draws`;
}

function renderAll() {
  renderHierarchy();
  renderAssets();
  renderInspector();
  renderJson();
  renderViewport();
}

function animate() {
  renderViewport();
  requestAnimationFrame(animate);
}

$("addGroupBtn").addEventListener("click", addGroup);
$("duplicateBtn").addEventListener("click", duplicateSelected);
$("deleteBtn").addEventListener("click", deleteSelected);
$("exportBtn").addEventListener("click", exportLevel);
$("copyBtn").addEventListener("click", async () => {
  await navigator.clipboard.writeText(serializeLevel());
  setStatus("copied JSON", "ok");
});
$("newSceneBtn").addEventListener("click", () => {
  state.level.nodes = [state.level.nodes.find((node) => node.id === "root") ?? {
    id: "root",
    parentId: null,
    name: "Level",
    kind: "group",
    transform: defaultTransform(),
  }];
  state.selectedId = "root";
  renderAll();
});
$("importInput").addEventListener("change", async (event) => {
  const file = event.target.files?.[0];
  if (!file) return;
  try {
    await importLevel(file);
    setStatus(`imported ${file.name}`, "ok");
  } catch (error) {
    setStatus(error.message, "warn");
  }
});
document.querySelectorAll(".segmented button").forEach((button) => {
  button.addEventListener("click", () => {
    state.tool = button.dataset.tool;
    document.querySelectorAll(".segmented button").forEach((b) => b.classList.toggle("active", b === button));
  });
});

renderAll();
requestAnimationFrame(animate);
