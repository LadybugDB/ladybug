add_library(lbug_service
        OBJECT
        connection_pool.cpp
        http_service_manager.cpp
        lbug_server.cpp
        query_result_json_serializer.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_service>
        PARENT_SCOPE)
