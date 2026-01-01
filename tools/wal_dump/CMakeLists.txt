add_executable(wal_dump main.cpp)

target_link_libraries(wal_dump lbug)

install(TARGETS wal_dump)
