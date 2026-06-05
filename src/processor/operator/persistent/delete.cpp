#include "processor/operator/persistent/delete.h"

namespace lbug {
namespace processor {

std::string DeleteNodePrintInfo::toString() const {
    std::string result = "Type: ";
    switch (deleteType) {
    case common::DeleteNodeType::DELETE:
        result += "Delete Nodes";
        break;
    case common::DeleteNodeType::DETACH_DELETE:
        result += "Detach Delete Nodes";
        break;
    }
    result += ", From: ";
    for (const auto& expression : expressions) {
        result += expression->toString() + ", ";
    }
    return result;
}

std::string DeleteRelPrintInfo::toString() const {
    std::string result = "Type: Delete Relationships, From: ";
    for (const auto& expression : expressions) {
        result += expression->toString() + ", ";
    }
    return result;
}

void DeleteNode::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    for (auto& executor : executors) {
        executor->init(resultSet, context);
    }
}

bool DeleteNode::getNextTuplesInternal(ExecutionContext* context) {
    if (!children[0]->getNextTuple(context)) {
        for (auto& executor : executors) {
            executor->finalize(context);
        }
        return false;
    }
    for (auto& executor : executors) {
        executor->delete_(context);
    }
    return true;
}

void DeleteNode::finalizeInternal(ExecutionContext* context) {
    for (auto& executor : executors) {
        executor->finalize(context);
    }
}

void DeleteRel::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    for (auto& executor : executors) {
        executor->init(resultSet, context);
    }
}

bool DeleteRel::getNextTuplesInternal(ExecutionContext* context) {
    if (!children[0]->getNextTuple(context)) {
        return false;
    }
    for (auto& executor : executors) {
        executor->delete_(context);
    }
    return true;
}

} // namespace processor
} // namespace lbug
