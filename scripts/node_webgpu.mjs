// Gives Node a WebGPU implementation - Dawn's, from the `webgpu` package - for the Node build
// (build/wasm/shadps4_node.js), which then renders as the browser build does. Loaded with
// `node --import`; only the main thread needs it (the device lives there, see
// emscripten/webgpu.js). The package is found at $NODE_WEBGPU_DIR, else in the sibling
// shadps4_web checkout's node_modules (`npm install` there).
import { existsSync } from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { isMainThread } from "node:worker_threads";

if (isMainThread) {
  const here = path.dirname(new URL(import.meta.url).pathname);
  const dir = process.env.NODE_WEBGPU_DIR ||
    path.resolve(here, "../../shadps4_web/node_modules/webgpu");
  if (existsSync(path.join(dir, "index.js"))) {
    const { create, globals } = await import(pathToFileURL(path.join(dir, "index.js")).href);
    Object.assign(globalThis, globals);
    Object.defineProperty(globalThis.navigator, "gpu", { value: create([]), configurable: true });
  } else {
    console.warn(`[node_webgpu] no webgpu package at ${dir}: running without a GPU`);
  }
}
