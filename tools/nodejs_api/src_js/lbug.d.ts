
/**
 * A nullable type that can be either T or null.
 */
export type Nullable<T> = T | null;

/**
 * A callback function pattern used throughout the API.
 * @template T The type of the result value (defaults to void)
 */
export type Callback<T = void> = (error: Error | null, result?: T) => void;

/**
 * Callback for query execution progress updates.
 * @param pipelineProgress Progress of the current pipeline (0-100)
 * @param numPipelinesFinished Number of pipelines completed
 * @param numPipelines Total number of pipelines
 */
export type ProgressCallback = (
    pipelineProgress: number,
    numPipelinesFinished: number,
    numPipelines: number
) => void;

/**
 * Options for query() and execute().
 * Use signal to cancel the operation via AbortController.
 */
export interface QueryOptions {
    signal?: AbortSignal;
    progressCallback?: ProgressCallback;
}

/**
 * Represents a node ID in the graph database.
 */
export interface NodeID {
    /** The offset of the node in its table */
    offset: number;
    /** The table ID the node belongs to */
    table: number;
}

/**
 * Represents a node value in the graph.
 */
export interface NodeValue {
    /** The label of the node (type) */
    _label: string | null;
    /** The ID of the node */
    _id: NodeID | null;
    /** Additional properties of the node */
    [key: string]: any;
}

/**
 * Represents a relationship (edge) value in the graph.
 */
export interface RelValue {
    /** The source node ID */
    _src: NodeID | null;
    /** The destination node ID */
    _dst: NodeID | null;
    /** The relationship type/label */
    _label: string | null;
    /** The relationship ID */
    _id: any;
    /** Additional properties of the relationship */
    [key: string]: any;
}

/**
 * Represents a recursive relationship value (path) in the graph.
 */
export interface RecursiveRelValue {
    /** Array of nodes in the path */
    _nodes: any[];
    /** Array of relationships in the path */
    _rels: any[];
}

/**
 * Union type representing all possible value types in Lbug.
 */
export type LbugValue =
    | null
    | boolean
    | number
    | bigint
    | string
    | Date
    | NodeValue
    | RelValue
    | RecursiveRelValue
    | LbugValue[]
    | { [key: string]: LbugValue };

/**
 * Configuration options for the database system.
 */
export interface SystemConfig {
    /** Size of the buffer pool in bytes */
    bufferPoolSize?: number;
    /** Whether to enable compression */
    enableCompression?: boolean;
    /** Whether to open the database in read-only mode */
    readOnly?: boolean;
    /** Maximum size of the database in bytes */
    maxDBSize?: number;
    /** Whether to enable automatic checkpoints */
    autoCheckpoint?: boolean;
    /** Threshold for automatic checkpoints */
    checkpointThreshold?: number;
}

/**
 * Options for createPool(). Same shape as Database constructor args (except path).
 */
export interface PoolDatabaseOptions {
    bufferManagerSize?: number;
    enableCompression?: boolean;
    readOnly?: boolean;
    maxDBSize?: number;
    autoCheckpoint?: boolean;
    checkpointThreshold?: number;
    throwOnWalReplayFailure?: boolean;
    enableChecksums?: boolean;
    openLockRetryMs?: number;
}

/**
 * Options for createPool().
 */
export interface PoolOptions {
    /** Database file path (default ":memory:") */
    databasePath?: string;
    /** Same shape as Database constructor options (bufferManagerSize, readOnly, etc.) */
    databaseOptions?: PoolDatabaseOptions;
    /** Minimum connections to keep (default 0) */
    minSize?: number;
    /** Maximum connections in the pool (required) */
    maxSize: number;
    /** Max time to wait for acquire in ms (0 = wait forever, default 0) */
    acquireTimeoutMillis?: number;
    /** If true, call conn.ping() before handing out (default false) */
    validateOnAcquire?: boolean;
}

/**
 * Connection pool: acquire/release or run(fn). One shared Database, up to maxSize Connection instances.
 */
