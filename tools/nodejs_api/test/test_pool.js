require("./common.js");
const path = require("path");
const fsp = require("fs/promises");
const os = require("os");

describe("Connection pool", function () {
  let pool;
  let tmpDir;

  before(async function () {
    await initTests();
    tmpDir = await fsp.mkdtemp(path.join(os.tmpdir(), "lbug-pool-"));
  });

  after(async function () {
    if (tmpDir) await fsp.rm(tmpDir, { recursive: true }).catch(() => {});
  });

  afterEach(async function () {
    if (pool && !pool._closed) {
      await pool.close();
    }
  });

  it("createPool requires maxSize", function () {
    assert.throws(() => lbug.createPool({}), /maxSize/);
    assert.throws(() => lbug.createPool({ databasePath: ":memory:" }), /maxSize/);
    assert.doesNotThrow(() => lbug.createPool({ maxSize: 5 }));
  });

  it("pool.run(fn) runs with a connection and releases on success", async function () {
    pool = lbug.createPool({
      databasePath: path.join(tmpDir, "p1.kz"),
      maxSize: 2,
      databaseOptions: { bufferManagerSize: 1 << 24 },
    });
    const result = await pool.run(async (conn) => {
      const r = await conn.query("RETURN 1 AS x");
      const rows = await r.getAll();
      r.close();
      return rows;
    });
    assert.lengthOf(result, 1);
    assert.strictEqual(result[0].x, 1);
  });

  it("pool.run(fn) releases on throw", async function () {
    pool = lbug.createPool({
      databasePath: path.join(tmpDir, "p2.kz"),
      maxSize: 2,
      databaseOptions: { bufferManagerSize: 1 << 24 },
    });
    let err;
    try {
      await pool.run(async () => {
        throw new Error("fail");
      });
    } catch (e) {
      err = e;
    }
    assert.instanceOf(err, Error);
    assert.include(err.message, "fail");
    const again = await pool.run(async (conn) => {
      const r = await conn.query("RETURN 2 AS y");
      const rows = await r.getAll();
      r.close();
      return rows;
    });
    assert.lengthOf(again, 1);
    assert.strictEqual(again[0].y, 2);
  });

  it("acquire/release and multiple concurrent cycles", async function () {
    pool = lbug.createPool({
      databasePath: path.join(tmpDir, "p3.kz"),
      maxSize: 3,
      databaseOptions: { bufferManagerSize: 1 << 24 },
    });
    const conn1 = await pool.acquire();
    const conn2 = await pool.acquire();
    const conn3 = await pool.acquire();
    const r1 = await conn1.query("RETURN 1 AS a");
    const r2 = await conn2.query("RETURN 2 AS b");
    const r3 = await conn3.query("RETURN 3 AS c");
    assert.strictEqual((await r1.getAll())[0].a, 1);
    assert.strictEqual((await r2.getAll())[0].b, 2);
    assert.strictEqual((await r3.getAll())[0].c, 3);
    r1.close();
    r2.close();
    r3.close();
    pool.release(conn1);
    pool.release(conn2);
    pool.release(conn3);
    const conn4 = await pool.acquire();
    const r4 = await conn4.query("RETURN 4 AS d");
    assert.strictEqual((await r4.getAll())[0].d, 4);
    r4.close();
    pool.release(conn4);
  });

  it("pool does not exceed maxSize", async function () {
    pool = lbug.createPool({
      databasePath: path.join(tmpDir, "p4.kz"),
      maxSize: 2,
      databaseOptions: { bufferManagerSize: 1 << 24 },
    });
    const c1 = await pool.acquire();
    const c2 = await pool.acquire();
    let resolved = false;
    const p3 = pool.acquire().then((c) => {
      resolved = true;
      pool.release(c);
    });
    await new Promise((r) => setImmediate(r));
    assert.isFalse(resolved);
    pool.release(c1);
    await p3;
    assert.isTrue(resolved);
    pool.release(c2);
  });

  it("acquire() rejects after acquireTimeoutMillis when no connection available", async function () {
    pool = lbug.createPool({
      databasePath: path.join(tmpDir, "p5a.kz"),
      maxSize: 1,
      acquireTimeoutMillis: 80,
      databaseOptions: { bufferManagerSize: 1 << 24 },
    });
    const c1 = await pool.acquire();
    let timeoutErr;
    try {
      await pool.acquire();
    } catch (e) {
      timeoutErr = e;
    }
    assert.instanceOf(timeoutErr, Error);
    assert.include(timeoutErr.message, "timed out");
    pool.release(c1);
  });

  it("pool.close() prevents new acquire and closes all", async function () {
    pool = lbug.createPool({
      databasePath: path.join(tmpDir, "p5.kz"),
      maxSize: 2,
      databaseOptions: { bufferManagerSize: 1 << 24 },
    });
    await pool.run(async (conn) => {
      const r = await conn.query("RETURN 1");
      r.close();
    });
    await pool.close();
    let closedErr;
    try {
      await pool.acquire();
    } catch (e) {
      closedErr = e;
    }
    assert.instanceOf(closedErr, Error);
    assert.include(closedErr.message, "closed");
  });
});
