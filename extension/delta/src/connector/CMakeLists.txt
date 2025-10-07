add_library(lbug_delta_connector
        OBJECT
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_secret_manager.cpp
        ${PROJECT_SOURCE_DIR}/extension/duckdb/src/connector/duckdb_connector.cpp
        delta_connector.cpp)

set(DELTA_EXTENSION_OBJECT_FILES
        ${DELTA_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_delta_connector>
        PARENT_SCOPE)
