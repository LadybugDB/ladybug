# Ladybug Service Mode

Ladybug Service Mode provides HTTP service capabilities for the embedded graph database, enabling multiple clients to access the database concurrently via a REST API.

## Architecture

### Overview

```
┌──────────────────────────────────────────────────┐
│                   LbugServer                     │
│               (Orchestrator Layer)               │
│                                                  │
│  ┌──────────────┐    ┌────────────────────────┐  │
│  │   Database    │    │   ConnectionPool        │  │
│  │  (Engine)     │◄───│   ConnectionGuard(RAII) │  │
│  │              │    │   Connection[0..N]      │  │
│  └──────────────┘    └────────────────────────┘  │
│                                                  │
│  ┌──────────────────────────────────────────────┐│
│  │       IServiceManager (Abstract Interface)    ││
│  │  ┌────────────────────────────────────────┐  ││
│  │  │       HttpServiceManager               │  ││
│  │  │       (cpp-httplib Implementation)      │  ││
│  │  │                                        │  ││
│  │  │  POST /cypher  → Execute Cypher query   │  ││
│  │  │  GET  /cypher  → URL parameter query    │  ││
│  │  │  GET  /health  → Health check           │  ││
│  │  │  GET  /schema  → Return table schema    │  ││
│  │  └────────────────────────────────────────┘  ││
│  └──────────────────────────────────────────────┘│
└──────────────────────────────────────────────────┘
```

### Core Components

| Component | File | Description |
|-----------|------|-------------|
| `ServiceConfig` | `src/include/main/service/service_config.h` | Service configuration (host, port, poolSize) |
| `IServiceManager` | `src/include/main/service/i_service_manager.h` | Abstract network layer interface, swappable implementation |
| `ConnectionPool` | `src/include/main/service/connection_pool.h` | Pre-allocated connection pool + RAII ConnectionGuard |
| `HttpServiceManager` | `src/include/main/service/http_service_manager.h` | HTTP service implementation using cpp-httplib |
| `LbugServer` | `src/include/main/service/lbug_server.h` | Orchestrator that assembles all components |
| `queryResultToJson` | `src/include/main/service/query_result_json_serializer.h` | QueryResult to JSON serialization |
| `Session` | `tools/python_api/src_py/session.py` | Python SDK remote client |

### Design Principles

**Decoupled network layer and business logic**: `HttpServiceManager` interacts with business logic through `QueryHandler` / `SchemaHandler` callbacks, without directly depending on `Database` or `Connection`. To switch to BRPC or gRPC in the future, simply create a new `IServiceManager` implementation — no changes needed to `LbugServer`, `ConnectionPool`, or other components.

**Connection pool model**: Inspired by NeuG's `SessionPool` design. Pre-allocates N `Connection` objects and uses `std::mutex` + `std::condition_variable` for blocking acquisition. `ConnectionGuard` uses the RAII pattern to ensure connections are automatically returned after each request.

**JSON response**: Uses yyjson (an existing project dependency) to build JSON. Each column value is serialized via `Value::toString()`, which natively supports all types (node, rel, list, map, struct, etc.).

### Request/Response Format

**POST /cypher request body:**

```json
{"query": "MATCH (n:Person) RETURN n.name, n.age"}
```

**Success response:**

```json
{
  "columns": ["n.name", "n.age"],
  "rows": [["Alice", "30"], ["Bob", "25"]],
  "numRows": 2,
  "compilingTime": 1.23,
  "executionTime": 4.56
}
```

**Error response:**

```json
{"error": "Parser exception: ..."}
```

---

## Build Guide

### Prerequisites

- CMake >= 3.15
- C++20 compiler (GCC 11+, Clang 14+, MSVC 2022+)
- No additional dependencies (cpp-httplib and yyjson are already bundled in the project)

### Build Steps

```bash
# Create build directory
mkdir -p build/release && cd build/release

# Configure (enable Service Mode server)
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVER=TRUE ../..

# Build the server binary
make lbug_server -j$(nproc)

# The binary is located at
# build/release/tools/server/lbug_server
```

> `BUILD_SERVER` defaults to `FALSE` and does not affect the existing build workflow. The Service Mode core code is compiled as an OBJECT library into `liblbug.a`, so language bindings (Python/Java/Node.js) can also use it through the library interface.

### Build Options

| CMake Option | Default | Description |
|-------------|---------|-------------|
| `BUILD_SERVER` | `FALSE` | Build the `lbug_server` binary |
| `BUILD_SHELL` | `TRUE` | Build the interactive shell (unaffected) |

---

## Server Usage

### Command-Line Arguments

