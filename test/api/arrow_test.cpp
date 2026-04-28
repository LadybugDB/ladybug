#include "api_test/api_test.h"
#include "common/exception/runtime.h"
#include "main/query_result/arrow_query_result.h"

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::testing;

class ArrowTest : public ApiTest {};

TEST_F(ArrowTest, resultToArrow) {
    auto query = "MATCH (a:person) WHERE a.fName = 'Bob' RETURN a.fName";
    auto result = conn->query(query);
    auto arrowArray = result->getNextArrowChunk(1);
    ASSERT_EQ(arrowArray->length, 1);
    ASSERT_EQ(arrowArray->null_count, 0);
    ASSERT_EQ(arrowArray->n_children, 1);
    // FIXME: Not sure where the length of the string is stored
    ASSERT_EQ(std::string((const char*)arrowArray->children[0]->buffers[2], 3), "Bob");
    ASSERT_FALSE(result->hasNextArrowChunk());
    arrowArray->release(arrowArray.get());
}

TEST_F(ArrowTest, queryAsArrow) {
    auto query = "MATCH (a:person) WHERE a.fName = 'Bob' RETURN a.fName";
    auto result = conn->queryAsArrow(query, 1);
    auto arrowArray = result->getNextArrowChunk(1);
    ASSERT_EQ(arrowArray->length, 1);
    ASSERT_EQ(arrowArray->null_count, 0);
    ASSERT_EQ(arrowArray->n_children, 1);
    // FIXME: Not sure where the length of the string is stored
    ASSERT_EQ(std::string((const char*)arrowArray->children[0]->buffers[2], 3), "Bob");
    ASSERT_FALSE(result->hasNextArrowChunk());
    arrowArray->release(arrowArray.get());
}

TEST_F(ArrowTest, getArrowResult) {
    auto query = "MATCH (a:person) WHERE a.fName = 'Bob' RETURN a.fName";
    auto result = conn->queryAsArrow(query, 1);
    try {
        result->getNextArrowChunk(2);
        FAIL();
    } catch (const Exception& e) {
        ASSERT_STREQ(e.what(), "Runtime exception: Chunk size does not match expected value 1.");
    }
    try {
        (void)result->hasNext();
        FAIL();
    } catch (const Exception& e) {
        ASSERT_STREQ(e.what(),
            "ArrowQueryResult does not implement hasNext. Use MaterializedQueryResult instead.");
    }
    try {
        (void)result->getNext();
        FAIL();
    } catch (const Exception& e) {
        ASSERT_STREQ(e.what(),
            "ArrowQueryResult does not implement getNext. Use MaterializedQueryResult instead.");
    }
    try {
        (void)result->toString();
        FAIL();
    } catch (const Exception& e) {
        ASSERT_STREQ(e.what(),
            "ArrowQueryResult does not implement toString. Use MaterializedQueryResult instead.");
    }
    ASSERT_TRUE(result->hasNextArrowChunk());
    auto arrowArray = result->getNextArrowChunk(1);
    ASSERT_EQ(arrowArray->length, 1);
    ASSERT_EQ(arrowArray->null_count, 0);
    ASSERT_FALSE(result->hasNextArrowChunk());
    arrowArray->release(arrowArray.get());
}

TEST_F(ArrowTest, getArrowSchema) {
    auto query = "MATCH (a:person) RETURN a.fName as NAME";
    auto result = conn->query(query);
    auto schema = result->getArrowSchema();
    ASSERT_EQ(schema->n_children, 1);
    ASSERT_EQ(std::string(schema->children[0]->name), "NAME");
    schema->release(schema.get());
}

TEST_F(ArrowTest, queryAsArrowTracksCSRMetadataWithoutRelIDs) {
    auto query =
        "MATCH (a:person)-[:knows]->(b:person) RETURN a.rowid, b.rowid ORDER BY a.rowid, b.rowid";
    auto rowResult = conn->query(query);
    std::vector<std::pair<int64_t, int64_t>> expected;
    while (rowResult->hasNext()) {
        auto tuple = rowResult->getNext();
        expected.emplace_back(tuple->getValue(0)->getValue<int64_t>(),
            tuple->getValue(1)->getValue<int64_t>());
    }

    auto result = conn->queryAsArrow(query, 8);
    auto* arrowResult = dynamic_cast<ArrowQueryResult*>(result.get());
    ASSERT_NE(arrowResult, nullptr);
    ASSERT_TRUE(arrowResult->hasCSRMetadata());

    const auto& metadata = arrowResult->getCSRMetadata();
    ASSERT_FALSE(metadata.hasEdgeIDs);
    ASSERT_TRUE(metadata.edgeIDs.empty());

    std::vector<std::pair<int64_t, int64_t>> reconstructed;
    ASSERT_GE(metadata.indptr.size(), 1);
    for (auto srcRowID = 0u; srcRowID + 1 < metadata.indptr.size(); ++srcRowID) {
        for (auto idx = metadata.indptr[srcRowID]; idx < metadata.indptr[srcRowID + 1]; ++idx) {
            reconstructed.emplace_back(static_cast<int64_t>(srcRowID), metadata.indices[idx]);
        }
    }
    ASSERT_EQ(reconstructed, expected);
}

TEST_F(ArrowTest, queryAsArrowTracksCSRMetadataWithRelIDsAndExtraColumns) {
    auto query = "MATCH (a:person)-[e:knows]->(b:person) "
                 "RETURN a.rowid, e.rowid, b.rowid, e.date, b.fName "
                 "ORDER BY a.rowid, e.rowid, b.rowid";
    auto rowResult = conn->query(query);
    std::vector<std::tuple<int64_t, int64_t, int64_t>> expected;
    while (rowResult->hasNext()) {
        auto tuple = rowResult->getNext();
        expected.emplace_back(tuple->getValue(0)->getValue<int64_t>(),
            tuple->getValue(1)->getValue<int64_t>(), tuple->getValue(2)->getValue<int64_t>());
    }

    auto result = conn->queryAsArrow(query, 8);
    auto* arrowResult = dynamic_cast<ArrowQueryResult*>(result.get());
    ASSERT_NE(arrowResult, nullptr);
    ASSERT_TRUE(arrowResult->hasCSRMetadata());
    ASSERT_EQ(result->getColumnNames().size(), 5);

    const auto& metadata = arrowResult->getCSRMetadata();
    ASSERT_TRUE(metadata.hasEdgeIDs);
    ASSERT_EQ(metadata.indices.size(), metadata.edgeIDs.size());

    std::vector<std::tuple<int64_t, int64_t, int64_t>> reconstructed;
    ASSERT_GE(metadata.indptr.size(), 1);
    for (auto srcRowID = 0u; srcRowID + 1 < metadata.indptr.size(); ++srcRowID) {
        for (auto idx = metadata.indptr[srcRowID]; idx < metadata.indptr[srcRowID + 1]; ++idx) {
            reconstructed.emplace_back(static_cast<int64_t>(srcRowID), metadata.edgeIDs[idx],
                metadata.indices[idx]);
        }
    }
    ASSERT_EQ(reconstructed, expected);
}

TEST_F(ArrowTest, queryAsArrowDoesNotTrackCSRMetadataForNonCSRShape) {
    auto query = "MATCH (a:person)-[e:knows]->(b:person) RETURN a.rowid, e.date ORDER BY a.rowid";
    auto result = conn->queryAsArrow(query, 8);
    auto* arrowResult = dynamic_cast<ArrowQueryResult*>(result.get());
    ASSERT_NE(arrowResult, nullptr);
    ASSERT_FALSE(arrowResult->hasCSRMetadata());
}
