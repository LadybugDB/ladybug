package com.ladybugdb;

import java.io.IOException;
import java.nio.file.Path;

import org.junit.jupiter.api.AfterAll;
import static org.junit.jupiter.api.Assertions.fail;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.io.TempDir;

public class TestBase {

    protected static Database db;
    protected static Connection conn;
    @TempDir
    static Path tempDir;

    @BeforeAll
    static void getDBandConn() throws IOException {
        String dbPath = tempDir.resolve("db.lbdb").toString();
        TestHelper.loadData(dbPath);
        db = TestHelper.getDatabase();
        conn = TestHelper.getConnection();
    }

    @AfterAll
    static void destroyDBandConn() {
        try {
            db.close();
            conn.close();
        } catch (AssertionError e) {
            fail("destroyDBandConn failed: ");
        }
    }

}
