find_package(Python3 3.9...4 REQUIRED)

file(GLOB WAL_TYPESPEC_RECORD_SPECS CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/storage/wal/typespec/records/*.tsp)
file(GLOB WAL_TYPESPEC_TEMPLATES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/storage/wal/typespec/templates/*.j2)

add_custom_target(generate_wal_typespec
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/generate_wal_typespec.py
        DEPENDS
                ${PROJECT_SOURCE_DIR}/scripts/generate_wal_typespec.py
                ${PROJECT_SOURCE_DIR}/src/storage/wal/typespec/common.tsp
                ${WAL_TYPESPEC_RECORD_SPECS}
                ${WAL_TYPESPEC_TEMPLATES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Generating WAL record files from TypeSpec"
        VERBATIM)

add_library(lbug_storage_wal
        OBJECT
        checksum_reader.cpp
        checksum_writer.cpp
        local_wal.cpp
        wal.cpp
        wal_record.cpp
        wal_replayer.cpp
        records/begin_transaction_record.cpp
        records/commit_record.cpp
        records/checkpoint_record.cpp
        records/create_catalog_entry_record.cpp
        records/create_index_record.cpp
        records/drop_catalog_entry_record.cpp
        records/alter_table_entry_record.cpp
        records/copy_table_record.cpp
        records/update_sequence_record.cpp
        records/table_insertion_record.cpp
        records/node_deletion_record.cpp
        records/node_update_record.cpp
        records/rel_deletion_record.cpp
        records/rel_detach_delete_record.cpp
        records/rel_update_record.cpp
        records/load_extension_record.cpp
        records/create_catalog_entry_record_replay.cpp
        records/create_index_record_replay.cpp
        records/drop_catalog_entry_record_replay.cpp
        records/alter_table_entry_record_replay.cpp
        records/table_insertion_record_replay.cpp
        records/node_deletion_record_replay.cpp
        records/node_update_record_replay.cpp
        records/rel_deletion_record_replay.cpp
        records/rel_detach_delete_record_replay.cpp
        records/rel_update_record_replay.cpp
        records/copy_table_record_replay.cpp
        records/update_sequence_record_replay.cpp
        records/load_extension_record_replay.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_storage_wal>
        PARENT_SCOPE)
