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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/smart_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator_for_bucket_auto.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cmath>
#include <cstddef>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::string;
using std::vector;

REGISTER_DOCUMENT_SOURCE(bucketAuto,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceBucketAuto::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(bucketAuto, DocumentSourceBucketAuto::id)

namespace {

boost::intrusive_ptr<Expression> parseGroupByExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONElement& groupByField,
    const VariablesParseState& vps) {
    if (groupByField.type() == BSONType::object &&
        groupByField.embeddedObject().firstElementFieldName()[0] == '$') {
        return Expression::parseObject(expCtx.get(), groupByField.embeddedObject(), vps);
    } else if (groupByField.type() == BSONType::string &&
               // Lager than 2 because we need a '$', at least one char for the field name and
               // the final terminating 0.
               groupByField.valuestrsize() > 2 && groupByField.valueStringData()[0] == '$') {
        return ExpressionFieldPath::parse(expCtx.get(), groupByField.str(), vps);
    }
    uasserted(40239,
              str::stream() << "The $bucketAuto 'groupBy' field must be defined as a $-prefixed "
                               "path or an expression object, but found: "
                            << groupByField.toString(false, false));
}

}  // namespace

const char* DocumentSourceBucketAuto::getSourceName() const {
    return kStageName.data();
}

