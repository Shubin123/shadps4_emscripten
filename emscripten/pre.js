// There's no alert() in Node or in Web Workers (the emulator's main thread runs in one, via
// PROXY_TO_PTHREAD), and SDL's Emscripten backend implements SDL_ShowSimpleMessageBox with it.
if (typeof alert === "undefined") {
  globalThis.alert = (msg) => console.warn("[messagebox] " + msg);
}
