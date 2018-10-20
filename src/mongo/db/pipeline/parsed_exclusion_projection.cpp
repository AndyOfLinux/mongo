
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/parsed_exclusion_projection.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace parsed_aggregation_projection {

//
// ExclusionNode.
//

ExclusionNode::ExclusionNode(ProjectionArrayRecursionPolicy recursionPolicy, std::string pathToNode)
    : _arrayRecursionPolicy(recursionPolicy), _pathToNode(std::move(pathToNode)) {}

Document ExclusionNode::serialize() const {
    MutableDocument output;
    for (auto&& excludedField : _excludedFields) {
        output.addField(excludedField, Value(false));
    }

    for (auto&& childPair : _children) {
        output.addField(childPair.first, Value(childPair.second->serialize()));
    }
    return output.freeze();
}

void ExclusionNode::excludePath(FieldPath path) {
    if (path.getPathLength() == 1) {
        _excludedFields.insert(path.fullPath());
        return;
    }
    addOrGetChild(path.getFieldName(0))->excludePath(path.tail());
}

Document ExclusionNode::applyProjection(const Document& input) const {
    MutableDocument output(input);
    for (auto&& field : _excludedFields) {
        output.remove(field);
    }
    for (auto&& childPair : _children) {
        output[childPair.first] = childPair.second->applyProjectionToValue(input[childPair.first]);
    }
    return output.freeze();
}

ExclusionNode* ExclusionNode::addOrGetChild(FieldPath fieldPath) {
    invariant(fieldPath.getPathLength() == 1);
    auto child = getChild(fieldPath.fullPath());
    return child ? child : addChild(fieldPath.fullPath());
}

ExclusionNode* ExclusionNode::getChild(std::string field) const {
    auto it = _children.find(field);
    return it == _children.end() ? nullptr : it->second.get();
}

ExclusionNode* ExclusionNode::addChild(std::string field) {
    auto pathToChild = _pathToNode.empty() ? field : _pathToNode + "." + field;

    auto emplacedPair = _children.emplace(std::make_pair(
        std::move(field), stdx::make_unique<ExclusionNode>(_arrayRecursionPolicy, pathToChild)));

    // emplacedPair is a pair<iterator position, bool inserted>.
    invariant(emplacedPair.second);

    return emplacedPair.first->second.get();
}

Value ExclusionNode::applyProjectionToValue(Value val) const {
    switch (val.getType()) {
        case BSONType::Object:
            return Value(applyProjection(val.getDocument()));
        case BSONType::Array: {
            // Apply exclusion to each element of the array. Note that numeric paths aren't treated
            // specially, and we will always apply the projection to each element in the array.
            //
            // For example, applying the projection {"a.1": 0} to the document
            // {a: [{b: 0, "1": 0}, {b: 1, "1": 1}]} will not result in {a: [{b: 0, "1": 0}]}, but
            // instead will result in {a: [{b: 0}, {b: 1}]}.
            std::vector<Value> values = val.getArray();
            for (auto it = values.begin(); it != values.end(); it++) {
                // If this is a nested array and our policy is to not recurse, leave the array
                // as-is. Otherwise, descend into the array and project each element individually.
                const bool shouldSkip = it->isArray() &&
                    _arrayRecursionPolicy ==
                        ProjectionArrayRecursionPolicy::kDoNotRecurseNestedArrays;
                *it = (shouldSkip ? *it : applyProjectionToValue(*it));
            }
            return Value(std::move(values));
        }
        default:
            return val;
    }
}

void ExclusionNode::addModifiedPaths(std::set<std::string>* modifiedPaths) const {
    for (auto&& excludedField : _excludedFields) {
        modifiedPaths->insert(FieldPath::getFullyQualifiedPath(_pathToNode, excludedField));
    }

    for (auto&& childPair : _children) {
        childPair.second->addModifiedPaths(modifiedPaths);
    }
}

//
// ParsedExclusionProjection.
//

Document ParsedExclusionProjection::serializeTransformation(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return _root->serialize();
}

Document ParsedExclusionProjection::applyProjection(const Document& inputDoc) const {
    return _root->applyProjection(inputDoc);
}

void ParsedExclusionProjection::parse(const BSONObj& spec, ExclusionNode* node, size_t depth) {
    bool idSpecified = false;

    for (auto elem : spec) {
        const auto fieldName = elem.fieldNameStringData();

        // A $ should have been detected by ParsedAggregationProjection before we get here.
        invariant(fieldName[0] != '$');

        // Track whether the projection spec specifies a desired behavior for the _id field.
        idSpecified = idSpecified || fieldName == "_id"_sd || fieldName.startsWith("_id."_sd);

        switch (elem.type()) {
            case BSONType::Bool:
            case BSONType::NumberInt:
            case BSONType::NumberLong:
            case BSONType::NumberDouble:
            case BSONType::NumberDecimal: {
                // We have already verified this is an exclusion projection. _id is the only field
                // which is permitted to be explicitly included here.
                invariant(!elem.trueValue() || elem.fieldNameStringData() == "_id"_sd);
                if (!elem.trueValue()) {
                    node->excludePath(FieldPath(fieldName));
                }
                break;
            }
            case BSONType::Object: {
                // This object represents a nested projection specification, like the sub-object in
                // {a: {b: 0, c: 0}} or {"a.b": {c: 0}}.
                ExclusionNode* child;

                if (elem.fieldNameStringData().find('.') == std::string::npos) {
                    child = node->addOrGetChild(fieldName);
                } else {
                    // A dotted field is not allowed in a sub-object, and should have been detected
                    // in ParsedAggregationProjection's parsing before we get here.
                    invariant(depth == 0);

                    // We need to keep adding children to our tree until we create a child that
                    // represents this dotted path.
                    child = node;
                    auto fullPath = FieldPath(fieldName);
                    while (fullPath.getPathLength() > 1) {
                        child = child->addOrGetChild(fullPath.getFieldName(0));
                        fullPath = fullPath.tail();
                    }
                    // It is illegal to construct an empty FieldPath, so the above loop ends one
                    // iteration too soon. Add the last path here.
                    child = child->addOrGetChild(fullPath.fullPath());
                }

                parse(elem.Obj(), child, depth + 1);
                break;
            }
            default: { MONGO_UNREACHABLE; }
        }
    }

    // If _id was not specified, then doing nothing will cause it to be included. If the default _id
    // policy is kExcludeId, we add a new entry for _id to the ExclusionNode tree here.
    if (!idSpecified && _defaultIdPolicy == ProjectionDefaultIdPolicy::kExcludeId) {
        _root->excludePath({FieldPath("_id")});
    }
}

}  // namespace parsed_aggregation_projection
}  // namespace mongo
