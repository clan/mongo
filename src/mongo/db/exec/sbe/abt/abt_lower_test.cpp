/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo::optimizer {
namespace {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/exec/sbe"};
using GoldenTestContext = unittest::GoldenTestContext;
using GoldenTestConfig = unittest::GoldenTestConfig;
using namespace unit_test_abt_literals;
class ABTPlanGeneration : public unittest::Test {
protected:
    ProjectionName scanLabel = ProjectionName{"scan0"_sd};
    LoweringNodeToGroupPropsMap _nodeMap;
    // This can be modified by tests that need other labels.
    FieldProjectionMap _fieldProjMap{{}, {scanLabel}, {}};
    sbe::InputParamToSlotMap inputParamToSlotMap;

    void runExpressionVariation(GoldenTestContext& gctx, const std::string& name, const ABT& n) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }
        stream << "==== VARIATION: " << name << " ====" << std::endl;
        stream << "-- INPUT:" << std::endl;
        stream << ExplainGenerator::explainV2(n) << std::endl;
        stream << "-- OUTPUT:" << std::endl;
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids;
        auto expr = optimizer::SBEExpressionLowering{env,
                                                     map,
                                                     runtimeEnv,
                                                     ids,
                                                     inputParamToSlotMap,
                                                     nullptr /* scanDefs */,
                                                     nullptr /* LoweringNodeProps */}
                        .optimize(n);
        stream << expr->toString() << std::endl;
    }

    // SBE plans with scans print UUIDs. As there are no collections in these tests the UUIDS
    // are generated by the ScanStage. Remove all UUIDs them so they don't throw off the test
    // output.
    std::string stripUUIDs(std::string str) {
        size_t atIndex = -1;  // size_t is unsigned, but we just want atIndex+1 == 0 in the first
                              // loop, so underflow/overflow is fine.
        while ((atIndex = str.find('@', atIndex + 1)) != std::string::npos) {
            // UUIDs are printed with a leading '@' character, and in quotes.
            // Expect a quote after the '@' in the plan.
            ASSERT_EQUALS('\"', str[atIndex + 1]);
            // The next character is a quote. Find the close quote.
            auto closeQuote = str.find('"', atIndex + 2);
            str = str.substr(0, atIndex + 2) + "<collUUID>" + str.substr(closeQuote, str.length());
        }

        return str;
    }

    void runNodeVariation(GoldenTestContext& gctx,
                          const std::string& name,
                          const ABT& n,
                          sbe::RuntimeEnvironment* runtimeEnv,
                          sbe::value::SlotIdGenerator* ids,
                          stdx::unordered_map<std::string, LoweringScanDefinition> scanDefs =
                              stdx::unordered_map<std::string, LoweringScanDefinition>()) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }
        stream << "==== VARIATION: " << name << " ====" << std::endl;
        stream << "-- INPUT:" << std::endl;
        stream << ExplainGenerator::explainV2(n) << std::endl;
        stream << "-- OUTPUT:" << std::endl;
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        boost::optional<sbe::value::SlotId> ridSlot;

        scanDefs.insert({"collName", buildLoweringScanDefinition()});
        scanDefs.insert({"otherColl", buildLoweringScanDefinition()});

        auto planStage = SBENodeLowering{env,
                                         *runtimeEnv,
                                         *ids,
                                         inputParamToSlotMap,
                                         std::move(scanDefs),
                                         _nodeMap,
                                         1 /* numberOfPartitions */,
                                         nullptr /* yieldPolicy */}
                             .optimize(n, map, ridSlot);
        sbe::DebugPrinter printer;
        stream << stripUUIDs(printer.print(*planStage)) << std::endl;

        // After a variation is run, presumably any more variations in the test will use a new tree,
        // so reset the node map.
        _nodeMap = LoweringNodeToGroupPropsMap{};
        _fieldProjMap = {{}, {ProjectionName{scanLabel}}, {}};
        lastNodeGenerated = 0;
    }

    void runNodeVariation(GoldenTestContext& gctx,
                          const std::string& name,
                          const ABT& n,
                          stdx::unordered_map<std::string, LoweringScanDefinition> scanDefs =
                              stdx::unordered_map<std::string, LoweringScanDefinition>()) {
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids;
        runNodeVariation(gctx, name, n, &runtimeEnv, &ids, scanDefs);
    }

    std::string autoUpdateExpressionVariation(const ABT& n) {
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids;
        auto expr = optimizer::SBEExpressionLowering{env,
                                                     map,
                                                     runtimeEnv,
                                                     ids,
                                                     inputParamToSlotMap,
                                                     nullptr /* scanDefs */,
                                                     nullptr /* LoweringNodeProps */}
                        .optimize(n);
        return expr->toString();
    }

    LoweringScanDefinition buildLoweringScanDefinition(
        std::vector<LoweringIndexCollationEntry> shardKey = {}) {
        LoweringScanDefinition::ScanDefOptions opts;
        opts.insert({"type", "mongod"});
        bool exists = true;
        return LoweringScanDefinition{
            DatabaseNameUtil::deserialize(
                boost::none, "test", SerializationContext::stateDefault()),
            opts,
            UUID::gen(),
            exists,
            shardKey};
    }

    // Does not add the node to the Node map, must be called inside '_node()'.
    ABT scanForTest(std::string coll = "collName") {
        return make<PhysicalScanNode>(_fieldProjMap, coll, false);
    }

    auto getNextNodeID() {
        return lastNodeGenerated++;
    }

    auto makeNodeProp() {
        LoweringNodeProps n{._planNodeId = getNextNodeID(),
                            ._indexScanDefName = boost::none,
                            ._projections = boost::none,
                            ._hasLimitSkip = false,
                            ._limit = 0,
                            ._skip = 0,
                            ._ridProjName = boost::none};
        return n;
    }
    void runPathLowering(ABT& tree) {
        auto env = VariableEnvironment::build(tree);
        auto prefixId = PrefixId::createForTests();
        runPathLowering(env, prefixId, tree);
    }

    /**
     * Run passed in ABT through path lowering and return the same ABT. Useful for constructing
     * physical ABTs in-line for lowering tests.
     */
    ABT&& _path(ABT&& tree) {
        runPathLowering(tree);
        return std::move(tree);
    }

    /**
     * Register the passed-in ABT in the test's node map and return the same ABT. Useful for
     * constructing physical ABTs in-line for lowering tests.
     */
    ABT&& _node(ABT&& tree, std::initializer_list<ProjectionName> requiredProjections = {}) {
        auto props = makeNodeProp();
        props._projections = ProjectionNameOrderPreservingSet{requiredProjections};

        _nodeMap.insert_or_assign(tree.cast<Node>(), std::move(props));
        return std::move(tree);
    }

    ABT&& _node(ABT&& tree, LoweringNodeProps n) {
        _nodeMap.insert({tree.cast<Node>(), n});
        return std::move(tree);
    }

    void runPathLowering(VariableEnvironment& env, PrefixId& prefixId, ABT& tree) {
        // Run rewriters while things change
        bool changed = false;
        do {
            changed = false;
            if (PathLowering{prefixId}.optimize(tree)) {
                changed = true;
            }
            if (ConstEval{env}.optimize(tree)) {
                changed = true;
            }
        } while (changed);
    }

    ABT createBindings(std::vector<std::pair<std::string, std::string>> bindingList,
                       ABT source,
                       std::string sourceBinding) {
        for (auto [fieldName, bindingName] : bindingList) {
            auto field =
                make<EvalPath>(make<PathGet>(FieldNameType(fieldName), make<PathIdentity>()),
                               make<Variable>(ProjectionName(sourceBinding)));
            runPathLowering(field);
            ABT evalNode = make<EvaluationNode>(
                ProjectionName(bindingName), std::move(field), std::move(source));
            source = std::move(_node(std::move(evalNode)));
        }
        return source;
    }

    // Create bindings (as above) and also create a scan node source.
    ABT createBindings(std::vector<std::pair<std::string, std::string>> bindingList) {
        return createBindings(bindingList, _node(scanForTest()), "scan0");
    }

    ABT makeEquals(ABT lhs, ABT rhs) {
        return _path(
            make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Eq, rhs)), lhs));
    }

    ABT makeEquals(StringData lhs, ABT rhs) {
        return makeEquals(make<Variable>(ProjectionName(lhs)), rhs);
    }

    boost::optional<sbe::value::SlotId> getSlotId(MatchExpression::InputParamId paramId) const {
        auto it = inputParamToSlotMap.find(paramId);
        if (it != inputParamToSlotMap.end()) {
            return it->second;
        }
        return boost::none;
    }

