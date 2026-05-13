add_library(lbug_common_exception
        OBJECT
        exception.cpp
        message.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_common_exception>
        PARENT_SCOPE)
