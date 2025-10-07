add_library(lbug_azure_filesystem
        OBJECT
        azure_file_system.cpp)

set(AZURE_EXTENSION_OBJECT_FILES
        ${AZURE_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_azure_filesystem>
        PARENT_SCOPE)