private:
    int32_t lastNodeGenerated = 0;
};

TEST_F(ABTPlanGeneration, LowerShardFiltering) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    const std::string shardKeyName = "SHARDKEYNAME";
    {
        // In shard filtering-related tests, mock the global and unconditional creation of a shard
        // filtering slot in the runtime environment.
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids = sbe::value::SlotIdGenerator{};
        runtimeEnv.registerSlot(
            kshardFiltererSlotName, sbe::value::TypeTags::Nothing, 0, false, &ids);
        // Create node properties which allow the lowering to access information about the Shard
        // Key.
        LoweringNodeProps filterLoweringNodeProps = makeNodeProp();
        filterLoweringNodeProps._indexScanDefName = shardKeyName;

        stdx::unordered_map<std::string, LoweringScanDefinition> scanDefs{std::make_pair(
            shardKeyName,
            buildLoweringScanDefinition(
                {LoweringIndexCollationEntry{_get("a", _id())._n, CollationOp::Ascending},
                 LoweringIndexCollationEntry{_get("b", _id())._n, CollationOp::Ascending},
                 LoweringIndexCollationEntry{_get("c", _id())._n, CollationOp::Ascending}}))};

        // Create the ABT to be lowered, starting with the shardFilter FunctionCall.
        auto functionCallNode = make<FunctionCall>(
            "shardFilter",
            makeSeq(make<Variable>("proj0"), make<Variable>("proj1"), make<Variable>("proj2")));

        auto filterNode =
            _node(make<FilterNode>(
                      std::move(functionCallNode),
                      _node(make<PhysicalScanNode>(
                          FieldProjectionMap{{},
                                             {ProjectionName{"scan0"}},
                                             {{FieldNameType{"a"}, ProjectionName{"proj0"}},
                                              {FieldNameType{"b"}, ProjectionName{"proj1"}},
                                              {FieldNameType{"c"}, ProjectionName{"proj2"}}}},
                          "collName",
                          false))),
                  filterLoweringNodeProps);
        runNodeVariation(
            ctx, "Shard Filtering with Top Level Fields", filterNode, &runtimeEnv, &ids, scanDefs);
    }

    {
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids = sbe::value::SlotIdGenerator{};
        runtimeEnv.registerSlot(
            kshardFiltererSlotName, sbe::value::TypeTags::Nothing, 0, false, &ids);

        LoweringNodeProps filterLoweringNodeProps = makeNodeProp();
        filterLoweringNodeProps._indexScanDefName = shardKeyName;

        // "a.b" is the shard Key in this variation of the test.
        auto pathABT = _get("a", _get("b", _id()))._n;
        stdx::unordered_map<std::string, LoweringScanDefinition> scanDefs{
            std::make_pair(shardKeyName,
                           buildLoweringScanDefinition(
                               {LoweringIndexCollationEntry{pathABT, CollationOp::Ascending}}))};

        auto evalPathABT =
            make<EvalPath>(std::move(pathABT), make<Variable>(ProjectionName("scan0")));
        auto evalNode = _node(make<EvaluationNode>(
            ProjectionName("proj0"), _path(std::move(evalPathABT)), _node(scanForTest())));

        // Create the ABT to be lowered, starting with the shardFilter FunctionCall.
        auto functionCallNode = make<FunctionCall>("shardFilter", makeSeq(make<Variable>("proj0")));
        auto filterNode = _node(make<FilterNode>(std::move(functionCallNode), std::move(evalNode)),
                                filterLoweringNodeProps);
        runNodeVariation(
            ctx, "Shard Filtering with Dotted Field Path", filterNode, &runtimeEnv, &ids, scanDefs);
    }

    {
        // Test lowering for a shardFilter with expressions other than Variables as children.
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids = sbe::value::SlotIdGenerator{};
        runtimeEnv.registerSlot(
            kshardFiltererSlotName, sbe::value::TypeTags::Nothing, 0, false, &ids);

        LoweringNodeProps filterLoweringNodeProps = makeNodeProp();
        filterLoweringNodeProps._indexScanDefName = shardKeyName;

        stdx::unordered_map<std::string, LoweringScanDefinition> scanDefs{std::make_pair(
            shardKeyName,
            buildLoweringScanDefinition(
                {LoweringIndexCollationEntry{_get("a", _id())._n, CollationOp::Ascending},
                 LoweringIndexCollationEntry{_get("b", _id())._n, CollationOp::Ascending}}))};

        auto functionCallNode = make<FunctionCall>(
            "shardFilter",
            makeSeq(_path(make<EvalPath>(make<PathGet>("a", make<PathIdentity>()),
                                         make<Variable>("scan0"))),
                    make<Variable>("proj_b")));

        auto filterNode =
            _node(make<FilterNode>(
                      std::move(functionCallNode),
                      _node(make<PhysicalScanNode>(
                          FieldProjectionMap{{},
                                             {ProjectionName{"scan0"}},
                                             {{FieldNameType{"b"}, ProjectionName{"proj_b"}}}},
                          "collName",
                          false))),
                  filterLoweringNodeProps);
        runNodeVariation(
            ctx, "Shard Filtering with Inlined path", filterNode, &runtimeEnv, &ids, scanDefs);
    }
}

