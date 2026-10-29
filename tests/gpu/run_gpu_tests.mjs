// Runs build/wasm/gpu_tests (tests/gpu/compute_test.cpp) in headless Chromium with WebGPU and
// reports its result. The page is served cross-origin isolated, as pthreads need.
//
//   node tests/gpu/run_gpu_tests.mjs [build dir]      (DUMP_SHADERS=1 logs the WGSL)
// Uses the Playwright install from the sibling shadps4_web checkout (or PLAYWRIGHT_DIR).
import { readFileSync } from "node:fs";
import http from "node:http";
import path from "node:path";

const here = path.dirname(new URL(import.meta.url).pathname);
const buildDir = path.resolve(process.argv[2] || path.join(here, "../../build/wasm"));
const pwDir = process.env.PLAYWRIGHT_DIR ||
  path.resolve(here, "../../../shadps4_web/node_modules/playwright");
const { chromium } = await import(path.join(pwDir, "index.mjs"));

const page_html = `<!doctype html><title>gpu tests</title><script>
var Module = {
  print: (s) => console.log(s),
  printErr: (s) => console.log(s),
  shadps4GpuDumpShaders: ${process.env.DUMP_SHADERS ? "true" : "false"},
  shadps4Env: { SHADPS4_GPU_TEST_ENV: "page" },
};
</script><script src="gpu_tests.js"></script>`;

const types = { ".js": "text/javascript", ".wasm": "application/wasm" };
const server = http.createServer((req, res) => {
  const headers = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
  };
  const name = new URL(req.url, "http://x").pathname.slice(1);
  if (name === "" || name === "index.html") {
    res.writeHead(200, { ...headers, "Content-Type": "text/html" });
    res.end(page_html);
    return;
  }
  try {
    const data = readFileSync(path.join(buildDir, path.basename(name)));
    res.writeHead(200, { ...headers, "Content-Type": types[path.extname(name)] || "application/octet-stream" });
    res.end(data);
  } catch {
    res.writeHead(404, headers);
    res.end();
  }
});
await new Promise((r) => server.listen(0, r));

const browser = await chromium.launch({ headless: true, args: ["--enable-unsafe-webgpu"] });
const page = await browser.newPage();
let verdict = null;
const done = new Promise((resolve) => {
  page.on("console", (msg) => {
    const text = msg.text();
    console.log(text);
    const m = text.match(/^GPU TESTS: (.*)$/);
    if (m) {
      verdict = m[1];
      resolve();
    }
  });
  page.on("pageerror", (e) => {
    console.log("[pageerror] " + e.message);
    resolve();
  });
});
await page.goto(`http://localhost:${server.address().port}/`);
const timeout = new Promise((resolve) => setTimeout(resolve, 180000));
await Promise.race([done, timeout]);
await browser.close();
server.close();
if (verdict === null) {
  console.log("GPU TESTS: no result (crash or timeout)");
}
process.exit(verdict === "PASS" ? 0 : 1);
