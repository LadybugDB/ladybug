const { describe, it, before, after } = require("node:test");
global.describe = describe;
global.it = it;
global.before = before;
global.after = after;

require("./common.js");

const path = require("path");
const fs = require("fs/promises");

const importTest = (name, p) => {
  describe(name, () => {
    require(p);
  });
};

describe("lbug", () => {
  before(() => {
    return initTests();
  });
  after(async () => {
    if (global.conn && !global.conn._isClosed) {
      await global.conn.close().catch(() => {});
    }
    if (global.db && !global.db._isClosed) {
      await global.db.close().catch(() => {});
    }
    if (global.tmpPath) {
      await fs.rm(global.tmpPath, { recursive: true }).catch(() => {});
    }
    // Native addon may keep the event loop alive; force exit so process doesn't hang
    process.exit(0);
  });
  importTest("Database", path.join(__dirname, "test_database.js"));
  importTest("Connection", path.join(__dirname, "test_connection.js"));
  importTest("Query result", path.join(__dirname, "test_query_result.js"));
  importTest("Data types", path.join(__dirname, "test_data_type.js"));
  importTest("Query parameters", path.join(__dirname, "test_parameter.js"));
  importTest("Concurrent query execution", path.join(__dirname, "test_concurrency.js"));
  importTest("Version", path.join(__dirname, "test_version.js"));
  importTest("Synchronous API", path.join(__dirname, "test_sync_api.js"));
  importTest("registerStream / LOAD FROM stream", path.join(__dirname, "test_register_stream.js"));
  importTest("Resilience (close during/after use)", path.join(__dirname, "test_resilience.js"));
});
