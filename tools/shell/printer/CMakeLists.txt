include_directories(
    ${PROJECT_SOURCE_DIR}/third_party/yyjson/src
)

add_library(lbug_shell_printer
        OBJECT
        json_printer.cpp
        printer_factory.cpp
        ${PROJECT_SOURCE_DIR}/extension/json/src/type/json_type.cpp
)

set(LBUG_SHELL_OBJECT_FILES
        ${LBUG_SHELL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_shell_printer>
        PARENT_SCOPE)
