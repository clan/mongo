/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/column_store.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.t");

class CollectionValidationTest : public CatalogTestFixture {
protected:
    CollectionValidationTest(Options options = {}) : CatalogTestFixture(std::move(options)) {}

private:
    void setUp() override {
        CatalogTestFixture::setUp();

        // Create collection kNss for unit tests to use. It will possess a default _id index.
        const CollectionOptions defaultCollectionOptions;
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kNss, defaultCollectionOptions));
    };
};

// Calling verify() is not possible on an ephemeral instance.
class CollectionValidationDiskTest : public CollectionValidationTest {
protected:
    CollectionValidationDiskTest() : CollectionValidationTest(Options{}.ephemeral(false)) {}
};

/**
 * Calls validate on collection nss with both kValidateFull and kValidateNormal validation levels
 * and verifies the results.
 *
 * Returns the list of validation results.
 */
std::vector<std::pair<BSONObj, ValidateResults>> foregroundValidate(
    const NamespaceString& nss,
    OperationContext* opCtx,
    bool valid,
    int numRecords,
    int numInvalidDocuments,
    int numErrors,
    std::initializer_list<CollectionValidation::ValidateMode> modes =
        {CollectionValidation::ValidateMode::kForeground,
         CollectionValidation::ValidateMode::kForegroundFull},
    CollectionValidation::RepairMode repairMode = CollectionValidation::RepairMode::kNone) {
    std::vector<std::pair<BSONObj, ValidateResults>> results;
    for (auto mode : modes) {
        ValidateResults validateResults;
        BSONObjBuilder output;
        ASSERT_OK(CollectionValidation::validate(opCtx,
                                                 nss,
                                                 mode,
                                                 repairMode,
                                                 /*additionalOptions=*/{},
                                                 &validateResults,
                                                 &output,
                                                 /*logDiagnostics=*/false));
        BSONObj obj = output.obj();
        BSONObjBuilder validateResultsBuilder;
        validateResults.appendToResultObj(&validateResultsBuilder, true /* debugging */);
        auto validateResultsObj = validateResultsBuilder.obj();

        ASSERT_EQ(validateResults.valid, valid) << obj << validateResultsObj;
        ASSERT_EQ(validateResults.errors.size(), static_cast<long unsigned int>(numErrors))
            << obj << validateResultsObj;

        ASSERT_EQ(obj.getIntField("nrecords"), numRecords) << obj << validateResultsObj;
        ASSERT_EQ(obj.getIntField("nInvalidDocuments"), numInvalidDocuments)
            << obj << validateResultsObj;

        results.push_back(std::make_pair(obj, validateResults));
    }
    return results;
}

ValidateResults omitTransientWarnings(const ValidateResults& results) {
    ValidateResults copy = results;
    copy.warnings.clear();
    for (const auto& warning : results.warnings) {
        std::string endMsg =
            "This is a transient issue as the collection was actively in use by other "
            "operations.";
        std::string beginMsg = "Could not complete validation of ";
        if (warning.size() >= std::max(endMsg.size(), beginMsg.size())) {
            bool startsWith = std::equal(beginMsg.begin(), beginMsg.end(), warning.begin());
            bool endsWith = std::equal(endMsg.rbegin(), endMsg.rend(), warning.rbegin());
            if (!(startsWith && endsWith)) {
                copy.warnings.emplace_back(warning);
            }
        } else {
            copy.warnings.emplace_back(warning);
        }
    }
    return copy;
}

/**
 * Inserts a range of documents into the nss collection and then returns that count. The range is
 * defined by [startIDNum, startIDNum+numDocs), not inclusive of (startIDNum+numDocs), using the
 * numbers as values for '_id' of the document being inserted followed by numFields fields.
 */
int insertDataRangeForNumFields(const NamespaceString& nss,
                                OperationContext* opCtx,
                                const int startIDNum,
                                const int numDocs,
                                const int numFields) {
    const AutoGetCollection coll(opCtx, nss, MODE_IX);
    std::vector<InsertStatement> inserts;
    for (int i = 0; i < numDocs; ++i) {
        BSONObjBuilder bsonBuilder;
        bsonBuilder << "_id" << i + startIDNum;
        for (int c = 1; c <= numFields; ++c) {
            bsonBuilder << "a" + std::to_string(c) << i + (i * numFields + startIDNum) + c;
        }
        const auto obj = bsonBuilder.obj();
        inserts.push_back(InsertStatement(obj));
    }

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection_internal::insertDocuments(
            opCtx, *coll, inserts.begin(), inserts.end(), nullptr, false));
        wuow.commit();
    }
    return numDocs;
}

