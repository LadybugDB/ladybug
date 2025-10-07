add_library(lbug_json_table_functions
        OBJECT
        json_scan.cpp)

set(JSON_EXTENSION_OBJECT_FILES
        ${JSON_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_json_table_functions>
        PARENT_SCOPE)
