add_library(lbug_json_cast_functions
        OBJECT
        json_cast.cpp)

set(JSON_EXTENSION_OBJECT_FILES
        ${JSON_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_json_cast_functions>
        PARENT_SCOPE)