/**
 * Inserts a range of documents into the kNss collection and then returns that count. The range is
 * defined by [startIDNum, endIDNum), not inclusive of endIDNum, using the numbers as values for
 * '_id' of the document being inserted.
 */
int insertDataRange(OperationContext* opCtx, const int startIDNum, const int endIDNum) {
    invariant(startIDNum < endIDNum,
              str::stream() << "attempted to insert invalid data range from " << startIDNum
                            << " to " << endIDNum);

    return insertDataRangeForNumFields(kNss, opCtx, startIDNum, endIDNum - startIDNum, 0);
}

/**
 * Inserts a single invalid document into the kNss collection and then returns that count.
 */
int setUpInvalidData(OperationContext* opCtx) {
    AutoGetCollection coll(opCtx, kNss, MODE_IX);
    RecordStore* rs = coll->getRecordStore();

    {
        WriteUnitOfWork wuow(opCtx);
        auto invalidBson = "\0\0\0\0\0"_sd;
        ASSERT_OK(
            rs->insertRecord(opCtx, invalidBson.rawData(), invalidBson.size(), Timestamp::min())
                .getStatus());
        wuow.commit();
    }

    return 1;
}

/**
 * Convenience function to convert ValidateResults to a BSON object.
 */
BSONObj resultToBSON(const ValidateResults& vr) {
    BSONObjBuilder builder;
    vr.appendToResultObj(&builder, true /* debugging */);
    return builder.obj();
}

// Verify that calling validate() on an empty collection with different validation levels returns an
// OK status.
TEST_F(CollectionValidationTest, ValidateEmpty) {
    foregroundValidate(kNss,
                       operationContext(),
                       /*valid*/ true,
                       /*numRecords*/ 0,
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0);
}

// Verify calling validate() on a nonempty collection with different validation levels.
TEST_F(CollectionValidationTest, Validate) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       /*valid*/ true,
                       /*numRecords*/ insertDataRange(opCtx, 0, 5),
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0);
}

// Verify calling validate() on a collection with an invalid document.
TEST_F(CollectionValidationTest, ValidateError) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       /*valid*/ false,
                       /*numRecords*/ setUpInvalidData(opCtx),
                       /*numInvalidDocuments*/ 1,
                       /*numErrors*/ 1);
}

// Verify calling validate() with enforceFastCount=true.
TEST_F(CollectionValidationTest, ValidateEnforceFastCount) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       /*valid*/ true,
                       /*numRecords*/ insertDataRange(opCtx, 0, 5),
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0,
                       {CollectionValidation::ValidateMode::kForegroundFullEnforceFastCount});
}

TEST_F(CollectionValidationDiskTest, ValidateIndexDetailResultsSurfaceVerifyErrors) {
    FailPointEnableBlock fp{"WTValidateIndexStructuralDamage"};
    auto opCtx = operationContext();
    insertDataRange(opCtx, 0, 5);  // initialize collection
    foregroundValidate(
        kNss,
        opCtx,
        /*valid*/ false,
        /*numRecords*/ std::numeric_limits<int32_t>::min(),           // uninitialized
        /*numInvalidDocuments*/ std::numeric_limits<int32_t>::min(),  // uninitialized
        /*numErrors*/ 1,
        {CollectionValidation::ValidateMode::kForegroundFull});
}

/**
 * Waits for a parallel running collection validation operation to start and then hang at a
 * failpoint.
 *
 * A failpoint in the validate() code should have been set prior to calling this function.
 */
void waitUntilValidateFailpointHasBeenReached() {
    while (!CollectionValidation::getIsValidationPausedForTest()) {
        sleepmillis(100);  // a fairly arbitrary sleep period.
    }
    ASSERT(CollectionValidation::getIsValidationPausedForTest());
}

/**
 * Generates a KeyString suitable for positioning a cursor at the beginning of an index.
 */
key_string::Value makeFirstKeyString(const SortedDataInterface& sortedDataInterface) {
    key_string::Builder firstKeyStringBuilder(sortedDataInterface.getKeyStringVersion(),
                                              BSONObj(),
                                              sortedDataInterface.getOrdering(),
                                              key_string::Discriminator::kExclusiveBefore);
    return firstKeyStringBuilder.getValueCopy();
}

/**
 * Extracts KeyString without RecordId.
 */
key_string::Value makeKeyStringWithoutRecordId(const key_string::Value& keyStringWithRecordId,
                                               key_string::Version version) {
    BufBuilder bufBuilder;
    keyStringWithRecordId.serializeWithoutRecordIdLong(bufBuilder);
    auto builderSize = bufBuilder.len();

    auto buffer = bufBuilder.release();

    BufReader bufReader(buffer.get(), builderSize);
    return key_string::Value::deserialize(bufReader, version, boost::none /* ridFormat */);
}

