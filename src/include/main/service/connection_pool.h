#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "main/connection.h"

namespace lbug {
namespace main {

class ConnectionPool;

class ConnectionGuard {
public:
    ConnectionGuard(ConnectionPool* pool, Connection* conn);
    ~ConnectionGuard();
    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;
    ConnectionGuard(ConnectionGuard&& other) noexcept;
    ConnectionGuard& operator=(ConnectionGuard&& other) noexcept;

    Connection* get() const { return conn_; }
    Connection* operator->() const { return conn_; }

private:
    ConnectionPool* pool_;
    Connection* conn_;
};

class ConnectionPool {
    friend class ConnectionGuard;

public:
    ConnectionPool(Database* db, uint32_t poolSize);
    ~ConnectionPool() = default;

    ConnectionGuard acquire();

private:
    void release(Connection* conn);

    std::vector<std::unique_ptr<Connection>> connections_;
    std::vector<Connection*> available_;
    std::mutex mu_;
    std::condition_variable cv_;
};

} // namespace main
} // namespace lbug
