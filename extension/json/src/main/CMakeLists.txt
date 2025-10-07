add_library(lbug_json_main
        OBJECT
        json_extension.cpp)

set(JSON_EXTENSION_OBJECT_FILES
        ${JSON_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_json_main>
        PARENT_SCOPE)
