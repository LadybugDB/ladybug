add_library(lbug_common_arrow
        OBJECT
        arrow_array_scan.cpp
        arrow_converter.cpp
        arrow_schema_metadata_generic_decoder.cpp
        arrow_schema_metadata_snowflake_decoder.cpp
        arrow_schema_metadata_utils.cpp
        arrow_schema_metadata.cpp
        arrow_null_mask_tree.cpp
        arrow_row_batch.cpp
        arrow_type.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_common_arrow>
        PARENT_SCOPE)
