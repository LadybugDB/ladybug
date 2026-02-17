const nodeAssert = require("node:assert");
const fs = require("fs/promises");
const path = require("path");
const os = require("os");

// Chai-like API on top of node:assert for minimal test changes
const assert = Object.create(nodeAssert);
assert.exists = (val, msg) => nodeAssert.ok(val != null, msg || "expected value to exist");
assert.notExists = (val, msg) => nodeAssert.ok(val == null, msg || "expected value to not exist");
assert.isNull = (val, msg) => nodeAssert.strictEqual(val, null, msg);
assert.isNotNull = (val, msg) => nodeAssert.notStrictEqual(val, null, msg);
assert.isTrue = (val, msg) => nodeAssert.strictEqual(val, true, msg);
assert.isFalse = (val, msg) => nodeAssert.strictEqual(val, false, msg);
assert.include = (container, value, msg) =>
  nodeAssert.ok(container.includes(value), msg || `expected ${container} to include ${value}`);
assert.isEmpty = (val, msg) =>
  nodeAssert.strictEqual(val.length, 0, msg || `expected empty, got length ${val.length}`);
assert.instanceOf = (obj, Ctor, msg) =>
  nodeAssert.ok(obj instanceof Ctor, msg || `expected instance of ${Ctor.name}`);
assert.isNumber = (val, msg) =>
  nodeAssert.strictEqual(typeof val, "number", msg);
assert.isString = (val, msg) =>
  nodeAssert.strictEqual(typeof val, "string", msg);
assert.isAtLeast = (n, min, msg) =>
  nodeAssert.ok(n >= min, msg || `expected ${n} >= ${min}`);
assert.lengthOf = (arr, n, msg) =>
  nodeAssert.strictEqual(arr.length, n, msg || `expected length ${n}, got ${arr.length}`);
assert.equal = (a, b, msg) => nodeAssert.strictEqual(a, b, msg);
assert.notEqual = (a, b, msg) => nodeAssert.notStrictEqual(a, b, msg);
assert.deepEqual = (a, b, msg) => nodeAssert.deepStrictEqual(a, b, msg);
assert.approximately = (actual, expected, delta, msg) =>
  nodeAssert.ok(
    Math.abs(actual - expected) <= delta,
    msg || `expected ${actual} to be approximately ${expected} (±${delta})`
  );
global.assert = assert;

const TEST_INSTALLED = process.env.TEST_INSTALLED || false;
if (TEST_INSTALLED) {
  global.lbug = require("lbug");
  global.lbugPath = require.resolve("lbug");
  console.log("Testing installed version @", lbugPath);
} else {
  global.lbug = require("../build/");
  global.lbugPath = require.resolve("../build/");
  console.log("Testing locally built version @", lbugPath);
}

// Temp dir: os.tmpdir() respects TMPDIR (Unix) and TEMP/TMP (Windows). XDG spec
// does not define a temp directory; industry practice is TMPDIR + mkdtemp (unique names).
const initTests = async () => {
  const tmpPath = await fs.mkdtemp(path.join(os.tmpdir(), "lbug-"));
  const dbPath = path.join(tmpPath, "db.kz");
  const db = new lbug.Database(dbPath, 1 << 28 /* 256MB */);
  const conn = new lbug.Connection(db, 4);
  const tinysnbDir = "../../dataset/tinysnb/";

  const schema = (await fs.readFile(tinysnbDir + "schema.cypher"))
    .toString()
    .split("\n");
  for (const line of schema) {
    if (line.trim().length === 0) {
      continue;
    }
    await conn.query(line);
  }

  const copy = (await fs.readFile(tinysnbDir + "copy.cypher"))
    .toString()
    .split("\n");

  const dataFileExtension = ["csv", "parquet", "npy", "ttl", "nq", "json", "lbug_extension"];
  const dataFileRegex = new RegExp(`"([^"]+\\.(${dataFileExtension.join("|")}))"`, "gi");

  for (const line of copy) {
    if (!line || line.trim().length === 0) {
      continue;
    }
    const statement = line.replace(dataFileRegex, `"${tinysnbDir}$1"`);
    await conn.query(statement);
  }

  await conn.query(
    "create node table moviesSerial (ID SERIAL, name STRING, length INT32, note STRING, PRIMARY KEY (ID))"
  );
  await conn.query(
    'copy moviesSerial from "../../dataset/tinysnb-serial/vMovies.csv"'
  );

  global.dbPath = dbPath;
  global.tmpPath = tmpPath;
  global.db = db;
  global.conn = conn;
};

global.initTests = initTests;
