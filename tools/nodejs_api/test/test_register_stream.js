const { assert } = require("chai");

describe("registerStream / LOAD FROM stream", function () {
  it("should LOAD FROM registered stream and return rows", async function () {
    async function* rowSource() {
      yield [1, "a"];
      yield [2, "b"];
      yield [3, "c"];
    }
    await conn.registerStream("mystream", rowSource(), {
      columns: [
        { name: "id", type: "INT64" },
        { name: "label", type: "STRING" },
      ],
    });
    try {
      const result = await conn.query("LOAD FROM mystream RETURN *");
      const rows = Array.isArray(result) ? result : [result];
      assert.isAtLeast(rows.length, 1);
      const r = rows[0];
      assert.isTrue(r.hasNext());
      const row1 = await r.getNext();
      assert.exists(row1);
      assert.equal(row1["id"], 1);
      assert.equal(row1["label"], "a");
      const row2 = await r.getNext();
      assert.equal(row2["id"], 2);
      assert.equal(row2["label"], "b");
      const row3 = await r.getNext();
      assert.equal(row3["id"], 3);
      assert.equal(row3["label"], "c");
      assert.isNull(await r.getNext());
    } finally {
      conn.unregisterStream("mystream");
    }
  });

  it("should LOAD FROM stream with object rows (column order from schema)", async function () {
    async function* objectRowSource() {
      yield { id: 10, label: "x" };
      yield { label: "y", id: 20 };
    }
    await conn.registerStream("objstream", objectRowSource(), {
      columns: [
        { name: "id", type: "INT64" },
        { name: "label", type: "STRING" },
      ],
    });
    try {
      const result = await conn.query("LOAD FROM objstream RETURN *");
      const r = Array.isArray(result) ? result[0] : result;
      const row1 = await r.getNext();
      assert.isNotNull(row1, "expected first row from stream");
      assert.equal(row1["id"], 10);
      assert.equal(row1["label"], "x");
      const row2 = await r.getNext();
      assert.isNotNull(row2, "expected second row from stream");
      assert.equal(row2["id"], 20);
      assert.equal(row2["label"], "y");
      assert.isNull(await r.getNext());
    } finally {
      conn.unregisterStream("objstream");
    }
  });

  it("should unregisterStream by name", async function () {
    async function* empty() {
      if (false) yield [];
    }
    await conn.registerStream("tmpstream", empty(), {
      columns: [{ name: "x", type: "INT64" }],
    });
    conn.unregisterStream("tmpstream");
    try {
      await conn.query("LOAD FROM tmpstream RETURN *");
      assert.fail("Expected error when loading from unregistered stream.");
    } catch (e) {
      assert.include(e.message, "not in scope");
    }
  });
});
