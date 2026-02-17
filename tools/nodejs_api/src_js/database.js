"use strict";

const LbugNative = require("./lbug_native.js");

/** Error code when the database file is locked by another process. */
const LBUG_DATABASE_LOCKED = "LBUG_DATABASE_LOCKED";

const LOCK_ERROR_MESSAGE = "Could not set lock on file";

function isLockError(err) {
  return err && typeof err.message === "string" && err.message.includes(LOCK_ERROR_MESSAGE);
}

function normalizeInitError(err) {
  if (isLockError(err)) {
    const e = new Error(err.message);
    e.code = LBUG_DATABASE_LOCKED;
    e.cause = err;
    return e;
  }
  return err;
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

class Database {
  /**
   * Initialize a new Database object. Note that the initialization is done
   * lazily, so the database file is not opened until the first query is
   * executed. To initialize the database immediately, call the `init()`
   * function on the returned object.
   *
   * @param {String} databasePath path to the database file. If the path is not specified, or empty, or equal to
   `:memory:`, the database will be created in memory.
   * @param {Number} bufferManagerSize size of the buffer manager in bytes.
   * @param {Boolean} enableCompression whether to enable compression.
   * @param {Boolean} readOnly if true, database will be opened in read-only mode.
   * @param {Number} maxDBSize maximum size of the database file in bytes. Note that
   * this is introduced temporarily for now to get around with the default 8TB mmap
   * address space limit some environment.
   * @param {Boolean} autoCheckpoint If true, the database will automatically checkpoint when the size of
   * the WAL file exceeds the checkpoint threshold.
   * @param {Number} checkpointThreshold The threshold of the WAL file size in bytes. When the size of the
   * WAL file exceeds this threshold, the database will checkpoint if autoCheckpoint is true.
   * @param {Boolean} throwOnWalReplayFailure If true, any WAL replaying failure when loading the database
   * will throw an error. Otherwise, Lbug will silently ignore the failure and replay up to where
   * the error occured.
   * @param {Boolean} enableChecksums If true, the database will use checksums to detect corruption in the
   * WAL file.
   * @param {Number} [openLockRetryMs=5000] When the database file is locked, retry opening for up to this many ms
   * (grace period). Only applies to async init(); set to 0 to fail immediately. Ignored for in-memory databases.
   */
  constructor(
    databasePath,
    bufferManagerSize = 0,
    enableCompression = true,
    readOnly = false,
    maxDBSize = 0,
    autoCheckpoint = true,
    checkpointThreshold = -1,
    throwOnWalReplayFailure = true,
    enableChecksums = true,
    openLockRetryMs = 5000,
  ) {
    if (!databasePath) {
      databasePath = ":memory:";
    }
    else if (typeof databasePath !== "string") {
      throw new Error("Database path must be a string.");
    }
    if (typeof bufferManagerSize !== "number" || bufferManagerSize < 0) {
      throw new Error("Buffer manager size must be a positive integer.");
    }
    if (typeof maxDBSize !== "number" || maxDBSize < 0) {
      throw new Error("Max DB size must be a positive integer.");
    }
    if (typeof checkpointThreshold !== "number" || maxDBSize < -1) {
      throw new Error("Checkpoint threshold must be a positive integer.");
    }
    if (typeof openLockRetryMs !== "number" || openLockRetryMs < 0) {
      throw new Error("openLockRetryMs must be a non-negative number.");
    }
    bufferManagerSize = Math.floor(bufferManagerSize);
    maxDBSize = Math.floor(maxDBSize);
    checkpointThreshold = Math.floor(checkpointThreshold);
    this._database = new LbugNative.NodeDatabase(
      databasePath,
      bufferManagerSize,
      enableCompression,
      readOnly,
      maxDBSize,
      autoCheckpoint,
      checkpointThreshold,
      throwOnWalReplayFailure,
      enableChecksums
    );
    this._isInitialized = false;
    this._initPromise = null;
    this._isClosed = false;
    // Grace period for lock: retry for up to openLockRetryMs (0 = no retry). In-memory has no file lock.
    this._openLockRetryMs = databasePath === ":memory:" ? 0 : Math.floor(openLockRetryMs);
  }

  /**
   * Get the version of the library.
   * @returns {String} the version of the library.
   */
  static getVersion() {
    return LbugNative.NodeDatabase.getVersion();
  }

  /**
   * Get the storage version of the library.
   * @returns {Number} the storage version of the library.
   */
  static getStorageVersion() {
    return LbugNative.NodeDatabase.getStorageVersion();
  }

  /**
   * Initialize the database. Calling this function is optional, as the
   * database is initialized automatically when the first query is executed.
   * When the file is locked, init() retries for up to openLockRetryMs (default 5s) before throwing.
   */
  async init() {
    if (!this._isInitialized) {
      if (!this._initPromise) {
        const self = this;
        const tryOnce = () =>
          new Promise((resolve, reject) => {
            self._database.initAsync((err) => {
              if (err) reject(err);
              else {
                self._isInitialized = true;
                resolve();
              }
            });
          });
        const OPEN_LOCK_DELAY_MS = 200;

        this._initPromise = (async () => {
          const start = Date.now();
          for (;;) {
            if (self._isClosed) throw new Error("Database is closed.");
            try {
              await tryOnce();
              return;
            } catch (err) {
              if (!isLockError(err)) throw normalizeInitError(err);
              if (
                self._openLockRetryMs <= 0 ||
                Date.now() - start >= self._openLockRetryMs
              ) {
                throw normalizeInitError(err);
              }
              await sleep(OPEN_LOCK_DELAY_MS);
            }
          }
        })();
      }
      try {
        await this._initPromise;
      } finally {
        this._initPromise = null;
      }
    }
  }

  /**
   * Initialize the database synchronously. Calling this function is optional, as the
   * database is initialized automatically when the first query is executed. This function
   * may block the main thread, so use it with caution.
   */
  initSync() {
    if (this._initPromise) {
      throw new Error("There is an ongoing asynchronous initialization. Please wait for it to finish.");
    }
    if (this._isInitialized) {
      return;
    }
    try {
      this._database.initSync();
    } catch (err) {
      throw normalizeInitError(err);
    }
    this._isInitialized = true;
  }

  /**
   * Internal function to get the underlying native database object.
   * @returns {LbugNative.NodeDatabase} the underlying native database.
   * @throws {Error} if the database is closed.
   */
  async _getDatabase() {
    if (this._isClosed) {
      throw new Error("Database is closed.");
    }
    await this.init();
    return this._database;
  }

  /**
   * Internal function to get the underlying native database object synchronously.
   * @returns {LbugNative.NodeDatabase} the underlying native database.
   * @throws {Error} if the database is closed.
   */
  _getDatabaseSync() {
    if (this._isClosed) {
      throw new Error("Database is closed.");
    }
    if (!this._isInitialized) {
      this.initSync();
    }
    return this._database;
  }

  /**
   * Close the database.
   */
  async close() {
    if (this._isClosed) {
      return;
    }
    if (!this._isInitialized) {
      if (this._initPromise) {
        // Database is initializing, wait for it to finish first.
        await this._initPromise;
      } else {
        // Database is not initialized, simply mark it as closed and initialized.
        this._isInitialized = true;
        this._isClosed = true;
        delete this._database;
        return;
      }
    }
    // Database is initialized, close it.
    this._database.close();
    delete this._database;
    this._isClosed = true;
  }

  /**
   * Close the database synchronously.
   * @throws {Error} if there is an ongoing asynchronous initialization.
   */
  closeSync() {
    if (this._isClosed) {
      return;
    }
    if (!this._isInitialized) {
      if (this._initPromise) {
        throw new Error("There is an ongoing asynchronous initialization. Please wait for it to finish.");
      } else {
        this._isInitialized = true;
        this._isClosed = true;
        delete this._database;
        return;
      }
    }
    this._database.close();
    delete this._database;
    this._isClosed = true;
  }
}

Database.LBUG_DATABASE_LOCKED = LBUG_DATABASE_LOCKED;

module.exports = Database;
