const os = require("os");
const childProcess = require("child_process");
const path = require("path");
const fs = require("fs");
const fsCallback = require("fs");
const process = require("process");

const isNpmBuildFromSourceSet = process.env.npm_config_build_from_source;
const platform = process.platform;
const arch = process.arch;

// Skip when already built (e.g. local dev after make nodejs)
if (fsCallback.existsSync(path.join(__dirname, "build", "lbugjs.node"))) {
  process.exit(0);
}

const prebuiltPath = path.join(
  __dirname,
  "prebuilt",
  `lbugjs-${platform}-${arch}.node`
);

const buildDir = path.join(__dirname, "build");
const srcJsDir = path.join(__dirname, "src_js");
const lbugSourceDir = path.join(__dirname, "lbug-source");

// Check if building from source is forced
if (isNpmBuildFromSourceSet) {
  console.log(
    "The NPM_CONFIG_BUILD_FROM_SOURCE environment variable is set. Building from source."
  );
}
// Prebuilt available + git-clone layout (src_js present, no lbug-source): use prebuilt and copy src_js → build/
else if (fsCallback.existsSync(prebuiltPath) && fsCallback.existsSync(srcJsDir)) {
  console.log("Prebuilt binary is available (git clone layout).");
  if (!fsCallback.existsSync(buildDir)) {
    fsCallback.mkdirSync(buildDir, { recursive: true });
  }
  fs.copyFileSync(prebuiltPath, path.join(buildDir, "lbugjs.node"));
  const jsFiles = fs.readdirSync(srcJsDir).filter((file) => {
    return file.endsWith(".js") || file.endsWith(".mjs") || file.endsWith(".d.ts");
  });
  for (const file of jsFiles) {
    fs.copyFileSync(path.join(srcJsDir, file), path.join(buildDir, file));
  }
  console.log("Done! Prebuilt + JS copied to build/.");
  process.exit(0);
}
// Prebuilt available + tarball layout (lbug-source present): copy to root (legacy publish flow)
else if (fsCallback.existsSync(prebuiltPath)) {
  console.log("Prebuilt binary is available.");
  fs.copyFileSync(prebuiltPath, path.join(__dirname, "lbugjs.node"));
  const jsSourceDir = path.join(lbugSourceDir, "tools", "nodejs_api", "src_js");
  const jsFiles = fs.readdirSync(jsSourceDir).filter((file) => {
    return file.endsWith(".js") || file.endsWith(".mjs") || file.endsWith(".d.ts");
  });
  for (const file of jsFiles) {
    fs.copyFileSync(path.join(jsSourceDir, file), path.join(__dirname, file));
  }
  console.log("Done!");
  process.exit(0);
} else {
  console.log("Prebuilt binary is not available, building from source...");
}

if (!fsCallback.existsSync(lbugSourceDir)) {
  console.error(
    "lbug-source/ not found (install from git clone). Add prebuilt binary to prebuilt/lbugjs-" +
      platform +
      "-" +
      arch +
      ".node and commit, or install from a full clone and build there."
  );
  process.exit(1);
}

// Get number of threads
const THREADS = os.cpus().length;
console.log(`Using ${THREADS} threads to build Lbug.`);

// Install dependencies only; skip install script so nested install.js does not run (no lbug-source there).
console.log("Installing dependencies...");
const innerNpmEnv = { ...process.env, NPM_CONFIG_IGNORE_SCRIPTS: "true" };
childProcess.execSync("npm install --ignore-scripts", {
  cwd: path.join(__dirname, "lbug-source", "tools", "nodejs_api"),
  stdio: "inherit",
  env: innerNpmEnv,
});

// Build the Lbug source code
console.log("Building Lbug source code...");
const env = { ...process.env };

if (process.platform === "darwin") {
  const archflags = process.env["ARCHFLAGS"]
    ? process.env["ARCHFLAGS"] === "-arch arm64"
      ? "arm64"
      : process.env["ARCHFLAGS"] === "-arch x86_64"
        ? "x86_64"
        : null
    : null;
  if (archflags) {
    console.log(`The ARCHFLAGS is set to '${archflags}'.`);
    env["CMAKE_OSX_ARCHITECTURES"] = archflags;
  } else {
    console.log("The ARCHFLAGS is not set or is invalid and will be ignored.");
  }

  const deploymentTarget = process.env["MACOSX_DEPLOYMENT_TARGET"];
  if (deploymentTarget) {
    console.log(
      `The MACOSX_DEPLOYMENT_TARGET is set to '${deploymentTarget}'.`
    );
    env["CMAKE_OSX_DEPLOYMENT_TARGET"] = deploymentTarget;
  } else {
    console.log("The MACOSX_DEPLOYMENT_TARGET is not set and will be ignored.");
  }
}

if (process.platform === "win32") {
  // The `rc` package conflicts with the rc command (resource compiler) on
  // Windows. This causes the build to fail. This is a workaround which removes
  // all the environment variables added by npm.
  const pathEnv = process.env["Path"];
  const pathSplit = pathEnv.split(";").filter((path) => {
    const pathLower = path.toLowerCase();
    return !pathLower.includes("node_modules");
  });
  env["Path"] = pathSplit.join(";");
  console.log(
    "The PATH environment variable has been modified to remove any 'node_modules' directories."
  );

  for (let key in env) {
    if (
      key.toLowerCase().includes("node" || key.toLowerCase().includes("npm"))
    ) {
      delete env[key];
    }
  }
  console.log(
    "Any environment variables containing 'node' or 'npm' have been removed."
  );
}

childProcess.execSync("make nodejs NUM_THREADS=" + THREADS, {
  env,
  cwd: path.join(__dirname, "lbug-source"),
  stdio: "inherit",
});

// Copy the built files to the package directory
const BUILT_DIR = path.join(
  __dirname,
  "lbug-source",
  "tools",
  "nodejs_api",
  "build"
);
// Get all the js and node files
const files = fs.readdirSync(BUILT_DIR).filter((file) => {
  return file.endsWith(".js") || file.endsWith(".mjs") || file.endsWith(".d.ts") || file.endsWith(".node");
});
console.log("Files to copy: ");
for (const file of files) {
  console.log("  " + file);
}
console.log("Copying built files to package directory...");
for (const file of files) {
  fs.copyFileSync(path.join(BUILT_DIR, file), path.join(__dirname, file));
}

// Clean up
console.log("Cleaning up...");
childProcess.execSync("npm run clean-all", {
  cwd: path.join(__dirname, "lbug-source", "tools", "nodejs_api"),
});
childProcess.execSync("make clean", {
  cwd: path.join(__dirname, "lbug-source"),
});
console.log("Done!");
