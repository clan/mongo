/**
 * Provides helper functions to output content to markdown. This is used for golden testing, using
 * `printGolden` to write to the expected output files.
 */

/* eslint-disable no-undef */
import {
    formatExplainRoot,
} from "jstests/libs/analyze_plan.js";
import {normalizeArray, tojsonMultiLineSortKeys} from "jstests/libs/golden_test.js";

let sectionCount = 1;
export function section(msg) {
    printGolden(`## ${sectionCount}.`, msg);
    sectionCount++;
}

export function subSection(msg) {
    printGolden("###", msg);
}

export function line(msg) {
    printGolden(msg);
}

export function codeOneLine(msg) {
    printGolden("`" + tojsononeline(msg) + "`");
}

export function code(msg, fmt = "json") {
    printGolden("```" + fmt);
    printGolden(msg);
    printGolden("```");
}

export function linebreak() {
    printGolden();
}

/**
 * Takes a collection and an aggregation pipeline. Outputs the pipeline, the aggregation results and
 * a summary of the explain to markdown. By default the results will be sorted, but the original
 * order can be kept by setting `shouldSortResults` to false.
 */
export function outputAggregationPlanAndResults(coll, pipeline, shouldSortResults = true) {
    const results = coll.aggregate(pipeline).toArray();
    const explain = coll.explain("allPlansExecution").aggregate(pipeline);
    const flatPlan = formatExplainRoot(explain);

    subSection("Pipeline");
    code(tojson(pipeline));

    subSection("Results");
    code(normalizeArray(results, shouldSortResults));

    subSection("Summarized explain");
    code(tojsonMultiLineSortKeys(flatPlan));

    linebreak();
}

/**
 * Takes a collection, the key for which to return distinct values and a filter for the distinct
 * query. Outputs the expected results, the actual returned distinct results and a summary of the
 * explain to markdown.
 */
export function outputDistinctPlanAndResults(coll, key, filter = {}) {
    const results = coll.distinct(key, filter);
    const explain = coll.explain("allPlansExecution").distinct(key, filter);
    const flatPlan = formatExplainRoot(explain);

    subSection(`Distinct on "${key}", with filter: ${tojson(filter)}`);

    subSection("Expected results");
    codeOneLine(getUniqueResults(coll, key, filter));

    subSection("Distinct results");
    codeOneLine(results);

    subSection("Summarized explain");
    code(tojsonMultiLineSortKeys(flatPlan));

    linebreak();
}

/**
 * Helper function that manually computes the unique values for the given key in the given
 * collection (filtered on `filter`). Useful to compare with the actual output from a distinct()
 * query.
 */
function getUniqueResults(coll, key, filter) {
    return Array
        .from(new Set(coll.find(filter, {[key]: 1, _id: 0}).toArray().map(o => o ? o[key] : null)))
        .sort();
}
