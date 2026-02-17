require("./common.js");

const importTest = (name, path) => {
  describe(name, () => {
    require(path);
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
  });
  importTest("Database", "./test_database.js");
  importTest("Connection", "./test_connection.js");
  importTest("Query result", "./test_query_result.js");
  importTest("Data types", "./test_data_type.js");
  importTest("Query parameters", "./test_parameter.js");
  importTest("Concurrent query execution", "./test_concurrency.js");
  importTest("Version", "./test_version.js");
  importTest("Synchronous API", "./test_sync_api.js");
  importTest("registerStream / LOAD FROM stream", "./test_register_stream.js");
  importTest("Resilience (close during/after use)", "./test_resilience.js");
});
