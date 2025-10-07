const { assert } = require("chai");

describe("Get version", function () {
  it("should get the version of the library", function () {
    assert.isString(lbug.VERSION);
    assert.notEqual(lbug.VERSION, "");
  });

  it("should get the storage version of the library", function () {
    assert.isNumber(lbug.STORAGE_VERSION);
    assert.isAtLeast(lbug.STORAGE_VERSION, 1);
  });
});
