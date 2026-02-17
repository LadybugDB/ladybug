global.chai = require("chai");
global.assert = chai.assert;
global.expect = chai.expect;
chai.should();
chai.config.includeStack = true;

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

const tmp = require("tmp");
const fs = require("fs/promises");
const path = require("path");
const initTests = async () => {
  const tmpPath = await new Promise((resolve, reject) => {
    tmp.dir({ unsafeCleanup: true }, (err, path, _) => {
      if (err) {
        return reject(err);
      }
      return resolve(path);
    });
  });

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
  const dataFileRegex = new RegExp(`"([^"]+\\.(${dataFileExtension.join('|')}))"`, "gi");

  for (const line of copy) {
    if (!line || line.trim().length === 0) {
        continue;
    }

    // handle multiple data files in one line
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
  global.db = db;
  global.conn = conn;
};

global.initTests = initTests;
