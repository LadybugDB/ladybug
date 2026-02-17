"use strict";

const Database = require("./database.js");
const Connection = require("./connection.js");

const DEFAULT_MIN_SIZE = 0;
const DEFAULT_ACQUIRE_TIMEOUT_MILLIS = 0;
const DEFAULT_VALIDATE_ON_ACQUIRE = false;

function createDatabase(path, databaseOptions) {
  const o = databaseOptions || {};
  return new Database(
    path,
    o.bufferManagerSize ?? 0,
    o.enableCompression ?? true,
    o.readOnly ?? false,
    o.maxDBSize ?? 0,
    o.autoCheckpoint ?? true,
    o.checkpointThreshold ?? -1,
    o.throwOnWalReplayFailure ?? true,
    o.enableChecksums ?? true,
    o.openLockRetryMs ?? 5000
  );
}

class Pool {
  constructor(options) {
    if (options == null || typeof options !== "object") {
      throw new Error("createPool(options): options must be an object.");
    }
    const path = options.databasePath;
    if (path !== undefined && path !== null && path !== "" && typeof path !== "string") {
      throw new Error("createPool: databasePath must be a string or empty.");
    }
    const maxSize = options.maxSize;
    if (typeof maxSize !== "number" || maxSize < 1 || !Number.isInteger(maxSize)) {
      throw new Error("createPool: maxSize must be a positive integer.");
    }
    const minSize = options.minSize ?? DEFAULT_MIN_SIZE;
    if (typeof minSize !== "number" || minSize < 0 || !Number.isInteger(minSize) || minSize > maxSize) {
      throw new Error("createPool: minSize must be a non-negative integer not greater than maxSize.");
    }
    const acquireTimeoutMillis = options.acquireTimeoutMillis ?? DEFAULT_ACQUIRE_TIMEOUT_MILLIS;
    if (typeof acquireTimeoutMillis !== "number" || acquireTimeoutMillis < 0) {
      throw new Error("createPool: acquireTimeoutMillis must be a non-negative number.");
    }
    const validateOnAcquire = options.validateOnAcquire ?? DEFAULT_VALIDATE_ON_ACQUIRE;

    this._databasePath = path == null || path === "" ? ":memory:" : path;
    this._databaseOptions = options.databaseOptions || null;
    this._maxSize = maxSize;
    this._minSize = minSize;
    this._acquireTimeoutMillis = acquireTimeoutMillis;
    this._validateOnAcquire = Boolean(validateOnAcquire);

    this._database = null;
    this._idle = [];
    this._allConnections = [];
    this._checkedOut = new Set();
    this._waiters = [];
    this._closed = false;
  }

  _ensureDatabase() {
    if (this._database === null) {
      this._database = createDatabase(this._databasePath, this._databaseOptions);
    }
    return this._database;
  }

  _createConnection() {
    const db = this._ensureDatabase();
    const conn = new Connection(db);
    this._allConnections.push(conn);
    return conn;
  }

  _wakeNextWaiter(conn) {
    while (this._waiters.length > 0) {
      const w = this._waiters.shift();
      if (w.timer) clearTimeout(w.timer);
      this._checkedOut.add(conn);
      w.resolve(conn);
      return;
    }
    this._idle.push(conn);
  }

  /**
   * Acquire a connection from the pool. Must call release(conn) when done (e.g. in finally).
   * Prefer pool.run(fn) to avoid forgetting release.
   * @returns {Promise<lbug.Connection>}
   */
  acquire() {
    if (this._closed) {
      return Promise.reject(new Error("Pool is closed."));
    }

    while (this._allConnections.length < this._minSize) {
      this._idle.push(this._createConnection());
    }
    if (this._idle.length > 0) {
      const conn = this._idle.shift();
      this._checkedOut.add(conn);
      if (this._validateOnAcquire) {
        return conn.ping().then(() => conn);
      }
      return Promise.resolve(conn);
    }
    if (this._allConnections.length < this._maxSize) {
      const conn = this._createConnection();
      this._checkedOut.add(conn);
      if (this._validateOnAcquire) {
        return conn.ping().then(() => conn);
      }
      return Promise.resolve(conn);
    }

    return new Promise((resolve, reject) => {
      const entry = {
        resolve,
        reject,
        timer: null,
      };
      if (this._acquireTimeoutMillis > 0) {
        entry.timer = setTimeout(() => {
          const i = this._waiters.indexOf(entry);
          if (i !== -1) {
            this._waiters.splice(i, 1);
            reject(new Error("Pool acquire timed out."));
          }
        }, this._acquireTimeoutMillis);
      }
      this._waiters.push(entry);
    });
  }

  /**
   * Return a connection to the pool. No-op if pool is closed.
   * @param {lbug.Connection} conn
   */
  release(conn) {
    if (this._closed) {
      return;
    }
    if (
      conn == null ||
      typeof conn !== "object" ||
      conn.constructor.name !== "Connection"
    ) {
      throw new Error("release(conn): conn must be a Connection from this pool.");
    }
    if (!this._checkedOut.has(conn)) {
      throw new Error("release(conn): connection not from this pool or already released.");
    }
    this._checkedOut.delete(conn);
    this._wakeNextWaiter(conn);
  }

  /**
   * Run a function with a connection; connection is released in finally (on success or throw).
   * @template T
   * @param {(conn: lbug.Connection) => Promise<T>} fn
   * @returns {Promise<T>}
   */
  async run(fn) {
    if (typeof fn !== "function") {
      throw new Error("pool.run(fn): fn must be a function.");
    }
    const conn = await this.acquire();
    try {
      return await fn(conn);
    } finally {
      this.release(conn);
    }
  }

  /**
   * Close the pool: reject new and pending acquire, then close all connections and the database.
   * @returns {Promise<void>}
   */
  async close() {
    if (this._closed) {
      return;
    }
    this._closed = true;
    const err = new Error("Pool is closed.");
    for (const w of this._waiters) {
      if (w.timer) clearTimeout(w.timer);
      w.reject(err);
    }
    this._waiters.length = 0;
    this._idle.length = 0;
    for (const conn of this._allConnections) {
      try {
        await conn.close();
      } catch (_) {
        // ignore
      }
    }
    this._allConnections.length = 0;
    if (this._database) {
      try {
        await this._database.close();
      } catch (_) {
        // ignore
      }
      this._database = null;
    }
  }
}

/**
 * Create a connection pool. One shared Database; up to maxSize Connection instances.
 * @param {lbug.PoolOptions} options
 * @returns {lbug.Pool}
 */
function createPool(options) {
  return new Pool(options);
}

module.exports = { createPool, Pool };
