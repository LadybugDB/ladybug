#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/database.h"
#include "test_helper/test_helper.h"
#include <format>

using namespace lbug::main;
using namespace lbug::testing;
namespace fs = std::filesystem;

namespace {

struct DumpResult {
    int exitCode;
    std::string out;
    std::string err;

    uint64_t records() const {
        uint64_t count = 0;
        const std::string line = "Record at offset ";
        for (auto i = out.find(line); i != std::string::npos; i = out.find(line, i + 1)) {
            count++;
        }
        return count;
    }

    uint64_t lastRecordOffset() const {
        const std::string line = "Record at offset ";
        return std::stoull(out.substr(out.rfind(line) + line.size()));
    }
};

std::string readFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void writeFile(const fs::path& path, const std::string& contents) {
    std::ofstream{path, std::ios::binary} << contents;
}

} // namespace

class WalDumpTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir = TestHelper::getTempDir("wal_dump_test");
        database = dir / "db";
        wal = dir / "db.wal";
    }

    void TearDown() override { fs::remove_all(dir); }

    // Closing without a checkpoint leaves every commit in the WAL.
    void writeWAL(bool enableChecksums = true) {
        SystemConfig config;
        config.enableChecksums = enableChecksums;
        Database db{database.string(), config};
        Connection conn{&db};
        for (const auto* query : {"CALL force_checkpoint_on_close=false",
                 "CALL auto_checkpoint=false", "CREATE NODE TABLE T(id INT64 PRIMARY KEY, v INT64)",
                 "CREATE (:T {id: 1, v: 2})", "ALTER TABLE T ADD extra INT64 DEFAULT 1",
                 "CREATE REL TABLE R(FROM T TO T)", "MATCH (a:T) CREATE (a)-[:R]->(a)",
                 "MATCH (a:T) DETACH DELETE a", "DROP TABLE R", "CREATE (:T {id: 3, v: 4})"}) {
            auto result = conn.query(query);
            ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
        }
    }

    DumpResult dump(const std::string& arguments) const {
        const auto out = dir / "dump.out";
        const auto err = dir / "dump.err";
        const auto status = std::system(std::format("\"{}\" {} > \"{}\" 2> \"{}\"", WAL_DUMP_PATH,
            arguments, out.string(), err.string())
                                            .c_str());
        return {WEXITSTATUS(status), readFile(out), readFile(err)};
    }

    DumpResult dumpDatabase() const { return dump(std::format("\"{}\"", database.string())); }

    DumpResult dumpFile(const fs::path& file) const {
        return dump(std::format("--wal \"{}\"", file.string()));
    }

    fs::path dir;
    fs::path database;
    fs::path wal;
};

TEST_F(WalDumpTest, ReadsTheWALOfADatabase) {
    writeWAL();
    const auto run = dumpDatabase();
    EXPECT_EQ(run.exitCode, 0) << run.err;
    EXPECT_GT(run.records(), 0u);
    EXPECT_NE(run.out.find("Total records found: " + std::to_string(run.records())),
        std::string::npos);
    EXPECT_EQ(run.err, "");
}

TEST_F(WalDumpTest, ADatabaseWithoutAWALIsNotAnError) {
    const auto run = dumpDatabase();
    EXPECT_EQ(run.exitCode, 0);
    EXPECT_NE(run.out.find("Database was cleanly shutdown"), std::string::npos);
}

TEST_F(WalDumpTest, ANamedWALThatIsMissingIsAnError) {
    const auto run = dumpFile(wal);
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.err.find("WAL file does not exist: " + wal.string()), std::string::npos);
    EXPECT_EQ(run.out.find("cleanly shutdown"), std::string::npos);
}

TEST_F(WalDumpTest, AnyOtherArgumentCountIsAnError) {
    for (const auto* arguments : {"", "a b", "--wal", "--wal a b"}) {
        const auto run = dump(arguments);
        EXPECT_EQ(run.exitCode, 1) << arguments;
        EXPECT_NE(run.err.find("Usage: "), std::string::npos) << arguments;
    }
}

TEST_F(WalDumpTest, ReadsAFrozenWALByName) {
    writeWAL();
    const auto expected = dumpDatabase().records();
    const auto frozen = dir / "db.wal.checkpoint";
    fs::rename(wal, frozen);
    EXPECT_NE(dumpDatabase().out.find("WAL file does not exist."), std::string::npos);
    const auto run = dumpFile(frozen);
    EXPECT_EQ(run.exitCode, 0) << run.err;
    EXPECT_EQ(run.records(), expected);
}

TEST_F(WalDumpTest, ADatabaseNamedLikeAWALIsStillADatabase) {
    writeWAL();
    const auto expected = dumpDatabase().records();
    fs::rename(database, dir / "x.wal");
    fs::rename(wal, dir / "x.wal.wal");
    database = dir / "x.wal";
    const auto run = dumpDatabase();
    EXPECT_EQ(run.exitCode, 0) << run.err;
    EXPECT_EQ(run.records(), expected);
}