DocumentSource::GetNextResult DocumentSourceBucketAuto::doGetNext() {
    if (!_populated) {
        auto populationResult = populateSorter();
        if (populationResult.isPaused()) {
            return populationResult;
        }
        invariant(populationResult.isEOF());

        initializeBucketIteration();
        _populated = true;
    }

    if (!_sortedInput) {
        // We have been disposed. Return EOF.
        _memoryTracker.resetCurrent();
        return GetNextResult::makeEOF();
    }

    if (_currentBucketDetails.currentBucketNum++ < _nBuckets) {
        if (auto bucket = populateNextBucket()) {
            return makeDocument(*bucket);
        }
    }
    dispose();
    return GetNextResult::makeEOF();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceBucketAuto::optimize() {
    _groupByExpression = _groupByExpression->optimize();
    for (auto&& accumulatedField : _accumulatedFields) {
        accumulatedField.expr.argument = accumulatedField.expr.argument->optimize();
        accumulatedField.expr.initializer = accumulatedField.expr.initializer->optimize();
    }
    return this;
}

DepsTracker::State DocumentSourceBucketAuto::getDependencies(DepsTracker* deps) const {
    expression::addDependencies(_groupByExpression.get(), deps);

    for (auto&& accumulatedField : _accumulatedFields) {
        // Anything the per-doc expression depends on, the whole stage depends on.
        expression::addDependencies(accumulatedField.expr.argument.get(), deps);
        // The initializer should be an ExpressionConstant, or something that optimizes to one.
        // ExpressionConstant doesn't have dependencies.
    }

    // We know exactly which fields will be present in the output document. Future stages cannot
    // depend on any further fields. The grouping process will remove any metadata from the
    // documents, so there can be no further dependencies on metadata.
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

void DocumentSourceBucketAuto::addVariableRefs(std::set<Variables::Id>* refs) const {
    expression::addVariableRefs(_groupByExpression.get(), refs);

    for (auto&& accumulatedField : _accumulatedFields) {
        expression::addVariableRefs(accumulatedField.expr.argument.get(), refs);
        // The initializer should be an ExpressionConstant, or something that optimizes to one.
        // ExpressionConstant doesn't have dependencies.
    }
}

SortOptions DocumentSourceBucketAuto::makeSortOptions() {
    SortOptions opts;
    opts.maxMemoryUsageBytes = _maxMemoryUsageBytes;
    if (pExpCtx->getAllowDiskUse() && !pExpCtx->getInRouter()) {
        opts.extSortAllowed = true;
        opts.tempDir = pExpCtx->getTempDir();
        opts.sorterFileStats = &_sorterFileStats;
    }
    return opts;
}

DocumentSource::GetNextResult DocumentSourceBucketAuto::populateSorter() {
    if (!_sorter) {
        const auto& valueCmp = pExpCtx->getValueComparator();
        auto comparator = [valueCmp](const Value& lhs, const Value& rhs) {
            return valueCmp.compare(lhs, rhs);
        };

        _sorter = Sorter<Value, Document>::make(makeSortOptions(), comparator);
    }

    auto next = pSource->getNext();
    auto prevMemUsage = _sorter->stats().memUsage();
    for (; next.isAdvanced(); next = pSource->getNext()) {
        auto nextDoc = next.releaseDocument();
        auto key = extractKey(nextDoc);

        auto doc = Document{{AccumulatorN::kFieldNameOutput, Value(std::move(nextDoc))},
                            {AccumulatorN::kFieldNameGeneratedSortKey, Value(_nDocPositions++)}};
        _sorter->add(std::move(key), std::move(doc));

        // To provide as accurate memory accounting as possible, we update the memory tracker after
        // we add each document to the sorter.
        auto currMemUsage = _sorter->stats().memUsage();
        _memoryTracker.add(currMemUsage - prevMemUsage);
        prevMemUsage = currMemUsage;

        ++_nDocuments;
    }

    return next;
}

Value DocumentSourceBucketAuto::extractKey(const Document& doc) {
    if (!_groupByExpression) {
        return Value(BSONNULL);
    }

    Value key = _groupByExpression->evaluate(doc, &pExpCtx->variables);

    if (_granularityRounder) {
        uassert(40258,
                str::stream() << "$bucketAuto can specify a 'granularity' with numeric boundaries "
                                 "only, but found a value with type: "
                              << typeName(key.getType()),
                key.numeric());

        double keyValue = key.coerceToDouble();
        uassert(
            40259,
            "$bucketAuto can specify a 'granularity' with numeric boundaries only, but found a NaN",
            !std::isnan(keyValue));

        uassert(40260,
                "$bucketAuto can specify a 'granularity' with non-negative numbers only, but found "
                "a negative number",
                keyValue >= 0.0);
    }

    // To be consistent with the $group stage, we consider "missing" to be equivalent to null when
    // grouping values into buckets.
    return key.missing() ? Value(BSONNULL) : std::move(key);
}

void DocumentSourceBucketAuto::addDocumentToBucket(const pair<Value, Document>& entry,
                                                   Bucket& bucket) {
    invariant(pExpCtx->getValueComparator().evaluate(entry.first >= bucket._max));
    bucket._max = entry.first;

    const size_t numAccumulators = _accumulatedFields.size();
    for (size_t k = 0; k < numAccumulators; k++) {
        AccumulatorState& accumulator = *bucket._accums[k];
        if (accumulator.needsInput()) {
            bool isPositionalAccum = isPositionalAccumulator(accumulator.getOpName());
            auto value = entry.second.getField(AccumulatorN::kFieldNameOutput);
            auto evaluated = _accumulatedFields[k].expr.argument->evaluate(value.getDocument(),
                                                                           &pExpCtx->variables);

            auto prevMemUsage = accumulator.getMemUsage();
            if (isPositionalAccum) {
                auto wrapped = Value(Document{
                    {AccumulatorN::kFieldNameGeneratedSortKey,
                     entry.second.getField(AccumulatorN::kFieldNameGeneratedSortKey)},
                    {AccumulatorN::kFieldNameOutput, std::move(evaluated)},
                });
                accumulator.process(Value(std::move(wrapped)), false);
            } else {
                accumulator.process(std::move(evaluated), false);
            }
            _accumulatedFieldMemoryTrackers[k]->add(accumulator.getMemUsage() - prevMemUsage);
        }
    }
}

void DocumentSourceBucketAuto::initializeBucketIteration() {
    // Initialize the iterator on '_sorter'.
    invariant(_sorter);
    _sortedInput = _sorter->done();

    _stats.spillingStats.updateSpillingStats(_sorter->stats().spilledRanges(),
                                             _sorterFileStats.bytesSpilledUncompressed(),
                                             _sorter->stats().spilledKeyValuePairs(),
                                             _sorterFileStats.bytesSpilled());
    bucketAutoCounters.incrementPerSpilling(_sorter->stats().spilledRanges(),
                                            _sorterFileStats.bytesSpilledUncompressed(),
                                            _sorter->stats().spilledKeyValuePairs(),
                                            _sorterFileStats.bytesSpilled());

    _sorter.reset();

    // If there are no buckets, then we don't need to populate anything.
    if (_nBuckets == 0) {
        return;
    }

    // Calculate the approximate bucket size. We attempt to fill each bucket with this many
    // documents.
    _currentBucketDetails.approxBucketSize = std::round(double(_nDocuments) / double(_nBuckets));

    if (_currentBucketDetails.approxBucketSize < 1) {
        // If the number of buckets is larger than the number of documents, then we try to make
        // as many buckets as possible by placing each document in its own bucket.
        _currentBucketDetails.approxBucketSize = 1;
    }
}

boost::optional<pair<Value, Document>>
DocumentSourceBucketAuto::adjustBoundariesAndGetMinForNextBucket(Bucket* currentBucket) {
    auto getNextValIfPresent = [this]() {
        return _sortedInput->more() ? boost::optional<pair<Value, Document>>(_sortedInput->next())
                                    : boost::none;
    };

    auto nextValue = getNextValIfPresent();
    if (_granularityRounder) {
        Value boundaryValue = _granularityRounder->roundUp(currentBucket->_max);

        // If there are any values that now fall into this bucket after we round the
        // boundary, absorb them into this bucket too.
        while (nextValue &&
               (pExpCtx->getValueComparator().evaluate(boundaryValue > nextValue->first) ||
                pExpCtx->getValueComparator().evaluate(currentBucket->_max == nextValue->first))) {
            addDocumentToBucket(*nextValue, *currentBucket);
            nextValue = getNextValIfPresent();
        }

        // Handle the special case where the largest value in the first bucket is zero. In this
        // case, we take the minimum boundary of the next bucket and round it down. We then set the
        // maximum boundary of the current bucket to be the rounded down value. This maintains that
        // the maximum boundary of the current bucket is exclusive and the minimum boundary of the
        // next bucket is inclusive.
        double currentMax = boundaryValue.coerceToDouble();
        if (currentMax == 0.0 && nextValue) {
            currentBucket->_max = _granularityRounder->roundDown(nextValue->first);
        } else {
            currentBucket->_max = boundaryValue;
        }
    } else {
        // If there are any more values that are equal to the boundary value, then absorb
        // them into the current bucket too.
        while (nextValue &&
               pExpCtx->getValueComparator().evaluate(currentBucket->_max == nextValue->first)) {
            addDocumentToBucket(*nextValue, *currentBucket);
            nextValue = getNextValIfPresent();
        }

        // If there is a bucket that comes after the current bucket, then the current bucket's max
        // boundary is updated to the next bucket's min. This makes it so that buckets' min
        // boundaries are inclusive and max boundaries are exclusive (except for the last bucket,
        // which has an inclusive max).
        if (nextValue) {
            currentBucket->_max = nextValue->first;
        }
    }
    return nextValue;
}

boost::optional<DocumentSourceBucketAuto::Bucket> DocumentSourceBucketAuto::populateNextBucket() {
    // If there was a bucket before this, the 'currentMin' should be populated, or there are no more
    // documents.
    if (!_currentBucketDetails.currentMin && !_sortedInput->more()) {
        return {};
    }

    std::pair<Value, Document> currentValue =
        _currentBucketDetails.currentMin ? *_currentBucketDetails.currentMin : _sortedInput->next();

    Bucket currentBucket(pExpCtx, currentValue.first, currentValue.first, _accumulatedFields);

    // If we have a granularity specified and if there is a bucket that came before the current
    // bucket being added, then the current bucket's min boundary is updated to be the previous
    // bucket's max boundary. This makes it so that bucket boundaries follow the granularity, have
    // inclusive minimums, and have exclusive maximums.
    if (_granularityRounder) {
        currentBucket._min = _currentBucketDetails.previousMax.value_or(
            _granularityRounder->roundDown(currentValue.first));
    }

    // Evaluate each initializer against an empty document. Normally the initializer can refer to
    // the group key, but in $bucketAuto there is no single group key per bucket.
    Document emptyDoc;
    for (size_t k = 0; k < _accumulatedFields.size(); ++k) {
        Value initializerValue =
            _accumulatedFields[k].expr.initializer->evaluate(emptyDoc, &pExpCtx->variables);
        AccumulatorState& accumulator = *currentBucket._accums[k];
        accumulator.startNewGroup(initializerValue);
        _accumulatedFieldMemoryTrackers[k]->add(accumulator.getMemUsage());
    }

    // Add 'approxBucketSize' number of documents to the current bucket. If this is the last bucket,
    // add all the remaining documents.
    addDocumentToBucket(currentValue, currentBucket);
    const auto isLastBucket = (_currentBucketDetails.currentBucketNum == _nBuckets);
    for (long long i = 1;
         _sortedInput->more() && (i < _currentBucketDetails.approxBucketSize || isLastBucket);
         i++) {
        addDocumentToBucket(_sortedInput->next(), currentBucket);
    }

    // Modify the bucket details for next bucket.
    _currentBucketDetails.currentMin = adjustBoundariesAndGetMinForNextBucket(&currentBucket);
    _currentBucketDetails.previousMax = currentBucket._max;

    // Free the accumulators' memory usage that we've tallied from this past bucket.
    for (size_t i = 0; i < currentBucket._accums.size(); i++) {
        // Subtract the current usage.
        _accumulatedFieldMemoryTrackers[i]->add(-1 * currentBucket._accums[i]->getMemUsage());

        currentBucket._accums[i]->reduceMemoryConsumptionIfAble();

        // Update the memory usage for this AccumulationStatement.
        _accumulatedFieldMemoryTrackers[i]->add(currentBucket._accums[i]->getMemUsage());
    }
    return currentBucket;
}

DocumentSourceBucketAuto::Bucket::Bucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Value min,
    Value max,
    const vector<AccumulationStatement>& accumulationStatements)
    : _min(min), _max(max) {
    _accums.reserve(accumulationStatements.size());
    for (auto&& accumulationStatement : accumulationStatements) {
        _accums.push_back(accumulationStatement.makeAccumulator());
    }
}

Document DocumentSourceBucketAuto::makeDocument(const Bucket& bucket) {
    const size_t nAccumulatedFields = _accumulatedFields.size();
    MutableDocument out(1 + nAccumulatedFields);

    out.addField("_id", Value{Document{{"min", bucket._min}, {"max", bucket._max}}});

    const bool mergingOutput = false;
    for (size_t i = 0; i < nAccumulatedFields; i++) {
        Value val = bucket._accums[i]->getValue(mergingOutput);

        // To be consistent with the $group stage, we consider "missing" to be equivalent to null
        // when evaluating accumulators.
        out.addField(_accumulatedFields[i].fieldName,
                     val.missing() ? Value(BSONNULL) : std::move(val));
    }
    return out.freeze();
}

void DocumentSourceBucketAuto::doDispose() {
    _sortedInput.reset();

    _memoryTracker.resetCurrent();
}


void DocumentSourceBucketAuto::doForceSpill() {
    if (_sorter) {
        auto prevSorterSize = _sorter->stats().memUsage();
        _sorter->spill();

        // The sorter's size has decreased after spilling. Subtract this freed memory from the
        // memory tracker.
        auto currSorterSize = _sorter->stats().memUsage();
        _memoryTracker.add(-1 * (prevSorterSize - currSorterSize));
    } else if (_sortedInput && _sortedInput->spillable()) {
        SortOptions opts = makeSortOptions();
        SorterTracker tracker;
        opts.sorterTracker = &tracker;

        auto previousSpilledBytes = _sorterFileStats.bytesSpilledUncompressed();

        _sortedInput = _sortedInput->spill(opts, Sorter<Value, Document>::Settings{});

        auto spilledDataStorageIncrease = _stats.spillingStats.updateSpillingStats(
            1,
            _sorterFileStats.bytesSpilledUncompressed() - previousSpilledBytes,
            tracker.spilledKeyValuePairs.loadRelaxed(),
            _sorterFileStats.bytesSpilled());
        bucketAutoCounters.incrementPerSpilling(1,
                                                _sorterFileStats.bytesSpilledUncompressed() -
                                                    previousSpilledBytes,
                                                tracker.spilledKeyValuePairs.loadRelaxed(),
                                                spilledDataStorageIncrease);
        // The sorter iterator has spilled everything. Set the memory tracker and the
        // accumulators' trackers back to 0.
        _memoryTracker.resetCurrent();
    }
}

Value DocumentSourceBucketAuto::serialize(const SerializationOptions& opts) const {
    MutableDocument insides;

    insides["groupBy"] = _groupByExpression->serialize(opts);
    insides["buckets"] = opts.serializeLiteral(_nBuckets);

    if (_granularityRounder) {
        //"granularity" only supports some strings, so a specific representative value is used if
        // necessary.
        insides["granularity"] =
            opts.serializeLiteral(_granularityRounder->getName(), Value("R5"_sd));
    }

    MutableDocument outputSpec(_accumulatedFields.size());
    for (auto&& accumulatedField : _accumulatedFields) {
        intrusive_ptr<AccumulatorState> accum = accumulatedField.makeAccumulator();
        outputSpec[opts.serializeFieldPathFromString(accumulatedField.fieldName)] =
            Value(accum->serialize(
                accumulatedField.expr.initializer, accumulatedField.expr.argument, opts));
    }

    insides["output"] = outputSpec.freezeToValue();

    MutableDocument out;
    out[getSourceName()] = insides.freezeToValue();

    if (opts.isSerializingForExplain() &&
        *opts.verbosity >= ExplainOptions::Verbosity::kExecStats) {
        out["usedDisk"] = opts.serializeLiteral(_stats.spillingStats.getSpills() > 0);
        out["spills"] =
            opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpills()));
        out["spilledDataStorageSize"] = opts.serializeLiteral(
            static_cast<long long>(_stats.spillingStats.getSpilledDataStorageSize()));
        out["spilledBytes"] =
            opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledBytes()));
        out["spilledRecords"] =
            opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledRecords()));
        if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
            out["maxUsedMemBytes"] =
                opts.serializeLiteral(static_cast<long long>(_memoryTracker.maxMemoryBytes()));
        }
    }

    return out.freezeToValue();
}

