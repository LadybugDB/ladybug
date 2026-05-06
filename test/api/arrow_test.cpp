#include "api_test/api_test.h"
#include "common/exception/runtime.h"
#include "main/query_result/arrow_query_result.h"

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::testing;

class ArrowTest : public ApiTest {};

static void releaseCSRArrowArray(ArrowQueryResult::CSRArrowArray& array) {
    array.release();
}

TEST(ArrowQueryResultTest, exportsCSRMetadataAsZeroCopyArrowArrays) {
    ArrowQueryResult::CSRMetadata metadata;
    metadata.indptr = {0, 2, 3};
    metadata.indices = {4, 5, 6};
    metadata.edgeIDs = {10, 11, 12};
    metadata.hasEdgeIDs = true;

    ArrowQueryResult result{{}, 8, std::move(metadata)};
    const auto& storedMetadata = result.getCSRMetadata();
    auto csrArrays = result.getCSRArrowArrays();

    ASSERT_STREQ(csrArrays.indptr.schema.format, "l");
    ASSERT_STREQ(csrArrays.indices.schema.format, "l");
    ASSERT_TRUE(csrArrays.edgeIDs.has_value());
    ASSERT_STREQ(csrArrays.edgeIDs->schema.format, "l");
    ASSERT_EQ(csrArrays.indptr.array.length, static_cast<int64_t>(storedMetadata.indptr.size()));
    ASSERT_EQ(csrArrays.indices.array.length, static_cast<int64_t>(storedMetadata.indices.size()));
    ASSERT_EQ(csrArrays.edgeIDs->array.length, static_cast<int64_t>(storedMetadata.edgeIDs.size()));
    ASSERT_EQ(csrArrays.indptr.array.buffers[0], nullptr);
    ASSERT_EQ(csrArrays.indices.array.buffers[0], nullptr);
    ASSERT_EQ(csrArrays.edgeIDs->array.buffers[0], nullptr);
    ASSERT_EQ(csrArrays.indptr.array.buffers[1], storedMetadata.indptr.data());
    ASSERT_EQ(csrArrays.indices.array.buffers[1], storedMetadata.indices.data());
    ASSERT_EQ(csrArrays.edgeIDs->array.buffers[1], storedMetadata.edgeIDs.data());
}

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

    auto csrArrays = arrowResult->getCSRArrowArrays();
    ASSERT_STREQ(csrArrays.indptr.schema.format, "l");
    ASSERT_STREQ(csrArrays.indices.schema.format, "l");
    ASSERT_EQ(csrArrays.indptr.array.length, static_cast<int64_t>(metadata.indptr.size()));
    ASSERT_EQ(csrArrays.indices.array.length, static_cast<int64_t>(metadata.indices.size()));
    ASSERT_EQ(csrArrays.indptr.array.null_count, 0);
    ASSERT_EQ(csrArrays.indices.array.null_count, 0);
    ASSERT_EQ(csrArrays.indptr.array.buffers[0], nullptr);
    ASSERT_EQ(csrArrays.indices.array.buffers[0], nullptr);
    ASSERT_EQ(csrArrays.indptr.array.buffers[1], metadata.indptr.data());
    ASSERT_EQ(csrArrays.indices.array.buffers[1], metadata.indices.data());
    ASSERT_FALSE(csrArrays.edgeIDs.has_value());
    releaseCSRArrowArray(csrArrays.indptr);
    releaseCSRArrowArray(csrArrays.indices);

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

    auto csrArrays = arrowResult->getCSRArrowArrays();
    ASSERT_TRUE(csrArrays.edgeIDs.has_value());
    ASSERT_EQ(csrArrays.edgeIDs->array.length, static_cast<int64_t>(metadata.edgeIDs.size()));
    ASSERT_EQ(csrArrays.edgeIDs->array.buffers[0], nullptr);
    ASSERT_EQ(csrArrays.edgeIDs->array.buffers[1], metadata.edgeIDs.data());
    releaseCSRArrowArray(csrArrays.indptr);
    releaseCSRArrowArray(csrArrays.indices);
    releaseCSRArrowArray(*csrArrays.edgeIDs);

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
