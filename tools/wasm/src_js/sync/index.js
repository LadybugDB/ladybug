/**
 * @file index.js is the root file for the synchronous version of Lbug
 * WebAssembly module. It exports the module's public interface.
 */
"use strict";

const LbugWasm = require("./lbug.js");
const Database = require("./database.js");
const Connection = require("./connection.js");
const PreparedStatement = require("./prepared_statement.js");
const QueryResult = require("./query_result.js");

/**
 * The synchronous version of Lbug WebAssembly module.
 * @module lbug-wasm
 */
module.exports = {
  /**
   * Initialize the Lbug WebAssembly module.
   * @memberof module:lbug-wasm
   * @returns {Promise<void>} a promise that resolves when the module is 
   * initialized. The promise is rejected if the module fails to initialize.
   */
  init: () => {
    return LbugWasm.init();
  },

  /**
   * Get the version of the Lbug WebAssembly module.
   * @memberof module:lbug-wasm
   * @returns {String} the version of the Lbug WebAssembly module.
   */
  getVersion: () => {
    return LbugWasm.getVersion();
  },

  /**
   * Get the storage version of the Lbug WebAssembly module.
   * @memberof module:lbug-wasm
   * @returns {BigInt} the storage version of the Lbug WebAssembly module.
   */
  getStorageVersion: () => {
    return LbugWasm.getStorageVersion();
  },
  
  /**
   * Get the standard emscripten filesystem module (FS). Please refer to the 
   * emscripten documentation for more information.
   * @memberof module:lbug-wasm
   * @returns {Object} the standard emscripten filesystem module (FS).
   */
  getFS: () => {
    return LbugWasm.getFS();
  },

  /**
   * Get the WebAssembly memory. Please refer to the emscripten documentation 
   * for more information.
   * @memberof module:lbug-wasm
   * @returns {Object} the WebAssembly memory object.
   */
  getWasmMemory: () => {
    return LbugWasm.getWasmMemory();
  },

  Database,
  Connection,
  PreparedStatement,
  QueryResult,
};
