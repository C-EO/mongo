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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/md5.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

constexpr char SKIP_TEMP_COLLECTION[] = "skipTempCollections";
constexpr char EXCLUDE_RECORDIDS[] = "excludeRecordIds";
// TODO SERVER-106005: Remove this option once all versions tested in multiversion suites can scan
// in natural order for capped collections.
constexpr char USE_INDEX_SCAN_FOR_CAPPED_COLLECTIONS[] = "useIndexScanForCappedCollections";

std::shared_ptr<const CollectionCatalog> getConsistentCatalogAndSnapshot(OperationContext* opCtx) {
    // Loop until we get a consistent catalog and snapshot. This is only used for the lock-free
    // implementation of dbHash which skips acquiring database and collection locks.
    while (true) {
        auto catalogBeforeSnapshot = CollectionCatalog::get(opCtx);
        shard_role_details::getRecoveryUnit(opCtx)->preallocateSnapshot();
        const auto catalogAfterSnapshot = CollectionCatalog::get(opCtx);
        if (catalogBeforeSnapshot == catalogAfterSnapshot) {
            return catalogBeforeSnapshot;
        }
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    }
}

// Includes Records and their RecordIds explicitly in the hash.
void hashRecordsAndRecordIds(const CollectionPtr& collection, PlanExecutor* exec, md5_state_t* st) {
    // Clustered collections implicitly replicate RecordIds. Clustered collections are also the only
    // collections which have RecordIds that aren't of type "Long". This method assumes RecordIds
    // are of type "Long" to optimize their translation into the hash.
    invariant(!collection->isClustered());

    BSONObj c;
    RecordId rid;
    while (exec->getNext(&c, &rid) == PlanExecutor::ADVANCED) {
        md5_append(st, (const md5_byte_t*)c.objdata(), c.objsize());
        const auto ridInt = rid.getLong();
        md5_append(st, (const md5_byte_t*)&ridInt, sizeof(ridInt));
    }
}

void hashRecordsOnly(PlanExecutor* exec, md5_state_t* st) {
    BSONObj c;
    while (exec->getNext(&c, nullptr) == PlanExecutor::ADVANCED) {
        md5_append(st, (const md5_byte_t*)c.objdata(), c.objsize());
    }
}

class DBHashCmd : public BasicCommand {
public:
    DBHashCmd() : BasicCommand("dbHash", "dbhash") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool allowsAfterClusterTime(const BSONObj& cmd) const override {
        return false;
    }

    bool canIgnorePrepareConflicts() const override {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const final {

        static const Status kReadConcernNotSupported{ErrorCodes::InvalidOptions,
                                                     "read concern not supported"};
        static const Status kDefaultReadConcernNotPermitted{ErrorCodes::InvalidOptions,
                                                            "default read concern not permitted"};
        // The dbHash command only supports local and snapshot read concern. Additionally, snapshot
        // read concern is only supported if test commands are enabled.
        return {{level != repl::ReadConcernLevel::kLocalReadConcern &&
                     (!getTestCommandsEnabled() ||
                      level != repl::ReadConcernLevel::kSnapshotReadConcern),
                 kReadConcernNotSupported},
                kDefaultReadConcernNotPermitted};
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbName),
                                                  ActionType::dbHash)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        Timer timer;

        // We want the dbCheck command to be allowed via direct shard connections despite the fact
        // that it acquires a collection lock on a user collection. This is an exception, and should
        // not be repeated elsewhere without sufficient need.
        OperationShardingState::get(opCtx).setShouldSkipDirectShardConnectionChecks();

        std::set<std::string> desiredCollections;
        if (cmdObj["collections"].type() == BSONType::array) {
            BSONObjIterator i(cmdObj["collections"].Obj());
            while (i.more()) {
                BSONElement e = i.next();
                uassert(ErrorCodes::BadValue,
                        "collections entries have to be strings",
                        e.type() == BSONType::string);
                desiredCollections.insert(e.String());
            }
        }

