add_library(lbug_iceberg_connector
        OBJECT
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_secret_manager.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_connector.cpp
        iceberg_connector.cpp)

set(ICEBERG_EXTENSION_OBJECT_FILES
        ${ICEBERG_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_iceberg_connector>
        PARENT_SCOPE)
