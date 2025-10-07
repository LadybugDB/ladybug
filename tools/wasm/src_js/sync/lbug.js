/**
 * @file lbug.js is the internal wrapper for the WebAssembly module.
 */
const lbug_wasm = require("../lbug/lbug_wasm.js");

class lbug {
  constructor() {
    this._lbug = null;
  }

  async init() {
    this._lbug = await lbug_wasm();
  }

  checkInit() {
    if (!this._lbug) {
      throw new Error("The WebAssembly module is not initialized.");
    }
  }

  getVersion() {
    this.checkInit();
    return this._lbug.getVersion();
  }

  getStorageVersion() {
    this.checkInit();
    return this._lbug.getStorageVersion();
  }

  getFS() {
    this.checkInit();
    return this._lbug.FS;
  }

  getWasmMemory() {
    this.checkInit();
    return this._lbug.wasmMemory;
  }
}

const lbugInstance = new lbug();
module.exports = lbugInstance;
