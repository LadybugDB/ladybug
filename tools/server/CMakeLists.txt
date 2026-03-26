include_directories(
        ${PROJECT_SOURCE_DIR}/third_party/taywee_args/include
)

add_executable(lbug_server server_main.cpp)

if(WIN32)
    target_compile_definitions(lbug_server PRIVATE LBUG_STATIC_DEFINE)
endif()

if(NOT MSVC)
    set_target_properties(lbug_server PROPERTIES OUTPUT_NAME lbug_server)
endif()

target_link_libraries(lbug_server lbug)
if(ENABLE_BACKTRACES)
    target_link_libraries(lbug_server register_backtrace_signal_handler)
endif()
if(MSVC)
    target_link_libraries(lbug_server ws2_32)
endif()

install(TARGETS lbug_server)
