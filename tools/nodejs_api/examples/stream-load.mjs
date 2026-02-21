/**
 * Load data from a JavaScript async iterable via LOAD FROM.
 * Run from tools/nodejs_api: node examples/stream-load.mjs
 */
import { Database, Connection } from "lbug";

async function* generateRows() {
  yield [1, "Alice"];
  yield [2, "Bob"];
  yield [3, "Carol"];
}

const db = new Database(":memory:");
const conn = new Connection(db);

await conn.registerStream("users", generateRows(), {
  columns: [
    { name: "id", type: "INT64" },
    { name: "name", type: "STRING" },
  ],
});

const result = await conn.query("LOAD FROM users RETURN *");
for await (const row of result) {
  console.log(row);
}

conn.unregisterStream("users");
await db.close();
