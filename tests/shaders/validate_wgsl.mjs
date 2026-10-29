// Compiles every .wgsl file in a directory with Chromium's WebGPU implementation (headless,
// SwiftShader adapter) and reports which ones the browser accepts.
//
//   node tests/shaders/validate_wgsl.mjs <dir>
// Uses the Playwright install from the sibling shadps4_web checkout (or PLAYWRIGHT_DIR).
import { readdirSync, readFileSync, writeFileSync } from "node:fs";
import http from "node:http";
import path from "node:path";

const dir = process.argv[2];
const pwDir = process.env.PLAYWRIGHT_DIR ||
  path.resolve(path.dirname(new URL(import.meta.url).pathname), "../../../shadps4_web/node_modules/playwright");
const { chromium } = await import(path.join(pwDir, "index.mjs"));

const files = readdirSync(dir).filter((f) => f.endsWith(".wgsl")).sort((a, b) => parseInt(a) - parseInt(b));
const server = http.createServer((req, res) => { res.end("<!doctype html><title>wgsl</title>"); });
await new Promise((r) => server.listen(0, r));
const browser = await chromium.launch({ headless: true, args: ["--enable-unsafe-webgpu"] });
const page = await browser.newPage();
await page.goto(`http://localhost:${server.address().port}/`);
const results = await page.evaluate(async (shaders) => {
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) return { error: "no WebGPU adapter" };
  const features = ["subgroups"].filter((f) => adapter.features.has(f));
  const device = await adapter.requestDevice({ requiredFeatures: features });
  const out = [];
  for (const { name, code } of shaders) {
    const module = device.createShaderModule({ code });
    const info = await module.getCompilationInfo();
    const errors = info.messages.filter((m) => m.type === "error");
    out.push({ name, ok: errors.length === 0, errors: errors.slice(0, 3).map((m) => `${m.lineNum}:${m.linePos} ${m.message}`) });
  }
  return { adapter: adapter.info ? `${adapter.info.vendor} ${adapter.info.description}` : "", features, out };
}, files.map((name) => ({ name, code: readFileSync(path.join(dir, name), "utf8") })));
await browser.close();
server.close();
if (results.error) {
  console.log(results.error);
  process.exit(1);
}
const ok = results.out.filter((r) => r.ok).length;
const lines = results.out.map((r) => `${r.ok ? "ok  " : "FAIL"} ${r.name}${r.ok ? "" : "\n      " + r.errors.join("\n      ")}`);
writeFileSync(path.join(dir, "validation.txt"), lines.join("\n") + "\n");
console.log(`WebGPU (${results.adapter}, features: ${results.features.join(",") || "none"}) accepted ${ok}/${results.out.length} WGSL modules`);
for (const r of results.out.filter((r) => !r.ok).slice(0, 8)) {
  console.log(`  FAIL ${r.name}: ${r.errors[0]}`);
}
process.exit(ok === results.out.length ? 0 : 1);
