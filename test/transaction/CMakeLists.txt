add_lbug_test(current_time_test current_time_test.cpp)

# Also tested for coverage in connection_test.cpp
# but full testing requires some private APIs
add_lbug_test(transaction_test transaction_test.cpp checkpoint_test.cpp wal_test.cpp)

# Loaded through LOAD EXTENSION from a WAL replay test; resolves core symbols from the
# test process instead of linking lbug, which would duplicate core state.
add_library(lbug_test_registry_mutating_extension SHARED registry_mutating_extension.cpp)
target_include_directories(lbug_test_registry_mutating_extension PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
    ${CMAKE_BINARY_DIR}/src/include)
set_target_properties(lbug_test_registry_mutating_extension PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
if (WIN32)
    target_link_libraries(lbug_test_registry_mutating_extension PRIVATE lbug)
endif()
if (APPLE)
    set_target_properties(lbug_test_registry_mutating_extension PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup")
endif()
add_dependencies(transaction_test lbug_test_registry_mutating_extension)
target_compile_definitions(transaction_test PRIVATE
    "LBUG_TEST_REGISTRY_MUTATING_EXTENSION_PATH=\"$<TARGET_FILE:lbug_test_registry_mutating_extension>\"")
