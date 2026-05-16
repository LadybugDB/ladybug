add_library(lbug_storage_index
        OBJECT
        art_index.cpp
        hash_index.cpp
        in_mem_hash_index.cpp
        index.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_storage_index>
        PARENT_SCOPE)
