/**
 * Copies src_js/*.js, *.mjs, *.d.ts into build/ so tests run with the latest JS
 * after "make nodejs" (which only copies at cmake configure time).
 * Run from tools/nodejs_api.
 */
const fs = require("fs");
const path = require("path");

const srcDir = path.join(__dirname, "src_js");
const buildDir = path.join(__dirname, "build");

if (!fs.existsSync(buildDir)) {
  console.warn("copy_src_to_build: build/ missing, run make nodejs first.");
  process.exit(0);
}

const re = /\.(js|mjs|d\.ts)$/;
const files = fs.readdirSync(srcDir).filter((n) => re.test(n));
for (const name of files) {
  fs.copyFileSync(path.join(srcDir, name), path.join(buildDir, name));
}
console.log("Copied", files.length, "files from src_js to build.");
