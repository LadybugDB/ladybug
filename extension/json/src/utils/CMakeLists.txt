add_library(lbug_json_utils
        OBJECT
        json_utils.cpp)

set(JSON_EXTENSION_OBJECT_FILES
        ${JSON_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_json_utils>
        PARENT_SCOPE)