        const bool skipTempCollections =
            cmdObj.hasField(SKIP_TEMP_COLLECTION) && cmdObj[SKIP_TEMP_COLLECTION].trueValue();
        if (skipTempCollections) {
            LOGV2(6859700, "Skipping hash computation for temporary collections");
        }

        const bool excludeRecordIds =
            cmdObj.hasField(EXCLUDE_RECORDIDS) && cmdObj[EXCLUDE_RECORDIDS].trueValue();
        if (excludeRecordIds) {
            LOGV2(6859701, "Exclude recordIds in dbHash for recordIdsReplicated collections");
        }

        const bool useIndexScanForCappedCollections =
            cmdObj.hasField(USE_INDEX_SCAN_FOR_CAPPED_COLLECTIONS) &&
            cmdObj[USE_INDEX_SCAN_FOR_CAPPED_COLLECTIONS].trueValue();
        if (useIndexScanForCappedCollections) {
            LOGV2(8218000,
                  "Performing index scan on the _id index instead of using natural order for "
                  "capped collections");
        }

        uassert(ErrorCodes::InvalidNamespace,
                "Cannot pass empty string for 'dbHash' field",
                !(cmdObj.firstElement().type() == BSONType::string &&
                  cmdObj.firstElement().valueStringData().empty()));

        const bool isPointInTimeRead =
            shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource() ==
            RecoveryUnit::ReadSource::kProvided;

        // We take the global lock here as dbHash runs lock-free with point-in-time catalog lookups.
        Lock::GlobalLock globalLock(opCtx, MODE_IS);

        // The CollectionCatalog to use for lock-free reads with point-in-time catalog lookups.
        std::shared_ptr<const CollectionCatalog> catalog = getConsistentCatalogAndSnapshot(opCtx);

        boost::optional<AutoGetDb> autoDb;
        if (isPointInTimeRead) {
            // We only need to lock the database in intent mode and then collection in intent
            // mode as well to ensure that none of the collections get dropped.
            // TODO:SERVER-75848 Make this lock-free
            autoDb.emplace(opCtx, dbName, MODE_IS);
        } else {
            // We lock the entire database in S-mode in order to ensure that the contents will not
            // change for the snapshot when not reading at a timestamp.
            autoDb.emplace(opCtx, dbName, MODE_S);
        }

        result.append("host", prettyHostName(opCtx->getClient()->getLocalPort()));

        md5_state_t globalState;
        md5_init_state(&globalState);

        std::map<std::string, std::string> collectionToHashMap;
        std::map<std::string, UUID> collectionToUUIDMap;
        std::set<std::string> cappedCollectionSet;

        auto checkAndHashCollection = [&](const Collection* collection) -> bool {
            auto collNss = collection->ns();

            uassert(ErrorCodes::BadValue,
                    str::stream() << "weird fullCollectionName [" << collNss.toStringForErrorMsg()
                                  << "]",
                    collNss.size() - 1 > dbName.size());

            if (repl::ReplicationCoordinator::isOplogDisabledForNS(collNss)) {
                return true;
            }

            if (collNss.coll().starts_with("tmp.mr.")) {
                // We skip any incremental map reduce collections as they also aren't
                // replicated.
                return true;
            }

            if (skipTempCollections && collection->isTemporary()) {
                return true;
            }

            if (desiredCollections.size() > 0 &&
                desiredCollections.count(std::string{collNss.coll()}) == 0)
                return true;

            if (collection->isCapped()) {
                cappedCollectionSet.insert(std::string{collNss.coll()});
            }

            collectionToUUIDMap.emplace(std::string{collNss.coll()}, collection->uuid());

            // Compute the hash for this collection.
            // TODO(SERVER-103401): Investigate usage validity of
            // CollectionPtr::CollectionPtr_UNSAFE
            std::string hash = _hashCollection(opCtx,
                                               CollectionPtr::CollectionPtr_UNSAFE(collection),
                                               excludeRecordIds,
                                               useIndexScanForCappedCollections);

            collectionToHashMap[std::string{collNss.coll()}] = hash;

            return true;
        };