```
lbug_server [databasePath] [OPTIONS]

Positional arguments:
  databasePath              Database directory path, ':memory:' for in-memory mode

Options:
  -h, --help                Show help
  --host=<host>             Listen address (default: 127.0.0.1)
  --port=<port>             Listen port (default: 8000)
  --pool_size=<n>           Number of pre-allocated connections (default: 4)
  -d, --default_bp_size=<MB> Buffer pool size in MB
  -r, --read_only           Open database in read-only mode
```

### Examples

```bash
# In-memory mode with default settings
./lbug_server :memory:

# Specify database path, port 9000, 8 connections
./lbug_server /path/to/mydb --port 9000 --pool_size 8

# Read-only mode with custom buffer pool
./lbug_server /path/to/mydb -r -d 2048 --port 8080

# Listen on all interfaces (LAN access)
./lbug_server /path/to/mydb --host 0.0.0.0 --port 8000
```

On successful startup:
```
Lbug service listening at http://127.0.0.1:8000
```

Press `Ctrl+C` or send `SIGTERM` for graceful shutdown.

### API Endpoints

| Endpoint | Method | Request Format | Description |
|----------|--------|---------------|-------------|
| `/cypher` | POST | JSON body `{"query":"..."}` | Execute a Cypher query |
| `/cypher?q=...` | GET | URL parameter | Simple query (convenient for browser testing) |
| `/health` | GET | None | Health check, returns `{"status":"ok"}` |
| `/schema` | GET | None | Return database table schema |

### curl Examples

```bash
# Health check
curl -X GET http://127.0.0.1:8000/health

# Create a table
curl -X POST http://127.0.0.1:8000/cypher \
  -d '{"query":"CREATE NODE TABLE Person(name STRING, age INT64, PRIMARY KEY(name))"}'

# Insert data
curl -X POST http://127.0.0.1:8000/cypher \
  -d '{"query":"CREATE (:Person {name: \"Alice\", age: 30})"}'

# Query
curl -X POST http://127.0.0.1:8000/cypher \
  -d '{"query":"MATCH (n:Person) RETURN n.name, n.age ORDER BY n.age"}'

# GET-style query
curl "http://127.0.0.1:8000/cypher?q=MATCH%20(n:Person)%20RETURN%20count(n)"

# View schema
curl http://127.0.0.1:8000/schema
```

---

## Python SDK

### Installation

The `Session` class is included in the `real_ladybug` package and requires no additional dependencies (uses only the Python standard library).

```bash
pip install real_ladybug
```

### Basic Usage

```python
import real_ladybug as lb

# Connect to a Service Mode server
session = lb.Session("http://localhost:8000")

# Create a table
session.execute("CREATE NODE TABLE Person(name STRING, age INT64, PRIMARY KEY(name))")

# Insert data
session.execute('CREATE (:Person {name: "Alice", age: 30})')
session.execute('CREATE (:Person {name: "Bob", age: 25})')

# Query
result = session.execute("MATCH (n:Person) RETURN n.name, n.age ORDER BY n.age")
print(result.get_column_names())  # ['n.name', 'n.age']
print(result.get_all())           # [['Bob', '25'], ['Alice', '30']]

# Close the session
session.close()
```

### Context Manager

```python
with lb.Session("http://localhost:8000") as session:
    result = session.execute("MATCH (n:Person) RETURN n.name, n.age")
    for row in result:
        print(row)
```

### Iterator

```python
result = session.execute("MATCH (n:Person) RETURN n.name")

# Iterate row by row
while result.has_next():
    row = result.get_next()
    print(row)

# Or use a for loop
for row in session.execute("MATCH (n:Person) RETURN n.name"):
    print(row)

# Get all results at once
rows = session.execute("MATCH (n:Person) RETURN n.name").get_all()
```

### Convert to Pandas DataFrame

```python
df = session.execute("MATCH (n:Person) RETURN n.name, n.age").get_as_df()
print(df)
#   n.name n.age
# 0  Alice    30
# 1    Bob    25
```

### View Schema

```python
schema = session.schema()
print(schema.get_column_names())  # ['id', 'name', 'type', 'database name', 'comment']
print(schema.get_all())
```

### Error Handling

```python
try:
    session.execute("INVALID QUERY")
except RuntimeError as e:
    print(f"Query failed: {e}")

# Connection failure
try:
    session = lb.Session("http://localhost:9999")
except ConnectionError as e:
    print(f"Cannot connect: {e}")
```

### Session API Reference

| Method | Return Type | Description |
|--------|------------|-------------|
| `Session(endpoint, timeout=30.0)` | `Session` | Create a connection, automatically checks connectivity |
| `session.execute(query)` | `RemoteQueryResult` | Execute a Cypher query |
| `session.health()` | `dict` | Health check |
| `session.schema()` | `RemoteQueryResult` | Get database schema |
| `session.close()` | `None` | Close the session |