TEST_F(ABTPlanGeneration, LowerConstantExpression) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runExpressionVariation(ctx, "string", Constant::str("hello world"_sd));

    runExpressionVariation(ctx, "int64", Constant::int64(100));
    runExpressionVariation(ctx, "int32", Constant::int32(32));
    runExpressionVariation(ctx, "double", Constant::fromDouble(3.14));
    runExpressionVariation(ctx, "decimal", Constant::fromDecimal(Decimal128("3.14")));

    runExpressionVariation(ctx, "timestamp", Constant::timestamp(Timestamp::max()));
    runExpressionVariation(ctx, "date", Constant::date(Date_t::fromMillisSinceEpoch(100)));

    runExpressionVariation(ctx, "boolean true", Constant::boolean(true));
    runExpressionVariation(ctx, "boolean false", Constant::boolean(false));
}

TEST_F(ABTPlanGeneration, LowerCollationNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    auto projections1 = ProjectionNameOrderPreservingSet({"sortA", "proj0", "proj1"});
    LoweringNodeProps collationNodeProp{._planNodeId = getNextNodeID(),
                                        ._indexScanDefName = boost::none,
                                        ._projections = std::move(projections1),
                                        ._hasLimitSkip = false,
                                        ._limit = 0,
                                        ._skip = 0,
                                        ._ridProjName = boost::none};

    auto node = _node(
        make<CollationNode>(properties::CollationRequirement({{"sortA", CollationOp::Ascending}}),
                            createBindings({{"a", "sortA"}, {"b", "proj0"}, {"c", "proj1"}})),
        collationNodeProp);

    runNodeVariation(
        ctx,
        "Lower collation node with single field",
        _node(make<FilterNode>(makeEquals("proj0", Constant::int32(23)), std::move(node))));

    // Sort on multiple fields.
    auto projections2 = ProjectionNameOrderPreservingSet({"sortA", "sortB", "proj0"});
    LoweringNodeProps collationNodeProp2{._planNodeId = getNextNodeID(),
                                         ._indexScanDefName = boost::none,
                                         ._projections = std::move(projections2),
                                         ._hasLimitSkip = false,
                                         ._limit = 0,
                                         ._skip = 0,
                                         ._ridProjName = boost::none};

    auto node2 = _node(
        make<CollationNode>(properties::CollationRequirement({{"sortA", CollationOp::Ascending},
                                                              {"sortB", CollationOp::Descending}}),
                            createBindings({{"a", "sortA"}, {"b", "sortB"}, {"c", "proj0"}})),
        collationNodeProp2);
    runNodeVariation(
        ctx,
        "Lower collation node with two fields",
        _node(make<FilterNode>(makeEquals("proj0", Constant::int32(35)), std::move(node2))));
}

