import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { dirname, extname, join, normalize, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = dirname(fileURLToPath(import.meta.url));
const rootPrefix = normalize(root) + sep;
const port = Number(process.env.PORT || 8781);
const types = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
};

createServer(async (req, res) => {
  const url = new URL(req.url || "/", `http://localhost:${port}`);
  const pathname = url.pathname === "/" ? "/index.html" : url.pathname;
  const file = normalize(join(root, pathname));
  // パストラバーサル防御: 末尾セパレータ付き接頭辞で照合し、root を接頭辞に持つ
  // 兄弟ディレクトリ (例: <root>-secret) を弾く。 root 直下のファイルも許可。
  if (file !== normalize(root) && !file.startsWith(rootPrefix)) {
    res.writeHead(403);
    res.end("Forbidden");
    return;
  }
  try {
    const body = await readFile(file);
    res.writeHead(200, { "content-type": types[extname(file)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404);
    res.end("Not found");
  }
}).listen(port, "127.0.0.1", () => {
  // ローカル開発ツールを LAN に晒さないよう loopback のみに bind。
  console.log(`Pictor Level Editor: http://localhost:${port}/`);
});
