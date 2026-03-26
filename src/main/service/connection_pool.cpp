#include "main/service/connection_pool.h"

namespace lbug {
namespace main {

// ConnectionGuard

ConnectionGuard::ConnectionGuard(ConnectionPool* pool, Connection* conn)
    : pool_(pool), conn_(conn) {}

ConnectionGuard::~ConnectionGuard() {
    if (pool_ && conn_) {
        pool_->release(conn_);
    }
}

ConnectionGuard::ConnectionGuard(ConnectionGuard&& other) noexcept
    : pool_(other.pool_), conn_(other.conn_) {
    other.pool_ = nullptr;
    other.conn_ = nullptr;
}

ConnectionGuard& ConnectionGuard::operator=(ConnectionGuard&& other) noexcept {
    if (this != &other) {
        if (pool_ && conn_) {
            pool_->release(conn_);
        }
        pool_ = other.pool_;
        conn_ = other.conn_;
        other.pool_ = nullptr;
        other.conn_ = nullptr;
    }
    return *this;
}

// ConnectionPool

ConnectionPool::ConnectionPool(Database* db, uint32_t poolSize) {
    connections_.reserve(poolSize);
    available_.reserve(poolSize);
    for (uint32_t i = 0; i < poolSize; ++i) {
        connections_.push_back(std::make_unique<Connection>(db));
        available_.push_back(connections_.back().get());
    }
}

ConnectionGuard ConnectionPool::acquire() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return !available_.empty(); });
    auto* conn = available_.back();
    available_.pop_back();
    return ConnectionGuard(this, conn);
}

void ConnectionPool::release(Connection* conn) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        available_.push_back(conn);
    }
    cv_.notify_one();
}

} // namespace main
} // namespace lbug
