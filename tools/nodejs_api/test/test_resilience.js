"use strict";

const { assert } = require("chai");
const tmp = require("tmp");
const path = require("path");

/**
 * Resilience tests: close connection/database during or after operations.
 * Goal: no crashes (SIGSEGV, native abort); all failures must surface as JS errors.
 */
function withTempDb(fn) {
  return async function () {
    const tmpPath = await new Promise((resolve, reject) => {
      tmp.dir({ unsafeCleanup: true }, (err, p, _) => {
        if (err) return reject(err);
        return resolve(p);
      });
    });
    const dbPath = path.join(tmpPath, "db.kz");
    const testDb = new lbug.Database(dbPath, 1 << 26 /* 64MB */);
    await testDb.init();
    const testConn = new lbug.Connection(testDb);
    await testConn.init();
    try {
      await fn.call(this, testDb, testConn);
    } finally {
      if (!testDb._isClosed) await testDb.close().catch(() => {});
      if (!testConn._isClosed) await testConn.close().catch(() => {});
    }
  };
}

describe("Resilience (close during/after use)", function () {
  this.timeout(10000);

  it("query rejects when connection is closed while query is in flight", withTempDb(async (testDb, testConn) => {
    const longQuery = "UNWIND range(1, 20000) AS x UNWIND range(1, 2000) AS y RETURN count(*)";
    const queryPromise = testConn.query(longQuery);
    await new Promise((r) => setTimeout(r, 80));
    testConn.closeSync();
    const timeoutMs = 2000;
    const timeoutPromise = new Promise((_, reject) => {
      setTimeout(() => reject(new Error(`Expected query to reject within ${timeoutMs}ms when connection was closed (timed out).`)), timeoutMs);
    });
    try {
      await Promise.race([queryPromise, timeoutPromise]);
      assert.fail("Expected query to reject when connection was closed during execution.");
    } catch (err) {
      if ((err.message || "").includes("timed out")) throw err;
      assert.instanceOf(err, Error);
      const msg = (err.message || "").toLowerCase();
      const ok = msg.includes("closed") || msg.includes("not allowed") || msg.includes("runtime");
      assert.isTrue(ok, `Expected error about closed/not allowed, got: ${err.message}`);
    }
  }));

  // Database close is synchronous and blocks until in-flight work completes (core behavior).
  // So we cannot observe "query rejects when database is closed" without a non-blocking close.
  it.skip("query rejects when database is closed while query is in flight", withTempDb(async (testDb, testConn) => {
    const longQuery = "UNWIND range(1, 20000) AS x UNWIND range(1, 2000) AS y RETURN count(*)";
    const queryPromise = testConn.query(longQuery);
    await new Promise((r) => setTimeout(r, 120));
    testDb.closeSync();
    const timeoutMs = 5000;
    const timeoutPromise = new Promise((_, reject) => {
      setTimeout(() => reject(new Error(`Expected query to reject within ${timeoutMs}ms when database was closed (timed out).`)), timeoutMs);
    });
    try {
      await Promise.race([queryPromise, timeoutPromise]);
      assert.fail("Expected query to reject when database was closed during execution.");
    } catch (err) {
      if ((err.message || "").includes("timed out")) throw err;
      assert.instanceOf(err, Error);
      const msg = (err.message || "").toLowerCase();
      const ok = msg.includes("closed") || msg.includes("not allowed") || msg.includes("runtime");
      assert.isTrue(ok, `Expected error about closed/not allowed, got: ${err.message}`);
    }
  }));

  it("getNext() after connection closed throws and does not crash", withTempDb(async (testDb, testConn) => {
    const res = await testConn.query("RETURN 1 AS x");
    const row = await res.getNext();
    assert.equal(row.x, 1);
    testConn.closeSync();
    try {
      await res.getNext();
      assert.fail("Expected getNext() to throw after connection closed.");
    } catch (err) {
      assert.instanceOf(err, Error);
      const msg = (err.message || "").toLowerCase();
      assert.isTrue(msg.includes("closed") || msg.includes("not allowed"), `Expected closed/not allowed, got: ${err.message}`);
    }
  }));

  it("hasNext() after connection closed throws and does not crash", withTempDb(async (testDb, testConn) => {
    const res = await testConn.query("RETURN 1 AS x");
    assert.isTrue(res.hasNext());
    testConn.closeSync();
    try {
      res.hasNext();
      assert.fail("Expected hasNext() to throw after connection closed.");
    } catch (err) {
      assert.instanceOf(err, Error);
      const msg = (err.message || "").toLowerCase();
      assert.isTrue(msg.includes("closed") || msg.includes("not allowed"), `Expected closed/not allowed, got: ${err.message}`);
    }
  }));

  it("getNext() after database closed throws and does not crash", withTempDb(async (testDb, testConn) => {
    const res = await testConn.query("RETURN 1 AS x");
    await res.getNext();
    testDb.closeSync();
    try {
      await res.getNext();
      assert.fail("Expected getNext() to throw after database closed.");
    } catch (err) {
      assert.instanceOf(err, Error);
      const msg = (err.message || "").toLowerCase();
      assert.isTrue(msg.includes("closed") || msg.includes("not allowed"), `Expected closed/not allowed, got: ${err.message}`);
    }
  }));

  it("hasNext() after database closed throws and does not crash", withTempDb(async (testDb, testConn) => {
    const res = await testConn.query("RETURN 1 AS x");
    testDb.closeSync();
    try {
      res.hasNext();
      assert.fail("Expected hasNext() to throw after database closed.");
    } catch (err) {
      assert.instanceOf(err, Error);
      const msg = (err.message || "").toLowerCase();
      assert.isTrue(msg.includes("closed") || msg.includes("not allowed"), `Expected closed/not allowed, got: ${err.message}`);
    }
  }));

  it("registerStream then close connection then query throws before running", withTempDb(async (testDb, testConn) => {
    async function* gen() {
      yield [1];
    }
    await testConn.registerStream("s", gen(), { columns: [{ name: "x", type: "INT64" }] });
    testConn.closeSync();
    try {
      await testConn.query("LOAD FROM s RETURN *");
      assert.fail("Expected query to throw when connection is already closed.");
    } catch (err) {
      assert.instanceOf(err, Error);
      assert.include((err.message || "").toLowerCase(), "closed");
    }
  }));

  it("close connection while iterating result: second getNext throws", withTempDb(async (testDb, testConn) => {
    const res = await testConn.query("UNWIND [1,2,3] AS x RETURN x");
    const a = await res.getNext();
    assert.equal(a.x, 1);
    testConn.closeSync();
    try {
      await res.getNext();
      assert.fail("Expected getNext() to throw after connection closed mid-iteration.");
    } catch (err) {
      assert.instanceOf(err, Error);
      const msg = (err.message || "").toLowerCase();
      assert.isTrue(msg.includes("closed") || msg.includes("not allowed"), `Expected closed/not allowed, got: ${err.message}`);
    }
  }));

  it("query after connection closed throws immediately (no native call)", async function () {
    const testConn = new lbug.Connection(db);
    await testConn.init();
    await testConn.close();
    try {
      await testConn.query("RETURN 1");
      assert.fail("Expected query to throw when connection is closed.");
    } catch (err) {
      assert.equal(err.message, "Connection is closed.");
    }
  });

  it("getNextSync after database closed throws", withTempDb(async (testDb, testConn) => {
    const res = await testConn.query("RETURN 1 AS x");
    testDb.closeSync();
    try {
      res.getNextSync();
      assert.fail("Expected getNextSync() to throw after database closed.");
    } catch (err) {
      assert.instanceOf(err, Error);
      const msg = (err.message || "").toLowerCase();
      assert.isTrue(msg.includes("closed") || msg.includes("not allowed"), `Expected closed/not allowed, got: ${err.message}`);
    }
  }));
});