TEST_F(ABTPlanGeneration, LowerCoScanNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runNodeVariation(ctx, "CoScan", _node(make<CoScanNode>()));
}

TEST_F(ABTPlanGeneration, LowerMultipleEvaluationNodes) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runNodeVariation(ctx,
                     "Lower two chained evaluation nodes",
                     createBindings({{"a", "proj0"}, {"b", "proj1"}}));
}

TEST_F(ABTPlanGeneration, LowerFilterNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    runNodeVariation(
        ctx,
        "filter for: a >= 23",
        _node(make<FilterNode>(
            _path(make<EvalFilter>(
                make<PathGet>("a", make<PathCompare>(Operations::Gte, Constant::int32(23))),
                make<Variable>(scanLabel))),
            _node(scanForTest()))));

    runNodeVariation(
        ctx,
        "filter for constant: true",
        _node(make<FilterNode>(_path(make<EvalFilter>(make<PathConstant>(Constant::boolean(true)),
                                                      make<Variable>(scanLabel))),
                               _node(scanForTest()))));
}

TEST_F(ABTPlanGeneration, LowerFilterNodeParameterized) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    runNodeVariation(
        ctx,
        "filter for: a >= Constant",
        _node(make<FilterNode>(
            _path(make<EvalFilter>(
                make<PathGet>(
                    "a",
                    make<PathCompare>(
                        Operations::Gte,
                        make<FunctionCall>("getParam",
                                           makeSeq(Constant::int32(0),
                                                   Constant::int32(static_cast<int32_t>(
                                                       sbe::value::TypeTags::NumberInt32)))))),
                make<Variable>(scanLabel))),
            _node(scanForTest()))));

    // Check that correct sbe slot entry is added to inputParamToSlotMap
    auto slotId0 = getSlotId(0);
    ASSERT(slotId0 != boost::none);
    ASSERT_EQUALS(*slotId0, 2);

    runNodeVariation(
        ctx,
        "range conjunction filter for: a >= Constant1, a < Constant2",
        _node(make<FilterNode>(
            _path(make<EvalFilter>(
                make<PathGet>(
                    "a",
                    make<PathComposeM>(
                        make<PathCompare>(
                            Operations::Gte,
                            make<FunctionCall>("getParam",
                                               makeSeq(Constant::int32(1),
                                                       Constant::int32(static_cast<int32_t>(
                                                           sbe::value::TypeTags::NumberInt32))))),
                        make<PathCompare>(
                            Operations::Lt,
                            make<FunctionCall>("getParam",
                                               makeSeq(Constant::int32(2),
                                                       Constant::int32(static_cast<int32_t>(
                                                           sbe::value::TypeTags::NumberInt32))))))),
                make<Variable>(scanLabel))),
            _node(scanForTest()))));

    // Check that correct sbe slot entry is added to inputParamToSlotMap
    auto slotId1 = getSlotId(1);
    ASSERT(slotId1 != boost::none);
    ASSERT_EQUALS(*slotId1, 2);

    auto slotId2 = getSlotId(2);
    ASSERT(slotId2 != boost::none);
    ASSERT_EQUALS(*slotId2, 3);

    runNodeVariation(
        ctx,
        "filter for getParam duplicated by the optimizer: a >= Constant1, a < Constant2",
        _node(make<FilterNode>(
            _path(make<EvalFilter>(
                make<PathGet>(
                    "a",
                    make<PathComposeM>(
                        make<PathCompare>(
                            Operations::Gte,
                            make<FunctionCall>("getParam",
                                               makeSeq(Constant::int32(1),
                                                       Constant::int32(static_cast<int32_t>(
                                                           sbe::value::TypeTags::NumberInt32))))),
                        make<PathCompare>(
                            Operations::Lt,
                            make<FunctionCall>("getParam",
                                               makeSeq(Constant::int32(2),
                                                       Constant::int32(static_cast<int32_t>(
                                                           sbe::value::TypeTags::NumberInt32))))))),
                make<Variable>(scanLabel))),
            _node(scanForTest()))));

    // Check that correct sbe slot entry is added to inputParamToSlotMap
    slotId1 = getSlotId(1);
    ASSERT(slotId1 != boost::none);
    ASSERT_EQUALS(*slotId1, 2);

    slotId2 = getSlotId(2);
    ASSERT(slotId2 != boost::none);
    ASSERT_EQUALS(*slotId2, 3);
}