        for (auto&& coll : catalog->range(dbName)) {
            UUID uuid = coll->uuid();

            // The namespace must be found as the UUID is fetched from the same
            // CollectionCatalog instance.
            boost::optional<NamespaceString> nss = catalog->lookupNSSByUUID(opCtx, uuid);
            invariant(nss);

            // TODO:SERVER-75848 Make this lock-free
            Lock::CollectionLock clk(opCtx, *nss, MODE_IS);

            auto collection = catalog->establishConsistentCollection(
                opCtx,
                {dbName, uuid},
                shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp());

            if (!collection) {
                // The collection did not exist at the read timestamp with the given UUID.
                continue;
            }

            (void)checkAndHashCollection(collection.get());
        }

        BSONObjBuilder bb(result.subobjStart("collections"));
        BSONArrayBuilder cappedCollections;
        BSONObjBuilder collectionsByUUID;

        for (const auto& elem : cappedCollectionSet) {
            cappedCollections.append(elem);
        }

        for (const auto& entry : collectionToUUIDMap) {
            auto collName = entry.first;
            auto uuid = entry.second;
            uuid.appendToBuilder(&collectionsByUUID, collName);
        }

        for (const auto& entry : collectionToHashMap) {
            auto collName = entry.first;
            auto hash = entry.second;
            bb.append(collName, hash);
            md5_append(&globalState, (const md5_byte_t*)hash.c_str(), hash.size());
        }

        bb.done();

        result.append("capped", BSONArray(cappedCollections.done()));
        result.append("uuids", collectionsByUUID.done());

        md5digest d;
        md5_finish(&globalState, d);
        std::string hash = digestToString(d);

        result.append("md5", hash);
        result.appendNumber("timeMillis", timer.millis());

        return true;
    }

private:
    std::string _hashCollection(OperationContext* opCtx,
                                const CollectionPtr& collection,
                                bool excludeRecordIds,
                                bool useIndexScanForCappedCollections) {
        auto desc = collection->getIndexCatalog()->findIdIndex(opCtx);

        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;

        // Replicated RecordIds circumvent the need to perform an _id lookup because natural
        // scan order is preserved across nodes. If to exclude recordIds, existing _id scan should
        // be kept too. This way a customer will get the same hash with excludeRecordIds option as
        // the one before upgrade.
        bool includeRids = collection->areRecordIdsReplicated() && !excludeRecordIds;

        if (desc && !includeRids &&
            !(collection->isCapped() && !useIndexScanForCappedCollections)) {
            exec = InternalPlanner::indexScan(opCtx,
                                              &collection,
                                              desc,
                                              BSONObj(),
                                              BSONObj(),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                              InternalPlanner::FORWARD,
                                              InternalPlanner::IXSCAN_FETCH);
        } else if (collection->isClustered() || includeRids ||
                   (collection->isCapped() && !useIndexScanForCappedCollections)) {
            exec = InternalPlanner::collectionScan(
                opCtx, &collection, PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
        } else {
            LOGV2(20455, "Can't find _id index for namespace", logAttrs(collection->ns()));
            return "no _id index";
        }

        md5_state_t st;
        md5_init_state(&st);

        try {
            MONGO_verify(nullptr != exec.get());

            // It's unnecessary to explicitly include the RecordIds of a clustered collection in the
            // hash. In a clustered collection, each RecordId is generated by the _id, which is
            // already hashed as a part of the Record.
            const bool explicitlyHashRecordIds = includeRids && !collection->isClustered();
            if (explicitlyHashRecordIds) {
                hashRecordsAndRecordIds(collection, exec.get(), &st);
            } else {
                hashRecordsOnly(exec.get(), &st);
            }
        } catch (DBException& exception) {
            LOGV2_WARNING(
                20456, "Error while hashing, db possibly dropped", logAttrs(collection->ns()));
            exception.addContext("Plan executor error while running dbHash command");
            throw;
        }

        md5digest d;
        md5_finish(&st, d);
        std::string hash = digestToString(d);

        return hash;
    }
};
MONGO_REGISTER_COMMAND(DBHashCmd).forShard();

}  // namespace
}  // namespace mongo