intrusive_ptr<DocumentSourceBucketAuto> DocumentSourceBucketAuto::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    int numBuckets,
    uint64_t maxMemoryBytes,
    std::vector<AccumulationStatement> accumulationStatements,
    const boost::intrusive_ptr<GranularityRounder>& granularityRounder) {
    uassert(40243,
            str::stream() << "The $bucketAuto 'buckets' field must be greater than 0, but found: "
                          << numBuckets,
            numBuckets > 0);
    // If there is no output field specified, then add the default one.
    if (accumulationStatements.empty()) {
        accumulationStatements.emplace_back(
            "count",
            AccumulationExpression(
                ExpressionConstant::create(pExpCtx.get(), Value(BSONNULL)),
                ExpressionConstant::create(pExpCtx.get(), Value(1)),
                [pExpCtx] { return make_intrusive<AccumulatorSum>(pExpCtx.get()); },
                AccumulatorSum::kName));
    }
    return new DocumentSourceBucketAuto(pExpCtx,
                                        groupByExpression,
                                        numBuckets,
                                        std::move(accumulationStatements),
                                        granularityRounder,
                                        maxMemoryBytes);
}

DocumentSourceBucketAuto::DocumentSourceBucketAuto(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    int numBuckets,
    std::vector<AccumulationStatement> accumulationStatements,
    const boost::intrusive_ptr<GranularityRounder>& granularityRounder,
    uint64_t maxMemoryUsageBytes)
    : DocumentSource(kStageName, pExpCtx),
      exec::agg::Stage(kStageName, pExpCtx),
      _sorterFileStats(nullptr /*sorterTracker*/),
      _maxMemoryUsageBytes(maxMemoryUsageBytes),
      _groupByExpression(groupByExpression),
      _granularityRounder(granularityRounder),
      _nBuckets(numBuckets),
      _currentBucketDetails{0},
      _memoryTracker{OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(
          *pExpCtx, pExpCtx->getAllowDiskUse() && !pExpCtx->getInRouter(), maxMemoryUsageBytes)} {
    invariant(!accumulationStatements.empty());
    for (auto&& accumulationStatement : accumulationStatements) {
        _accumulatedFields.push_back(accumulationStatement);
        _accumulatedFieldMemoryTrackers.push_back(&_memoryTracker[accumulationStatement.fieldName]);
    }
}