export interface Pool {
    /** Acquire a connection; must call release(conn) when done. Prefer run(fn) to avoid leaks. */
    acquire(): Promise<Connection>;
    /** Return a connection to the pool. */
    release(conn: Connection): void;
    /** Run fn(conn); connection is released in finally (on success or throw). */
    run<T>(fn: (conn: Connection) => Promise<T>): Promise<T>;
    /** Close pool: reject new/pending acquire, then close all connections and database. */
    close(): Promise<void>;
}

/** Pool constructor (use createPool() instead of new Pool()). */
export type PoolConstructor = new (options: PoolOptions) => Pool;

/**
 * Create a connection pool.
 * @param options Pool options (maxSize required; databasePath, databaseOptions, minSize, acquireTimeoutMillis, validateOnAcquire optional)
 * @returns Pool instance
 */
export function createPool(options: PoolOptions): Pool;

/**
 * Represents a Lbug database instance.
 */
export class Database {
    /**
     * Constructs a new Database instance.
     * @param databasePath Path to the database directory (defaults to ":memory:")
     * @param bufferManagerSize Size of the buffer manager in bytes
     * @param enableCompression Whether to enable compression
     * @param readOnly Whether to open in read-only mode
     * @param maxDBSize Maximum size of the database in bytes
     * @param autoCheckpoint Whether to enable automatic checkpoints
     * @param checkpointThreshold Threshold for automatic checkpoints
     * @param throwOnWalReplayFailure If true, WAL replay failures throw; otherwise replay stops at error
     * @param enableChecksums If true, use checksums to detect WAL corruption
     * @param openLockRetryMs When the file is locked, retry opening for up to this many ms (default 5000). Set 0 to fail immediately. Only for async init(); ignored for :memory:
     */
    constructor(
        databasePath?: string,
        bufferManagerSize?: number,
        enableCompression?: boolean,
        readOnly?: boolean,
        maxDBSize?: number,
        autoCheckpoint?: boolean,
        checkpointThreshold?: number,
        throwOnWalReplayFailure?: boolean,
        enableChecksums?: boolean,
        openLockRetryMs?: number
    );

    /**
     * Initialize the database. Calling this function is optional, as the
     * database is initialized automatically when the first query is executed.
     * When the file is locked, retries for up to openLockRetryMs (default 5s) before throwing.
     * @returns Promise that resolves when initialization completes
     */
    init(): Promise<void>;

    /**
     * Initialize the database synchronously. Calling this function is optional, as the
     * database is initialized automatically when the first query is executed. This function
     * may block the main thread, so use it with caution.
     */
    initSync(): void;

    /**
     * Close the database and release resources.
     * @returns Promise that resolves when database is closed
     */
    close(): Promise<void>;

    /**
     * Close the database synchronously.
     */
    closeSync(): void;

    /**
     * Get the version of the Lbug library.
     * @returns The version string of the library
     */
    static getVersion(): string;

    /**
     * Get the storage version of the Lbug library.
     * @returns The storage version of the library
     */
    static getStorageVersion(): number;
}

/**
 * Represents a connection to a Lbug database.
 */
export class Connection {
    /**
     * Creates a new connection to the specified database.
     * @param database The database instance to connect to
     * @param numThreads Optional maximum number of threads for query execution
     */
    constructor(database: Database, numThreads?: number);

    /**
     * Initialize the connection.
     * @returns Promise that resolves when initialization completes
     */
    init(): Promise<void>;

    /**
     * Initialize the connection synchronously. This function may block the main thread, so use it with caution.
     */
    initSync(): void;

    /**
     * Set the maximum number of threads for query execution.
     * @param numThreads The number of threads to use
     */
    setMaxNumThreadForExec(numThreads: number): void;

    /**
     * Set the query timeout in milliseconds.
     * @param timeoutInMs Timeout in milliseconds
     */
    setQueryTimeout(timeoutInMs: number): void;

    /**
     * Interrupt the currently executing query on this connection.
     * No-op if the connection is not initialized or no query is running.
     */
    interrupt(): void;

    /**
     * Close the connection.
     * @returns Promise that resolves when connection is closed
     */
    close(): Promise<void>;

    /**
     * Close the connection synchronously.
     */
    closeSync(): void;

