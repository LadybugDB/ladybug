const PERSON_IDS = [0, 2, 3, 5, 7, 8, 9, 10];

describe("Reset iterator", function () {
  it("should reset the iterator of the query result", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    assert.isTrue(queryResult.hasNext());
    let tuple = await queryResult.getNext();
    assert.equal(tuple["a.ID"], 0);
    tuple = await queryResult.getNext();
    assert.equal(tuple["a.ID"], 2);
    tuple = await queryResult.getNext();
    assert.equal(tuple["a.ID"], 3);
    queryResult.resetIterator();
    tuple = await queryResult.getNext();
    assert.equal(tuple["a.ID"], 0);
    tuple = await queryResult.getNext();
    assert.equal(tuple["a.ID"], 2);
    tuple = await queryResult.getNext();
    assert.equal(tuple["a.ID"], 3);
  });
});

describe("Has next", function () {
  it("should return true if there is a next tuple", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    for (let i = 0; i < 8; ++i) {
      assert.isTrue(queryResult.hasNext());
      await queryResult.getNext();
    }
  });

  it("should return false if there is no next tuple", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    for (let i = 0; i < 8; ++i) {
      await queryResult.getNext();
    }
    assert.isFalse(queryResult.hasNext());
  });
});

describe("Get number of tuples", function () {
  it("should return the number of tuples", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    assert.equal(queryResult.getNumTuples(), 8);
  });
});

describe("Get next", function () {
  it("should return the next tuple", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    for (let i = 0; i < 8; ++i) {
      const tuple = await queryResult.getNext();
      assert.equal(tuple["a.ID"], PERSON_IDS[i]);
    }
  });

  it("should return null when no more tuples", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    for (let i = 0; i < 8; ++i) {
      await queryResult.getNext();
    }
    const exhausted = await queryResult.getNext();
    assert.isNull(exhausted, "getNext() returns null when no more tuples");
  });

  it("getNext() returns null exactly when hasNext() is false", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    let count = 0;
    while (queryResult.hasNext()) {
      const row = await queryResult.getNext();
      assert.isNotNull(row, "getNext() must return value when hasNext() is true");
      count++;
    }
    assert.equal(count, 8);
    const afterExhausted = await queryResult.getNext();
    assert.isNull(afterExhausted, "getNext() must return null when hasNext() was false");
  });
});

describe("Each", function () {
  it("should iterate through all tuples in order", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    const ids = [];
    try {
      await new Promise((resolve, reject) => {
        queryResult.each(
          (tuple) => {
            ids.push(tuple["a.ID"]);
          },
          () => {
            resolve();
          },
          (err) => {
            reject(err);
          }
        );
      });
    } catch (err) {
      assert.fail(err);
    }
    assert.deepEqual(ids, PERSON_IDS);
  });
});

describe("Get all", function () {
  it("should return all tuples in order", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    const tuples = await queryResult.getAll();
    const ids = tuples.map((tuple) => tuple["a.ID"]);
    assert.deepEqual(ids, PERSON_IDS);
  });

  it("should automatically reset the iterator", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    await queryResult.getNext();
    await queryResult.getNext();
    await queryResult.getNext();
    const tuples = await queryResult.getAll();
    const ids = tuples.map((tuple) => tuple["a.ID"]);
    assert.deepEqual(ids, PERSON_IDS);
  });
});

describe("All", function () {
  it("should return all tuples in order", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    let tuples;
    try {
      await new Promise((resolve, reject) => {
        queryResult.all(
          (tuples_) => {
            tuples = tuples_;
            resolve();
          },
          (err) => {
            reject(err);
          }
        );
      });
    } catch (err) {
      assert.fail(err);
    }
    const ids = tuples.map((tuple) => tuple["a.ID"]);
    assert.deepEqual(ids, PERSON_IDS);
  });
});

describe("Get column names", function () {
  it("should return column names", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person)-[e:knows]->(b:person) RETURN a.fName, e.date, b.ID;"
    );
    const columnNames = await queryResult.getColumnNames();
    const expectedResult = ["a.fName", "e.date", "b.ID"];
    assert.deepEqual(columnNames, expectedResult);
  });
});

describe("Get column data types", function () {
  it("should return column data types", async function () {
    const queryResult = await conn.query(
      `MATCH (p:person) 
         RETURN p.ID, p.fName, p.isStudent, p.eyeSight, p.birthdate, 
                p.registerTime, p.lastJobDuration, p.workedHours, 
                p.courseScoresPerTerm`
    );
    const columnDataTypes = await queryResult.getColumnDataTypes();
    const expectedResultArr = [
      "INT64",
      "STRING",
      "BOOL",
      "DOUBLE",
      "DATE",
      "TIMESTAMP",
      "INTERVAL",
      "INT64[]",
      "INT64[][]",
    ];
    assert.deepEqual(columnDataTypes, expectedResultArr);
  });
});

describe("Get query summary", function () {
  it("should get query summary of a query result", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID, a.fName ORDER BY a.ID"
    );
    const querySummary = await queryResult.getQuerySummary();
    assert.isTrue(querySummary.hasOwnProperty("compilingTime"));
    assert.isTrue(querySummary.hasOwnProperty("executionTime"));
    assert.isNumber(querySummary.executionTime);
    assert.isNumber(querySummary.compilingTime);
    assert.isTrue(querySummary.executionTime >= 0);
    assert.isTrue(querySummary.compilingTime >= 0);
  });
});

describe("Async iterator (for await...of)", function () {
  it("should iterate rows same as getNext", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    const ids = [];
    for await (const row of queryResult) {
      ids.push(row["a.ID"]);
    }
    assert.deepEqual(ids, PERSON_IDS);
  });

  it("should not materialize full result in memory", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    let count = 0;
    for await (const row of queryResult) {
      count++;
      assert.equal(row["a.ID"], PERSON_IDS[count - 1]);
    }
    assert.equal(count, PERSON_IDS.length);
  });
});

describe("toStream", function () {
  it("should yield rows as Readable stream", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    const stream = queryResult.toStream();
    const rows = [];
    for await (const row of stream) {
      rows.push(row);
    }
    const ids = rows.map((r) => r["a.ID"]);
    assert.deepEqual(ids, PERSON_IDS);
  });
});

describe("Close", function () {
  it("should close the query result", async function () {
    const queryResult = await conn.query(
      "MATCH (a:person) RETURN a.ID ORDER BY a.ID"
    );
    assert.isFalse(queryResult._isClosed);
    queryResult.close();
    assert.isTrue(queryResult._isClosed);
    try {
      await queryResult.getNext();
      assert.fail("No error thrown when the query result is closed");
    } catch (err) {
      assert.equal(err.message, "Query result is closed.");
    }
  });
});