TEST_F(ABTPlanGeneration, LowerGroupByNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    std::vector<GroupNodeType> groupTypes{
        GroupNodeType::Complete, GroupNodeType::Local, GroupNodeType::Global};

    for (const auto& groupType : groupTypes) {
        runNodeVariation(
            ctx,
            str::stream() << "GroupByNode one output with type " << toStringData(groupType),
            _node(make<GroupByNode>(
                ProjectionNameVector{"key1", "key2"},
                ProjectionNameVector{"outFunc1"},
                makeSeq(make<FunctionCall>("$sum", makeSeq(make<Variable>("aggInput1")))),
                groupType,
                createBindings({{"a", "key1"}, {"b", "key2"}, {"c", "aggInput1"}}))));

        runNodeVariation(
            ctx,
            str::stream() << "GroupByNode multiple outputs with type " << toStringData(groupType),
            _node(make<GroupByNode>(
                ProjectionNameVector{"key1", "key2"},
                ProjectionNameVector{"outFunc1", "outFunc2"},
                makeSeq(make<FunctionCall>("$sum", makeSeq(make<Variable>("aggInput1"))),
                        make<FunctionCall>("$sum", makeSeq(make<Variable>("aggInput2")))),
                groupType,
                createBindings(
                    {{"a", "key1"}, {"b", "key2"}, {"c", "aggInput1"}, {"d", "aggInput2"}}))));
    }
}

