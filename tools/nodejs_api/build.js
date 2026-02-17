const fs = require("fs");
const path = require("path");
const { execSync } = require("child_process");

const SRC_PATH = path.resolve(__dirname, "../..");
const NODEJS_API = path.resolve(__dirname, ".");
const BUILD_DIR = path.join(NODEJS_API, "build");
const SRC_JS_DIR = path.join(NODEJS_API, "src_js");
const THREADS = require("os").cpus().length;

console.log(`Using ${THREADS} threads to build Lbug.`);

execSync("npm run clean", { stdio: "inherit" });
execSync(`make nodejs NUM_THREADS=${THREADS}`, {
  cwd: SRC_PATH,
  stdio: "inherit",
});

// Ensure build/ has latest JS from src_js (CMake copies at configure time only)
if (fs.existsSync(SRC_JS_DIR) && fs.existsSync(BUILD_DIR)) {
  const files = fs.readdirSync(SRC_JS_DIR);
  for (const name of files) {
    if (name.endsWith(".js") || name.endsWith(".mjs") || name.endsWith(".d.ts")) {
      fs.copyFileSync(path.join(SRC_JS_DIR, name), path.join(BUILD_DIR, name));
    }
  }
  // So package root has types when used as file: dependency
  const dts = path.join(BUILD_DIR, "lbug.d.ts");
  if (fs.existsSync(dts)) {
    fs.copyFileSync(dts, path.join(NODEJS_API, "lbug.d.ts"));
  }
  console.log("Copied src_js to build.");
}
