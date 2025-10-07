add_library(lbug_unity_catalog_options
        OBJECT
        unity_catalog_options.cpp)

set(UNITY_CATALOG_EXTENSION_OBJECT_FILES
        ${UNITY_CATALOG_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_unity_catalog_options>
        PARENT_SCOPE)
