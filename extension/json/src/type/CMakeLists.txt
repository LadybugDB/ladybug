add_library(lbug_json_type
        OBJECT
        json_type.cpp)

set(JSON_EXTENSION_OBJECT_FILES
        ${JSON_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_json_type>
        PARENT_SCOPE)