// Verify calling validate() on a collection with old (pre-4.2) keys in a WT unique index.
TEST_F(CollectionValidationTest, ValidateOldUniqueIndexKeyWarning) {
    auto opCtx = operationContext();

    {
        FailPointEnableBlock createOldFormatIndex("WTIndexCreateUniqueIndexesInOldFormat");

        // Durable catalog expects metadata updates to be timestamped but this is
        // not necessary in our case - we just want to check the contents of the index table.
        // The alternative here would be to provide a commit timestamp with a TimestamptBlock.
        repl::UnreplicatedWritesBlock uwb(opCtx);
        auto uniqueIndexSpec = BSON("v" << 2 << "name"
                                        << "a_1"
                                        << "key" << BSON("a" << 1) << "unique" << true);
        ASSERT_OK(
            storageInterface()->createIndexesOnEmptyCollection(opCtx, kNss, {uniqueIndexSpec}));
    }

    // Insert single document with the default (new) index key that includes a record id.
    ASSERT_OK(storageInterface()->insertDocument(opCtx,
                                                 kNss,
                                                 {BSON("_id" << 1 << "a" << 1), Timestamp()},
                                                 repl::OpTime::kUninitializedTerm));

    // Validate the collection here as a sanity check before we modify the index contents in-place.
    foregroundValidate(
        kNss, opCtx, /*valid*/ true, /*numRecords*/ 1, /*numInvalidDocuments*/ 0, /*numErrors*/ 0);

    // Update existing entry in index to pre-4.2 format without record id in key string.
    {
        AutoGetCollection autoColl(opCtx, kNss, MODE_IX);

        auto indexCatalog = autoColl->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(opCtx, "a_1");
        ASSERT(descriptor) << "Cannot find a_1 in index catalog";
        auto entry = indexCatalog->getEntry(descriptor);
        ASSERT(entry) << "Cannot look up index catalog entry for index a_1";

        auto sortedDataInterface = entry->accessMethod()->asSortedData()->getSortedDataInterface();
        ASSERT_FALSE(sortedDataInterface->isEmpty(opCtx)) << "index a_1 should not be empty";

        // Check key in index for only document.
        auto first = makeFirstKeyString(*sortedDataInterface);
        auto firstKeyString = StringData(first.getBuffer(), first.getSize());
        key_string::Value keyStringWithRecordId;
        {
            auto cursor = sortedDataInterface->newCursor(opCtx);
            auto indexEntry = cursor->seekForKeyString(firstKeyString);
            ASSERT(indexEntry);
            ASSERT(cursor->isRecordIdAtEndOfKeyString());
            keyStringWithRecordId = indexEntry->keyString;
            ASSERT_FALSE(cursor->nextKeyString());
        }

        // Replace key with old format (without record id).
        {
            WriteUnitOfWork wuow(opCtx);
            bool dupsAllowed = false;
            sortedDataInterface->unindex(opCtx, keyStringWithRecordId, dupsAllowed);
            FailPointEnableBlock insertOldFormatKeys("WTIndexInsertUniqueKeysInOldFormat");
            ASSERT_OK(sortedDataInterface->insert(opCtx, keyStringWithRecordId, dupsAllowed));
            wuow.commit();
        }

        // Confirm that key in index is in old format.
        {
            auto cursor = sortedDataInterface->newCursor(opCtx);
            auto indexEntry = cursor->seekForKeyString(firstKeyString);
            ASSERT(indexEntry);
            ASSERT_FALSE(cursor->isRecordIdAtEndOfKeyString());
            ASSERT_EQ(indexEntry->keyString.compareWithoutRecordIdLong(keyStringWithRecordId), 0);
            ASSERT_FALSE(cursor->nextKeyString());
        }
    }

    const auto results = foregroundValidate(kNss,
                                            opCtx,
                                            /*valid*/ true,
                                            /*numRecords*/ 1,
                                            /*numInvalidDocuments*/ 0,
                                            /*numErrors*/ 0);
    ASSERT_EQ(results.size(), 2);

    for (const auto& result : results) {
        const auto& validateResults = result.second;
        const auto obj = resultToBSON(validateResults);
        ASSERT(validateResults.valid) << obj;
        const auto warningsWithoutTransientErrors = omitTransientWarnings(validateResults);
        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 1U) << obj;
        ASSERT_STRING_CONTAINS(warningsWithoutTransientErrors.warnings[0],
                               "Unique index a_1 has one or more keys in the old format")
            << obj;
    }
}
}  // namespace
}  // namespace mongo
