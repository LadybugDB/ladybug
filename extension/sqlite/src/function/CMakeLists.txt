add_library(lbug_sqlite_scan
        OBJECT
        sqlite_scan.cpp)

set(SQLITE_EXTENSION_OBJECT_FILES
        ${SQLITE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_sqlite_scan>
        PARENT_SCOPE)
