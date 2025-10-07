add_library(lbug_postgres_function
        OBJECT
        sql_query.cpp)

set(POSTGRES_EXTENSION_OBJECT_FILES
        ${POSTGRES_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_postgres_function>
        PARENT_SCOPE)
