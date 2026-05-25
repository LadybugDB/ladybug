include_directories(
    ${PROJECT_SOURCE_DIR}/third_party/yyjson/src
)

add_library(lbug_shell_printer
        OBJECT
        json_printer.cpp
        printer_factory.cpp
)
if(NOT MSVC OR TARGET lbug)
    target_sources(lbug_shell_printer PRIVATE ${PROJECT_SOURCE_DIR}/src/common/types/json_type.cpp)
endif()
if(WIN32 AND TARGET lbug)
    target_compile_definitions(lbug_shell_printer PUBLIC LBUG_STATIC_DEFINE)
endif()

set(LBUG_SHELL_OBJECT_FILES
        ${LBUG_SHELL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_shell_printer>
        PARENT_SCOPE)