### RemoteQueryResult API Reference

| Method | Return Type | Description |
|--------|------------|-------------|
| `result.has_next()` | `bool` | Whether there are more rows |
| `result.get_next()` | `list[str]` | Get the next row |
| `result.get_all()` | `list[list[str]]` | Get all remaining rows |
| `result.get_column_names()` | `list[str]` | List of column names |
| `result.get_num_tuples()` | `int` | Total number of rows |
| `result.get_compiling_time()` | `float` | Compiling time (ms) |
| `result.get_execution_time()` | `float` | Execution time (ms) |
| `result.get_as_df()` | `pd.DataFrame` | Convert to Pandas DataFrame |
| `result.reset_iterator()` | `None` | Reset the iterator |

---

## Benchmarking

### Method 1: Python Script (Recommended)

```python
import urllib.request
import json
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

URL = "http://127.0.0.1:8000/cypher"
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def do_query(payload):
    req = urllib.request.Request(
        URL, data=payload.encode(),
        headers={"Content-Type": "application/json"})
    start = time.perf_counter()
    with opener.open(req, timeout=30) as resp:
        resp.read()
    return time.perf_counter() - start

def benchmark(query, num_requests, concurrency):
    payload = json.dumps({"query": query})
    latencies = []
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(do_query, payload) for _ in range(num_requests)]
        for f in as_completed(futures):
            latencies.append(f.result() * 1000)
    total = time.perf_counter() - t0
    latencies.sort()

    print(f"Requests: {num_requests}, Concurrency: {concurrency}")
    print(f"QPS:      {num_requests/total:.1f} req/s")
    print(f"Avg:      {sum(latencies)/len(latencies):.2f} ms")
    print(f"P50:      {latencies[len(latencies)//2]:.2f} ms")
    print(f"P95:      {latencies[int(len(latencies)*0.95)]:.2f} ms")
    print(f"P99:      {latencies[int(len(latencies)*0.99)]:.2f} ms")

# Usage
benchmark("MATCH (n:Person) RETURN n.name, n.age", num_requests=1000, concurrency=8)
```

### Method 2: Using wrk

```bash
# Install wrk (macOS)
brew install wrk

# Create a POST request script
cat > /tmp/cypher.lua << 'EOF'
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"query":"MATCH (n:Person) RETURN n.name, n.age"}'
EOF

# 8 threads, 32 concurrent connections, 10 seconds
wrk -t8 -c32 -d10s -s /tmp/cypher.lua http://127.0.0.1:8000/cypher
```

### Method 3: Using ab (Apache Bench)

```bash
# Write the request body to a file
echo '{"query":"MATCH (n:Person) RETURN n.name, n.age"}' > /tmp/query.json

# 1000 requests, 16 concurrent
ab -n 1000 -c 16 -p /tmp/query.json -T 'application/json' \
   http://127.0.0.1:8000/cypher
```

### Benchmark Reference Data

Test environment: macOS, Apple Silicon, pool_size=8, 1000 rows of data

**Lightweight query (9 rows returned):**

| Concurrency | QPS | Avg Latency | P95 | P99 |
|-------------|-----|------------|-----|-----|
| 1 | 1,936 | 0.49 ms | 0.57 ms | 0.66 ms |
| 4 | 3,675 | 1.05 ms | 1.64 ms | 2.02 ms |
| 8 | 3,735 | 2.06 ms | 3.48 ms | 4.54 ms |
| 16 | 3,675 | 4.01 ms | 7.03 ms | 9.44 ms |
| 32 | 3,617 | 6.45 ms | 11.74 ms | 14.20 ms |

**Heavy query (1000 rows returned):**

| Concurrency | QPS | Avg Latency | P95 | P99 |
|-------------|-----|------------|-----|-----|
| 1 | 1,301 | 0.74 ms | 0.90 ms | 3.06 ms |
| 4 | 3,021 | 1.26 ms | 2.02 ms | 2.36 ms |
| 8 | 3,095 | 2.45 ms | 3.81 ms | 4.93 ms |
| 16 | 3,042 | 4.49 ms | 6.81 ms | 10.27 ms |

### Tuning Tips

- **`--pool_size`**: Set to the number of CPU cores for maximum throughput (each Connection uses roughly a few MB of memory)
- **Read-only workloads**: Add the `-r` flag to reduce transaction overhead
- **Large result sets**: The bottleneck is JSON serialization and network transfer; consider adding Arrow IPC response format in the future
- **High concurrency (100+)**: The cpp-httplib thread-per-request model becomes the bottleneck; replace the `IServiceManager` implementation with BRPC (bthread M:N scheduling) for an estimated 3-5x improvement