    /**
     * Execute a prepared statement.
     * @param preparedStatement The prepared statement to execute
     * @param params Parameters for the query as a plain object
     * @param optionsOrProgressCallback Options (e.g. signal for abort) or legacy progress callback
     * @returns Promise that resolves to the query result(s). Rejects with DOMException AbortError if signal is aborted.
     */
    execute(
        preparedStatement: PreparedStatement,
        params?: Record<string, LbugValue>,
        optionsOrProgressCallback?: QueryOptions | ProgressCallback
    ): Promise<QueryResult | QueryResult[]>;

    /**
     * Execute a prepared statement synchronously.
     * @param preparedStatement The prepared statement to execute
     * @param params Parameters for the query as a plain object
     * @returns The query result(s)
     */
    executeSync(
        preparedStatement: PreparedStatement,
        params?: Record<string, LbugValue>
    ): QueryResult | QueryResult[];

    /**
     * Prepare a statement for execution.
     * @param statement The statement to prepare
     * @returns Promise that resolves to the prepared statement
     */
    prepare(statement: string): Promise<PreparedStatement>;

    /**
     * Prepare a statement for execution synchronously.
     * @param statement The statement to prepare
     * @returns The prepared statement
     */
    prepareSync(statement: string): PreparedStatement;

    /**
     * Execute a query.
     * @param statement The statement to execute
     * @param optionsOrProgressCallback Options (e.g. signal for abort) or legacy progress callback
     * @returns Promise that resolves to the query result(s). Rejects with DOMException AbortError if signal is aborted.
     */
    query(
        statement: string,
        optionsOrProgressCallback?: QueryOptions | ProgressCallback
    ): Promise<QueryResult | QueryResult[]>;

    /**
     * Run a function inside a single write transaction. Commits on success, rolls back on throw.
     * @param fn Async function that can use this connection's query/execute
     * @returns Promise that resolves to the return value of fn
     */
    transaction<T>(fn: () => Promise<T>): Promise<T>;

    /**
     * Execute a query synchronously.
     * @param statement The statement to execute
     * @returns The query result(s)
     */
    querySync(statement: string): QueryResult | QueryResult[];

    /**
     * Check that the connection is alive (e.g. for pools or health checks).
     * @returns Promise that resolves to true if OK, rejects if connection is broken
     */
    ping(): Promise<boolean>;

    /**
     * Run EXPLAIN on a Cypher statement and return the plan as a string.
     * @param statement Cypher statement (e.g. "MATCH (a:person) RETURN a")
     * @returns Promise that resolves to the plan string (one row per line)
     */
    explain(statement: string): Promise<string>;

    /**
     * Get the number of nodes in a node table. Connection must be initialized.
     * @param nodeName Name of the node table (e.g. "User")
     * @returns Count of nodes
     */
    getNumNodes(nodeName: string): number;

    /**
     * Get the number of relationships in a rel table. Connection must be initialized.
     * @param relName Name of the rel table (e.g. "Follows")
     * @returns Count of relationships
     */
    getNumRels(relName: string): number;

    /**
     * Register a stream source for LOAD FROM name. Source must be AsyncIterable of rows (array or object).
     * Unregister with unregisterStream(name) when done.
     * @param name Name used in Cypher: LOAD FROM name RETURN ...
     * @param source AsyncIterable of rows (array of column values or object keyed by column name)
     * @param options.columns Schema: array of { name: string, type: string } (type: INT64, INT32, DOUBLE, STRING, BOOL, DATE, etc.)
     */
    registerStream(
        name: string,
        source: AsyncIterable<unknown[] | Record<string, unknown>>,
        options: { columns: Array<{ name: string; type: string }> }
    ): Promise<void>;

    /**
     * Unregister a stream source by name.
     * @param name Name passed to registerStream
     */
    unregisterStream(name: string): void;
}

/**
 * Represents a prepared statement for efficient query execution.
 * Note: This class is created internally by Connection.prepare() methods.
 */
export class PreparedStatement {
    /**
     * Check if the statement was prepared successfully.
     * @returns True if preparation was successful
     */
    isSuccess(): boolean;

    /**
     * Get the error message if preparation failed.
     * @returns The error message or empty string if successful
     */
    getErrorMessage(): string;
}

