add_library(lbug_duckdb_storage
        OBJECT
        attached_duckdb_database.cpp
        duckdb_storage.cpp)

set(DUCKDB_EXTENSION_OBJECT_FILES
        ${DUCKDB_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_duckdb_storage>
        PARENT_SCOPE)
