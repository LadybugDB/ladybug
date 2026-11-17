add_lbug_test(wal_dump_test wal_dump_test.cpp)
add_dependencies(wal_dump_test wal_dump)
target_compile_definitions(wal_dump_test PRIVATE "WAL_DUMP_PATH=\"$<TARGET_FILE:wal_dump>\"")