/**
 * Query summary with compiling and execution times (milliseconds).
 */
export interface QuerySummary {
    compilingTime: number;
    executionTime: number;
}

/**
 * Represents the results of a query execution.
 * Note: This class is created internally by Connection query methods.
 * Supports async iteration: for await (const row of result) { ... }
 */
export class QueryResult implements AsyncIterable<Record<string, LbugValue> | null> {
    /**
     * Async iterator for row-by-row consumption (for await...of).
     */
    [Symbol.asyncIterator](): AsyncIterator<Record<string, LbugValue> | null>;

    /**
     * Reset the iterator for reading results.
     */
    resetIterator(): void;

    /**
     * Check if there are more rows to read.
     * @returns True if more rows are available
     */
    hasNext(): boolean;

    /**
     * Get the number of tuples (rows) in the result.
     * @returns The number of rows
     */
    getNumTuples(): number;

    /**
     * Get the next row.
     * @returns Promise that resolves to the next row or null if no more rows
     */
    getNext(): Promise<Record<string, LbugValue> | null>;

    /**
     * Get the next row synchronously.
     * @returns The next row or null if no more rows
     */
    getNextSync(): Record<string, LbugValue> | null;

    /**
     * Return the query result as a string (header + rows). For failed queries returns the error message.
     * @returns String representation of the result
     */
    toString(): string;

    /**
     * Return a Node.js Readable stream (object mode) that yields one row per chunk.
     * @returns Readable stream of row objects
     */
    toStream(): import("stream").Readable;

    /**
     * Iterate through the query result with callback functions.
     * @param resultCallback Callback function called for each row
     * @param doneCallback Callback function called when iteration is done
     * @param errorCallback Callback function called when there is an error
     */
    each(
        resultCallback: (row: Record<string, LbugValue>) => void,
        doneCallback: () => void,
        errorCallback: (error: Error) => void
    ): void;

    /**
     * Get all rows of the query result.
     * @returns Promise that resolves to all rows
     */
    getAll(): Promise<Record<string, LbugValue>[]>;

    /**
     * Get all rows of the query result synchronously.
     * @returns All rows of the query result
     */
    getAllSync(): Record<string, LbugValue>[];

    /**
     * Get all rows of the query result with callback functions.
     * @param resultCallback Callback function called with all rows
     * @param errorCallback Callback function called when there is an error
     */
    all(
        resultCallback: (rows: Record<string, LbugValue>[]) => void,
        errorCallback: (error: Error) => void
    ): void;

    /**
     * Get the column data types.
     * @returns Promise that resolves to array of data type strings
     */
    getColumnDataTypes(): Promise<string[]>;

    /**
     * Get the column data types synchronously.
     * @returns Array of data type strings
     */
    getColumnDataTypesSync(): string[];

    /**
     * Get the column names.
     * @returns Promise that resolves to array of column names
     */
    getColumnNames(): Promise<string[]>;

    /**
     * Get the column names synchronously.
     * @returns Array of column names
     */
    getColumnNamesSync(): string[];

    /**
     * Get the query summary (compiling and execution time in milliseconds).
     * @returns Promise that resolves to the query summary
     */
    getQuerySummary(): Promise<QuerySummary>;

    /**
     * Get the query summary synchronously.
     * @returns The query summary
     */
    getQuerySummarySync(): QuerySummary;

    /**
     * Close the result set and release resources.
     */
    close(): void;
}

/**
 * Error code when the database file is locked by another process.
 * Use with init() / initSync() or first query: catch and check err.code === LBUG_DATABASE_LOCKED.
 */
export const LBUG_DATABASE_LOCKED: "LBUG_DATABASE_LOCKED";

/**
 * Default export for the Lbug module.
 */
declare const lbug: {
    Database: typeof Database;
    Connection: typeof Connection;
    PreparedStatement: typeof PreparedStatement;
    QueryResult: typeof QueryResult;
    createPool: typeof createPool;
    Pool: PoolConstructor;
    LBUG_DATABASE_LOCKED: typeof LBUG_DATABASE_LOCKED;
    VERSION: string;
    STORAGE_VERSION: bigint;
};

export default lbug;
