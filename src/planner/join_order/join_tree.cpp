#include "planner/join_order/join_tree.h"

namespace lbug {
namespace planner {

std::string TreeNodeTypeUtils::toString(TreeNodeType type) {
    switch (type) {
    case TreeNodeType::NODE_SCAN:
        return "NODE_SCAN";
    case TreeNodeType::REL_SCAN:
        return "REL_SCAN";
    case TreeNodeType::BINARY_JOIN:
        return "BINARY_JOIN";
    case TreeNodeType::MULTIWAY_JOIN:
        return "MULTIWAY_JOIN";
    default:
        LBUG_UNREACHABLE;
    }
}

void ExtraScanTreeNodeInfo::merge(const ExtraScanTreeNodeInfo& other) {
    LBUG_ASSERT(other.nodeInfo == nullptr && other.relInfos.size() == 1);
    relInfos.push_back(other.relInfos[0]);
}

std::string JoinTreeNode::toString() const {
    switch (type) {
    case TreeNodeType::NODE_SCAN:
    case TreeNodeType::REL_SCAN: {
        auto& scanInfo = extraInfo->constCast<ExtraScanTreeNodeInfo>();
        auto result = "Scan(" + scanInfo.nodeInfo->nodeOrRel->toString();
        for (auto relInfo : scanInfo.relInfos) {
            result += "," + relInfo.nodeOrRel->toString();
        }
        result += ")";
        return result;
    }
    case TreeNodeType::BINARY_JOIN: {
        LBUG_ASSERT(children.size() == 2);
        return "JOIN(" + children[0]->toString() + "," + children[1]->toString() + ")";
    }
    case TreeNodeType::MULTIWAY_JOIN: {
        LBUG_ASSERT(!children.empty());
        auto result = "MULTI_JOIN(" + children[0]->toString();
        for (auto i = 1u; i < children.size(); ++i) {
            result += "," + children[i]->toString();
        }
        return result;
    }
    default: {
        LBUG_UNREACHABLE;
    }
    }
}

} // namespace planner
} // namespace lbug
