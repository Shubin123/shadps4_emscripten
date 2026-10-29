// There's no alert() in Node or in Web Workers (the emulator's main thread runs in one, via
// PROXY_TO_PTHREAD), and SDL's Emscripten backend implements SDL_ShowSimpleMessageBox with it.
if (typeof alert === "undefined") {
  globalThis.alert = (msg) => console.warn("[messagebox] " + msg);
}

// getenv() reads Emscripten's own ENV object. Under Node that already holds the process
// environment; a page passes variables (SHADPS4_*, Paleblood's BB_*) as Module.shadps4Env.
Module["preRun"] = [].concat(Module["preRun"] || [], function () {
  var extra = Module["shadps4Env"];
  for (var key in extra || {}) {
    ENV[key] = String(extra[key]);
  }
});