boost::intrusive_ptr<Expression> DocumentSourceBucketAuto::getGroupByExpression() const {
    return _groupByExpression;
}

boost::intrusive_ptr<Expression>& DocumentSourceBucketAuto::getMutableGroupByExpression() {
    tassert(7020501,
            "Cannot change group by expressions once execution has begun in BucketAuto",
            !_populated);
    return _groupByExpression;
}

const std::vector<AccumulationStatement>& DocumentSourceBucketAuto::getAccumulationStatements()
    const {
    return _accumulatedFields;
}

std::vector<AccumulationStatement>& DocumentSourceBucketAuto::getMutableAccumulationStatements() {
    tassert(7020502,
            "Cannot change accumulated field expression once execution has begun in BucketAuto",
            !_populated);
    return _accumulatedFields;
}

intrusive_ptr<DocumentSource> DocumentSourceBucketAuto::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40240,
            str::stream() << "The argument to $bucketAuto must be an object, but found type: "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    VariablesParseState vps = pExpCtx->variablesParseState;
    vector<AccumulationStatement> accumulationStatements;
    boost::intrusive_ptr<Expression> groupByExpression;
    boost::optional<int> numBuckets;
    boost::intrusive_ptr<GranularityRounder> granularityRounder;

    pExpCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();
        if ("groupBy" == argName) {
            groupByExpression = parseGroupByExpression(pExpCtx, argument, vps);
        } else if ("buckets" == argName) {
            Value bucketsValue = Value(argument);

            uassert(
                40241,
                str::stream()
                    << "The $bucketAuto 'buckets' field must be a numeric value, but found type: "
                    << typeName(argument.type()),
                bucketsValue.numeric());

            uassert(40242,
                    str::stream() << "The $bucketAuto 'buckets' field must be representable as a "
                                     "32-bit integer, but found "
                                  << Value(argument).coerceToDouble(),
                    bucketsValue.integral());

            numBuckets = bucketsValue.coerceToInt();
        } else if ("output" == argName) {
            uassert(40244,
                    str::stream()
                        << "The $bucketAuto 'output' field must be an object, but found type: "
                        << typeName(argument.type()),
                    argument.type() == BSONType::object);

            for (auto&& outputField : argument.embeddedObject()) {
                auto parsedStmt = AccumulationStatement::parseAccumulationStatement(
                    pExpCtx.get(), outputField, vps);
                auto stmt =
                    replaceAccumulationStatementForBucketAuto(pExpCtx.get(), std::move(parsedStmt));
                stmt.expr.initializer = stmt.expr.initializer->optimize();
                uassert(4544714,
                        "Can't refer to the group key in $bucketAuto",
                        ExpressionConstant::isNullOrConstant(stmt.expr.initializer));
                accumulationStatements.push_back(std::move(stmt));
            }
        } else if ("granularity" == argName) {
            uassert(40261,
                    str::stream()
                        << "The $bucketAuto 'granularity' field must be a string, but found type: "
                        << typeName(argument.type()),
                    argument.type() == BSONType::string);
            granularityRounder = GranularityRounder::getGranularityRounder(pExpCtx, argument.str());
        } else {
            uasserted(40245, str::stream() << "Unrecognized option to $bucketAuto: " << argName);
        }
    }

    uassert(40246,
            "$bucketAuto requires 'groupBy' and 'buckets' to be specified",
            groupByExpression && numBuckets);

    return DocumentSourceBucketAuto::create(
        pExpCtx,
        groupByExpression,
        numBuckets.value(),
        internalDocumentSourceBucketAutoMaxMemoryBytes.loadRelaxed(),
        std::move(accumulationStatements),
        granularityRounder);
}

}  // namespace mongo
