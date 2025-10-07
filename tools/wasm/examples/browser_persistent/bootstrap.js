const fs = require("fs");
const path = require("path");
const process = require("process");

const LBUG_WASM_INDEX_PATH = path.join(__dirname, "node_modules", "lbug-wasm", "index.js");
const LBUG_WASM_WORKER_PATH = path.join(__dirname, "node_modules", "lbug-wasm", "lbug_wasm_worker.js");
const DESTINATION_PATH = path.join(__dirname, "public");

if (!fs.existsSync(LBUG_WASM_INDEX_PATH) || !fs.existsSync(LBUG_WASM_WORKER_PATH)) {
    console.log("Lbug WebAssembly module not found. Please run `npm i` to install the dependencies.");
    process.exit(1);
}

console.log("Copying Lbug WebAssembly module to public directory...");
console.log(`Copying ${LBUG_WASM_INDEX_PATH} to ${DESTINATION_PATH}...`);
fs.copyFileSync(LBUG_WASM_INDEX_PATH, path.join(DESTINATION_PATH, "index.js"));
console.log(`Copying ${LBUG_WASM_WORKER_PATH} to ${DESTINATION_PATH}...`);
fs.copyFileSync(LBUG_WASM_WORKER_PATH, path.join(DESTINATION_PATH, "lbug_wasm_worker.js"));
console.log("Done.");