TEST_F(WalDumpTest, PrintsEnumsAsNumbers) {
    writeWAL();
    const auto run = dumpDatabase();
    EXPECT_NE(run.out.find("Entry Type: 0\n"), std::string::npos);
    EXPECT_NE(run.out.find("AlterType: 10\n"), std::string::npos);
    EXPECT_NE(run.out.find("TableType: 1\n"), std::string::npos);
    EXPECT_NE(run.out.find("Direction: 0\n"), std::string::npos);
    const std::string dropped = "EntryType: ";
    const auto entryType = run.out.find(dropped);
    ASSERT_NE(entryType, std::string::npos);
    EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(run.out[entryType + dropped.size()])));
    EXPECT_EQ(run.out.find('\0'), std::string::npos);
}

TEST_F(WalDumpTest, ACutInATrailingChecksumStopsAtItsRecord) {
    writeWAL();
    const auto whole = dumpDatabase();
    EXPECT_NE(whole.out.find("Checksums Enabled: true\n"), std::string::npos);
    const auto size = fs::file_size(wal);
    fs::resize_file(wal, size - 5);
    const auto run = dumpDatabase();
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_EQ(run.records(), whole.records() - 1);
    const auto offset = whole.lastRecordOffset();
    EXPECT_NE(run.err.find(std::format("Stopped at offset {} after {} records, {} of {} bytes left",
                  offset, whole.records() - 1, size - 5 - offset, size - 5)),
        std::string::npos)
        << run.err;
    EXPECT_NE(run.err.find("Reading past the end"), std::string::npos) << run.err;
}

TEST_F(WalDumpTest, ACutWithoutChecksumsStopsAtItsRecord) {
    writeWAL(false /* enableChecksums */);
    const auto whole = dumpDatabase();
    EXPECT_NE(whole.out.find("Checksums Enabled: false\n"), std::string::npos);
    const auto size = fs::file_size(wal);
    fs::resize_file(wal, size - 5);
    const auto run = dumpDatabase();
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_EQ(run.records(), whole.records() - 1);
    EXPECT_NE(run.err.find(std::format("Stopped at offset {} after", whole.lastRecordOffset())),
        std::string::npos)
        << run.err;
}

TEST_F(WalDumpTest, ACutInsideARecordIsReportedAsCutShort) {
    writeWAL();
    const auto header = 25u; // database ID, checksum flag and the header's checksum
    fs::resize_file(wal, header + 3);
    const auto run = dumpDatabase();
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_EQ(run.records(), 0u);
    EXPECT_NE(run.err.find(std::format("Stopped at offset {} after 0 records, 3 of {} bytes left: "
                                       "Storage exception: The record is cut short",
                  header, header + 3)),
        std::string::npos)
        << run.err;
}

TEST_F(WalDumpTest, AFlippedByteStopsAtItsRecord) {
    writeWAL();
    const auto whole = dumpDatabase();
    auto contents = readFile(wal);
    // The last byte of the last record's payload, before its 8-byte checksum.
    contents[contents.size() - 9] ^= 0xFF;
    writeFile(wal, contents);
    const auto run = dumpDatabase();
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_EQ(run.records(), whole.records() - 1);
    EXPECT_NE(run.err.find(std::format("Stopped at offset {} after {} records",
                  whole.lastRecordOffset(), whole.records() - 1)),
        std::string::npos)
        << run.err;
    EXPECT_NE(run.err.find("fails its checksum"), std::string::npos) << run.err;
}

TEST_F(WalDumpTest, AFlippedRecordTypeIsReportedAsSuch) {
    writeWAL();
    const auto whole = dumpDatabase();
    auto contents = readFile(wal);
    // The last record's payload starts after its 8-byte length, with the record type.
    contents[whole.lastRecordOffset() + 8] = 0;
    writeFile(wal, contents);
    const auto run = dumpDatabase();
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_EQ(run.records(), whole.records() - 1);
    EXPECT_NE(run.err.find("invalid WAL record type"), std::string::npos) << run.err;
}

TEST_F(WalDumpTest, AFileThatCannotBeOpenedIsReported) {
    if (geteuid() == 0) {
        GTEST_SKIP() << "root can open any file";
    }
    const auto file = dir / "unreadable.wal";
    writeFile(file, "not a wal!");
    fs::permissions(file, fs::perms::none);
    const auto run = dumpFile(file);
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.err.find("Cannot read " + file.string()), std::string::npos) << run.err;
}

TEST_F(WalDumpTest, ADirectoryThatCannotBeSearchedIsReported) {
    if (geteuid() == 0) {
        GTEST_SKIP() << "root can search any directory";
    }
    const auto closed = dir / "closed";
    fs::create_directory(closed);
    fs::permissions(closed, fs::perms::none);
    const auto run = dumpFile(closed / "db.wal");
    fs::permissions(closed, fs::perms::owner_all);
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.err.find("Cannot read " + (closed / "db.wal").string()), std::string::npos)
        << run.err;
}

TEST_F(WalDumpTest, AFileTooShortForAHeaderStopsAtTheStart) {
    const auto file = dir / "garbage.wal";
    writeFile(file, "not a wal!");
    const auto run = dumpFile(file);
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.err.find("Stopped at offset 0 after 0 records, 10 of 10 bytes left"),
        std::string::npos)
        << run.err;
}
