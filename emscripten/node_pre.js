// Node has no window.alert; SDL's Emscripten backend uses it for SDL_ShowSimpleMessageBox.
if (typeof alert === "undefined") {
  globalThis.alert = (msg) => console.warn("[messagebox] " + msg);
}
