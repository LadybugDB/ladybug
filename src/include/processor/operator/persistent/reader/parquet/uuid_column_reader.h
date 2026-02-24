#pragma once

#include "common/types/uuid.h"
#include "processor/operator/persistent/reader/parquet/resizable_buffer.h"
#include "templated_column_reader.h"

namespace lbug {
namespace processor {

struct UUIDValueConversion {
    static common::uuid dictRead(ByteBuffer& dict, uint32_t& offset, ColumnReader& /*reader*/) {
        return reinterpret_cast<common::uuid*>(dict.ptr)[offset];
    }

    static common::uuid ReadParquetUUID(const uint8_t* input);

    static common::uuid plainRead(ByteBuffer& bufferData, ColumnReader& /*reader*/);

    static void plainSkip(ByteBuffer& plain_data, ColumnReader& /*reader*/) {
        plain_data.inc(sizeof(common::uuid));
    }
};

class UUIDColumnReader : public TemplatedColumnReader<common::uuid, UUIDValueConversion> {
public:
    UUIDColumnReader(ParquetReader& reader, common::LogicalType dataType,
        const lbug_parquet::format::SchemaElement& schema_p, uint64_t file_idx_p,
        uint64_t maxDefine, uint64_t maxRepeat)
        : TemplatedColumnReader<common::uuid, UUIDValueConversion>(reader, std::move(dataType),
              schema_p, file_idx_p, maxDefine, maxRepeat) {};

protected:
    void dictionary(const std::shared_ptr<ResizeableBuffer>& dictionaryData,
        uint64_t numEntries) override;
};

} // namespace processor
} // namespace lbug
