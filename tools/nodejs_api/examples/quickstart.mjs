/**
 * Quickstart: in-memory database, create schema, load from stream, query.
 * Run from tools/nodejs_api: node examples/quickstart.mjs
 */
import { Database, Connection } from "lbug";

async function* userRows() {
  yield ["Alice", 30];
  yield ["Bob", 25];
}

const db = new Database(":memory:");
const conn = new Connection(db);

await conn.query(`
  CREATE NODE TABLE User(name STRING, age INT64, PRIMARY KEY (name));
`);

await conn.registerStream("users", userRows(), {
  columns: [
    { name: "name", type: "STRING" },
    { name: "age", type: "INT64" },
  ],
});
await conn.query("COPY User FROM (LOAD FROM users RETURN *)");
conn.unregisterStream("users");

const result = await conn.query("MATCH (u:User) RETURN u.name, u.age;");
const rows = await result.getAll();
console.log(rows);

await db.close();