TEST_F(ABTPlanGeneration, LowerHashJoinNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    // Arguments may be evaluated in any order, and since _node() assigns incrementing stage IDs,
    // nodes with multiple children must have the children defined before the parent to ensure
    // deterministic ordering.
    auto child1 =
        _node(createBindings(
                  {{"other_id", "otherID"}, {"info", "proj0"}},
                  _node(make<PhysicalScanNode>(
                      FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
                  "scan0"),
              {ProjectionName{"proj0"}});

    auto child2 =
        _node(createBindings(
                  {{"id", "ID"}, {"other_info", "proj1"}},
                  _node(make<PhysicalScanNode>(
                      FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
                  "scan1"),
              {ProjectionName{"proj1"}});

    runNodeVariation(
        ctx,
        "Hash join with one equality",
        _node(make<FilterNode>(makeEquals("proj0", Constant::int32(1337)),
                               _node(make<HashJoinNode>(JoinType::Inner,
                                                        std::vector<ProjectionName>{"otherID"},
                                                        std::vector<ProjectionName>{"ID"},
                                                        std::move(child1),
                                                        std::move(child2))))));

    child1 =
        _node(createBindings(
                  {{"city", "proj0"},
                   {"state", "proj1"},
                   {"info", "proj4"},
                   {"more_info", "proj5"},
                   {"some_more_info", "proj6"}},
                  _node(make<PhysicalScanNode>(
                      FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
                  "scan0"),
              {ProjectionName{"proj4"}, ProjectionName{"proj5"}, ProjectionName{"proj6"}});

    child2 =
        _node(createBindings(
                  {{"cityField", "proj2"}, {"state_id", "proj3"}, {"another", "proj7"}},
                  _node(make<PhysicalScanNode>(
                      FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
                  "scan1"),
              {ProjectionName{"proj7"}});

    runNodeVariation(ctx,
                     "Hash join with two equalities",
                     _node(make<FilterNode>(
                         makeEquals("proj7", Constant::int32(56)),
                         _node(make<HashJoinNode>(JoinType::Inner,
                                                  std::vector<ProjectionName>{"proj0", "proj1"},
                                                  std::vector<ProjectionName>{"proj2", "proj3"},
                                                  std::move(child1),
                                                  std::move(child2))))));
}

TEST_F(ABTPlanGeneration, LowerIndexScanNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    // Generate for simple interval and compound interval
    for (int i = 0; i <= 1; i++) {
        bool isReversed = i == 1;
        auto reversedString = isReversed ? "reverse" : "forward";
        // Basic index scan with RID
        runNodeVariation(
            ctx,
            str::stream() << "Basic " << reversedString << " index scan with RID",
            _node(make<IndexScanNode>(
                FieldProjectionMap{{ProjectionName{"rid"}}, {}, {}},
                "collName",
                "index0",
                CompoundIntervalRequirement{{i > 0, makeSeq(Constant::fromDouble(23 + i * 4))},
                                            {i == 0, makeSeq(Constant::fromDouble(35 + i * 100))}},
                isReversed)));


        // Covering index scan with one field
        runNodeVariation(
            ctx,
            str::stream() << "Covering " << reversedString << " index scan with one field",
            _node(make<IndexScanNode>(
                FieldProjectionMap{{}, {}, {{"<indexKey> 0", ProjectionName{"proj0"}}}},
                "collName",
                "index0",
                CompoundIntervalRequirement{
                    {i >= 0, makeSeq(Constant::fromDouble(23 + (i + 1) * 3))},
                    {i > 0, makeSeq(Constant::fromDouble(35 + ((i * 3) * (i * 4))))}},
                isReversed)));
    }
}

TEST_F(ABTPlanGeneration, LowerLimitSkipNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    // Just Limit
    runNodeVariation(ctx,
                     "Lower single limit without skip",
                     _node(make<LimitSkipNode>(5, 0, _node(scanForTest()))));

    // Just Skip
    runNodeVariation(ctx,
                     "Lower single skip without limit",
                     _node(make<LimitSkipNode>(0, 4, _node(scanForTest()))));

    // Limit and Skip
    runNodeVariation(ctx,
                     "Lower LimitSkip node with values for both limit and skip",
                     _node(make<LimitSkipNode>(4, 2, _node(scanForTest()))));
}

TEST_F(ABTPlanGeneration, LowerMergeJoinNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    std::vector<CollationOp> ops = {CollationOp::Ascending, CollationOp::Descending};
    // Run a variation for each supported collation.
    for (auto& op : ops) {
        auto child1 = createBindings(
            {{"other_id", "proj0"}},
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
            "scan0");
        auto child2 = createBindings(
            {{"id", "proj1"}},
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
            "scan1");
        runNodeVariation(ctx,
                         str::stream() << "Lower merge join with one projection (collation="
                                       << toStringData(op) << ")",
                         _node(make<MergeJoinNode>(ProjectionNameVector{ProjectionName{"proj0"}},
                                                   ProjectionNameVector{ProjectionName{"proj1"}},
                                                   std::vector<CollationOp>{op},
                                                   std::move(child1),
                                                   std::move(child2))));
    }

    // Run variations with two projections and every possible combination of collation.
    for (auto& op1 : ops) {
        for (auto& op2 : ops) {
            auto child1 = createBindings(
                {{"other_id", "proj0"}, {"city", "proj2"}},
                _node(make<PhysicalScanNode>(
                    FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
                "scan0");
            auto child2 = createBindings(
                {{"id", "proj1"}, {"city", "proj3"}},
                _node(make<PhysicalScanNode>(
                    FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
                "scan1");

            runNodeVariation(
                ctx,
                str::stream() << "Lower merge join with two projections (collation="
                              << toStringData(op1) << ", " << toStringData(op2) << ")",
                _node(make<MergeJoinNode>(
                    ProjectionNameVector{ProjectionName{"proj0"}, ProjectionName{"proj2"}},
                    ProjectionNameVector{ProjectionName{"proj1"}, ProjectionName{"proj3"}},
                    std::vector<CollationOp>{op1, op2},
                    std::move(child1),
                    std::move(child2))));
        }
        auto child1 = _node(
            createBindings(
                {{"other_id", "proj0"}, {"city", "proj2"}},
                _node(make<PhysicalScanNode>(
                    FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
                "scan0"),
            {ProjectionName{"proj2"}});
        auto child2 = _node(
            createBindings(
                {{"id", "proj1"}, {"city", "proj3"}},
                _node(make<PhysicalScanNode>(
                    FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
                "scan1"),
            {ProjectionName{"proj3"}});
        runNodeVariation(ctx,
                         str::stream() << "Lower merge join with required projection (collation="
                                       << toStringData(op1) << ")",
                         _node(make<FilterNode>(makeEquals("proj3", Constant::str("NYC")),
                                                _node(make<MergeJoinNode>(
                                                    ProjectionNameVector{ProjectionName{"proj0"}},
                                                    ProjectionNameVector{ProjectionName{"proj1"}},
                                                    std::vector<CollationOp>{op1},
                                                    std::move(child1),
                                                    std::move(child2))))));
    }
}

TEST_F(ABTPlanGeneration, LowerNestedLoopJoinNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    // Run a variation for both supported join types.
    std::vector<JoinType> joins = {JoinType::Inner, JoinType::Left};
    for (auto& joinType : joins) {
        auto child1 = _node(
            createBindings(
                {{"city", "proj0"}, {"zipcode", "proj2"}},
                _node(make<PhysicalScanNode>(
                    FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
                "scan0"),
            {ProjectionName{"proj2"}});
        auto child2 = createBindings(
            {{"id", "proj1"}},
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
            "scan1");
        auto nlj = _node(make<NestedLoopJoinNode>(
            joinType,
            ProjectionNameSet{"proj0"},
            _path(make<EvalFilter>(
                make<PathCompare>(Operations::Eq, make<Variable>(ProjectionName{"proj1"})),
                make<Variable>(ProjectionName{"proj0"}))),
            std::move(child1),
            std::move(child2)));

        runNodeVariation(
            ctx,
            str::stream() << "Nested loop join with equality predicate (" << toStringData(joinType)
                          << " join)",
            _node(make<FilterNode>(makeEquals("proj2", Constant::int32(10024)), std::move(nlj))));
    }
}

TEST_F(ABTPlanGeneration, LowerPhysicalScanNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    for (auto i = 0; i <= 1; i++) {
        auto isParallel = i == 1;
        auto parallelString = isParallel ? "(parallel)" : "(not parallel)";
        runNodeVariation(
            ctx,
            str::stream() << "Physical scan with root projection " << parallelString,
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"root0"}}, {}}, "collName", isParallel)));

        runNodeVariation(
            ctx,
            str::stream() << "Physical scan with RID projection " << parallelString,
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{ProjectionName{"RID0"}}, {}, {}}, "collName", isParallel)));

        runNodeVariation(
            ctx,
            str::stream() << "Physical scan with root and RID projections " << parallelString,
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{ProjectionName{"RID0"}}, {ProjectionName{"root0"}}, {}},
                "collName",
                isParallel)));

        runNodeVariation(
            ctx,
            str::stream() << "Physical scan with root, RID and field projections "
                          << parallelString,
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{ProjectionName{"RID0"}},
                                   {ProjectionName{"root0"}},
                                   {{FieldNameType{"field"}, {ProjectionName{"field2"}}}}},
                "collName",
                isParallel)));
    }
}

TEST_F(ABTPlanGeneration, LowerSeekNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    auto indexScan = _node(
        make<IndexScanNode>(FieldProjectionMap{{ProjectionName{"rid"}}, {}, {}},
                            "collName",
                            "index0",
                            CompoundIntervalRequirement{{false, makeSeq(Constant::fromDouble(23))},
                                                        {true, makeSeq(Constant::fromDouble(35))}},
                            false));

    auto seek = _node(make<LimitSkipNode>(
        1, 0, _node(make<SeekNode>(ProjectionName{"rid"}, _fieldProjMap, "collName"))));

    runNodeVariation(ctx,
                     "index seek",
                     _node(make<NestedLoopJoinNode>(JoinType::Inner,
                                                    ProjectionNameSet{"rid"},
                                                    Constant::boolean(true),
                                                    std::move(indexScan),
                                                    std::move(seek))));
}
TEST_F(ABTPlanGeneration, LowerSortedMergeNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    std::vector<CollationOp> ops = {CollationOp::Ascending, CollationOp::Descending};
    auto runVariations = [&](auto req, auto op, auto& suffix) {
        runNodeVariation(ctx,
                         str::stream() << "one source " << suffix,
                         _node(make<SortedMergeNode>(
                             req, makeSeq(createBindings({{"a", "proj0"}, {"b", "proj1"}})))));

        auto left = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto right = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        runNodeVariation(
            ctx,
            str::stream() << "two sources " << suffix,
            _node(make<SortedMergeNode>(req, makeSeq(std::move(left), std::move(right)))));


        auto child1 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto child2 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto child3 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto child4 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto child5 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        runNodeVariation(ctx,
                         str::stream() << "five sources " << suffix,
                         _node(make<SortedMergeNode>(req,
                                                     makeSeq(std::move(child1),
                                                             std::move(child2),
                                                             std::move(child3),
                                                             std::move(child4),
                                                             std::move(child5)))));
    };
    for (auto& op : ops) {
        runVariations(properties::CollationRequirement(
                          ProjectionCollationSpec({{ProjectionName{"proj0"}, op}})),
                      op,
                      str::stream() << "sorted on `a` " << toStringData(op));
        for (auto& op2 : ops) {
            runVariations(properties::CollationRequirement(ProjectionCollationSpec(
                              {{ProjectionName{"proj0"}, op}, {ProjectionName{"proj1"}, op2}})),
                          op,
                          str::stream() << "sorted on `a` " << toStringData(op) << " and `b` "
                                        << toStringData(op2));
        }
    }
}

TEST_F(ABTPlanGeneration, LowerSpoolNodes) {
    // This test tests both SpoolProducerNode and SpoolConsumerNode.
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    std::vector<SpoolProducerType> spoolPTypes = {SpoolProducerType::Eager,
                                                  SpoolProducerType::Lazy};
    std::vector<SpoolConsumerType> spoolCTypes = {SpoolConsumerType::Regular,
                                                  SpoolConsumerType::Stack};
    for (const auto& spoolProdType : spoolPTypes) {
        for (const auto& spoolConsumeType : spoolCTypes) {
            auto leftTree = _node(make<SpoolProducerNode>(spoolProdType,
                                                          1,
                                                          ProjectionNameVector{"proj0"},
                                                          Constant::boolean(true),
                                                          createBindings({{"a", "proj0"}})));
            auto rightTree =
                _node(make<SpoolConsumerNode>(spoolConsumeType, 1, ProjectionNameVector{"proj0"}));
            runNodeVariation(
                ctx,
                str::stream() << "Spool in union with " << toStringData(spoolProdType)
                              << " producer and " << toStringData(spoolConsumeType) << " consumer",
                _node(make<UnionNode>(ProjectionNameVector{"proj0"},
                                      makeSeq(std::move(leftTree), std::move(rightTree)))));
        }
    }

    // Test with a more interesting filter.
    auto filterTree = _path(make<EvalFilter>(
        make<PathGet>("b", make<PathCompare>(Operations::Gte, Constant::int32(23))),
        make<Variable>("scan0")));
    auto leftTree = _node(make<SpoolProducerNode>(SpoolProducerType::Lazy,
                                                  1,
                                                  ProjectionNameVector{"proj0"},
                                                  std::move(filterTree),
                                                  createBindings({{"a", "proj0"}})));
    auto rightTree =
        _node(make<SpoolConsumerNode>(SpoolConsumerType::Stack, 1, ProjectionNameVector{"proj0"}));
    runNodeVariation(ctx,
                     "Spool in union with filter expression",
                     _node(make<UnionNode>(ProjectionNameVector{"proj0"},
                                           makeSeq(std::move(leftTree), std::move(rightTree)))));
}

TEST_F(ABTPlanGeneration, LowerUnionNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    // Test a union with only one child.
    auto leftTree = createBindings({{"a", "proj0"}, {"b", "proj1"}});
    runNodeVariation(
        ctx,
        "UnionNode with only one child",
        _node(make<UnionNode>(ProjectionNameVector{"proj0"}, makeSeq(std::move(leftTree)))));

    // Test a union with two children.
    leftTree = createBindings({{"a", "proj0"}, {"b", "left1"}});
    auto rightTree = createBindings({{"a", "proj0"}, {"b", "right1"}});
    runNodeVariation(ctx,
                     "UnionNode with two children",
                     _node(make<UnionNode>(ProjectionNameVector{"proj0"},
                                           makeSeq(std::move(leftTree), std::move(rightTree)))));

    // Test a union with many children.
    auto aTree = createBindings({{"a", "proj0"}, {"b", "a1"}});
    auto bTree = createBindings({{"a", "proj0"}, {"b", "b1"}});
    auto cTree = createBindings({{"a", "proj0"}, {"b", "c1"}});
    auto dTree = createBindings({{"a", "proj0"}, {"b", "d1"}});
    auto eTree = createBindings({{"a", "proj0"}, {"b", "e1"}});
    runNodeVariation(ctx,
                     "UnionNode with many children",
                     _node(make<UnionNode>(ProjectionNameVector{"proj0"},
                                           makeSeq(std::move(aTree),
                                                   std::move(bTree),
                                                   std::move(cTree),
                                                   std::move(dTree),
                                                   std::move(eTree)))));
}

TEST_F(ABTPlanGeneration, LowerUniqueNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runNodeVariation(
        ctx,
        "Lower unique node with single key",
        _node(make<UniqueNode>(ProjectionNameVector{"proj0"}, createBindings({{"a", "proj0"}}))));


    runNodeVariation(
        ctx,
        "Lower unique node with multiple keys",
        _node(make<UniqueNode>(ProjectionNameVector{"proj0", "proj1", "proj2"},
                               createBindings({{"a", "proj0"}, {"b", "proj1"}, {"c", "proj2"}}))));
}

TEST_F(ABTPlanGeneration, LowerUnwindNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runNodeVariation(ctx,
                     "Lower UnwindNode discard non-arrays",
                     _node(make<UnwindNode>(ProjectionName("proj0"),
                                            ProjectionName("proj0_pid"),
                                            false,
                                            createBindings({{"a", "proj0"}}))));

    runNodeVariation(ctx,
                     "Lower UnwindNode keep non-arrays",
                     _node(make<UnwindNode>(ProjectionName("proj0"),
                                            ProjectionName("proj0_pid"),
                                            true,
                                            createBindings({{"a", "proj0"}}))));
}

TEST_F(ABTPlanGeneration, LowerVarExpression) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    runNodeVariation(
        ctx,
        "varInProj",
        _node(make<EvaluationNode>("proj0",
                                   _path(make<EvalPath>(make<PathGet>("a", make<PathIdentity>()),
                                                        make<Variable>(scanLabel))),
                                   _node(scanForTest()))));
}

DEATH_TEST_F(ABTPlanGeneration,
             LowerScanLogicalNode,
             "Should not be lowering only logical ABT node") {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    runNodeVariation(ctx, "Do not lower scan node", _node(make<ScanNode>("proj0", "scan0")));
}

TEST_F(ABTPlanGeneration, LowerBinaryOpEqMemberRHSArray) {
    // Lower BinaryOp [EqMember] where the type of RHS is array.
    std::string output = autoUpdateExpressionVariation(
        _binary("EqMember", "hello"_cstr, _carray("1"_cdouble, "2"_cdouble, "3"_cdouble))._n);

    ASSERT_STR_EQ_AUTO(  // NOLINT
        "isMember(\"hello\", [1L, 2L, 3L]) ",
        output);
}

}  // namespace
}  // namespace mongo::optimizer
