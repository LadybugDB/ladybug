#include "catalog/property_definition_collection.h"

#include <map>
#include <mutex>
#include <sstream>

#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/string_utils.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace catalog {

std::vector<binder::PropertyDefinition> PropertyDefinitionCollection::getDefinitions() const {
    std::vector<binder::PropertyDefinition> propertyDefinitions;
    for (auto i = 0u; i < nextPropertyID; i++) {
        if (definitions.contains(i)) {
            propertyDefinitions.push_back(definitions.at(i).copy());
        }
    }
    return propertyDefinitions;
}

const PropertyDefinition& PropertyDefinitionCollection::getDefinition(
    const std::string& name) const {
    return getDefinition(getPropertyID(name));
}

const PropertyDefinition& PropertyDefinitionCollection::getDefinition(
    property_id_t propertyID) const {
    DASSERT(definitions.contains(propertyID));
    return definitions.at(propertyID);
}

column_id_t PropertyDefinitionCollection::getColumnID(const std::string& name) const {
    return getColumnID(getPropertyID(name));
}

column_id_t PropertyDefinitionCollection::getColumnID(property_id_t propertyID) const {
    std::shared_lock lck{*columnIDsMtx};
    DASSERT(columnIDs.contains(propertyID));
    return columnIDs.at(propertyID);
}

void PropertyDefinitionCollection::vacuumColumnIDs(column_id_t nextColumnID) {
    std::unique_lock lck{*columnIDsMtx};
    // Checkpoint calls vacuum unconditionally, even when no columns were dropped.
    // Avoid mutating columnIDs when already compact so concurrent planning reads
    // (getColumnID) don't race with clear()/emplace() (see #1160).
    {
        column_id_t expected = nextColumnID;
        bool alreadyCompact = (columnIDs.size() == definitions.size());
        if (alreadyCompact) {
            for (auto& [propertyID, definition] : definitions) {
                auto it = columnIDs.find(propertyID);
                if (it == columnIDs.end() || it->second != expected++) {
                    alreadyCompact = false;
                    break;
                }
            }
            if (alreadyCompact && this->nextColumnID == expected) {
                return;
            }
        }
    }
    this->nextColumnID = nextColumnID;
    columnIDs.clear();
    for (auto& [propertyID, definition] : definitions) {
        columnIDs.emplace(propertyID, this->nextColumnID++);
    }
}

void PropertyDefinitionCollection::add(const PropertyDefinition& definition) {
    // NB: definitions/name map remain lock-free (pre-existing DDL-vs-read race);
    // the lock here only serializes columnIDs vs concurrent getColumnID/vacuum.
    std::unique_lock lck{*columnIDsMtx};
    auto propertyID = nextPropertyID++;
    columnIDs.emplace(propertyID, nextColumnID++);
    // Release before touching unguarded maps to keep the critical section minimal.
    lck.unlock();
    definitions.emplace(propertyID, definition.copy());
    nameToPropertyIDMap.emplace(definition.getName(), propertyID);
}

void PropertyDefinitionCollection::drop(const std::string& name) {
    DASSERT(contains(name));
    auto propertyID = nameToPropertyIDMap.at(name);
    definitions.erase(propertyID);
    {
        std::unique_lock lck{*columnIDsMtx};
        columnIDs.erase(propertyID);
    }
    nameToPropertyIDMap.erase(name);
}

void PropertyDefinitionCollection::rename(const std::string& name, const std::string& newName) {
    DASSERT(contains(name));
    auto idx = nameToPropertyIDMap.at(name);
    definitions[idx].rename(newName);
    nameToPropertyIDMap.erase(name);
    nameToPropertyIDMap.insert({newName, idx});
}

column_id_t PropertyDefinitionCollection::getMaxColumnID() const {
    std::shared_lock lck{*columnIDsMtx};
    column_id_t maxID = 0;
    for (auto [_, id] : columnIDs) {
        if (id > maxID) {
            maxID = id;
        }
    }
    return maxID;
}

property_id_t PropertyDefinitionCollection::getPropertyID(const std::string& name) const {
    DASSERT(contains(name));
    return nameToPropertyIDMap.at(name);
}

std::string PropertyDefinitionCollection::toCypher() const {
    std::stringstream ss;
    for (auto& [_, def] : definitions) {
        auto& dataType = def.getType();
        // Avoid exporting internal ID
        if (dataType.getPhysicalType() == PhysicalTypeID::INTERNAL_ID) {
            continue;
        }
        auto typeStr = dataType.toString();
        StringUtils::replaceAll(typeStr, ":", " ");
        if (typeStr.find("MAP") != std::string::npos) {
            StringUtils::replaceAll(typeStr, "  ", ",");
        }
        ss << common::StringUtils::quoteIdentifier(def.getName()) << " " << typeStr << ",";
    }
    return ss.str();
}

void PropertyDefinitionCollection::serialize(Serializer& serializer) const {
    // Vacuum only mutates nextColumnID/columnIDs; hold shared lock for those.
    std::shared_lock lck{*columnIDsMtx};
    serializer.writeDebuggingInfo("nextColumnID");
    serializer.serializeValue(nextColumnID);
    serializer.writeDebuggingInfo("nextPropertyID");
    serializer.serializeValue(nextPropertyID);
    serializer.writeDebuggingInfo("definitions");
    serializer.serializeMap(definitions);
    serializer.writeDebuggingInfo("columnIDs");
    serializer.serializeUnorderedMap(columnIDs);
}

PropertyDefinitionCollection PropertyDefinitionCollection::deserialize(Deserializer& deserializer) {
    std::string debuggingInfo;
    column_id_t nextColumnID = 0;
    deserializer.validateDebuggingInfo(debuggingInfo, "nextColumnID");
    deserializer.deserializeValue(nextColumnID);
    property_id_t nextPropertyID = 0;
    deserializer.validateDebuggingInfo(debuggingInfo, "nextPropertyID");
    deserializer.deserializeValue(nextPropertyID);
    std::map<property_id_t, PropertyDefinition> definitions;
    deserializer.validateDebuggingInfo(debuggingInfo, "definitions");
    deserializer.deserializeMap(definitions);
    std::unordered_map<property_id_t, column_id_t> columnIDs;
    deserializer.validateDebuggingInfo(debuggingInfo, "columnIDs");
    deserializer.deserializeUnorderedMap(columnIDs);
    auto collection = PropertyDefinitionCollection();
    for (auto& [propertyID, definition] : definitions) {
        collection.nameToPropertyIDMap.insert({definition.getName(), propertyID});
    }
    collection.nextColumnID = nextColumnID;
    collection.nextPropertyID = nextPropertyID;
    collection.definitions = std::move(definitions);
    collection.columnIDs = std::move(columnIDs);
    return collection;
}

} // namespace catalog
} // namespace lbug
