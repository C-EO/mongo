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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/hierarchical_acquisition.h"

#include <functional>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Implements the support for "read your own write" when run on a node of a replica set. Can be used
 * in the scenarios where causal consistency is not available yet.
 */
class RSLocalClient {
    RSLocalClient(const RSLocalClient&) = delete;
    RSLocalClient& operator=(const RSLocalClient&) = delete;

public:
    explicit RSLocalClient() = default;

    ~RSLocalClient() = default;

    /**
     * Runs the specified command returns the BSON command response plus parsed out Status of this
     * response and write concern error (if present).
     */
    StatusWith<Shard::CommandResponse> runCommandOnce(OperationContext* opCtx,
                                                      const DatabaseName& dbName,
                                                      const BSONObj& cmdObj);

    /**
     * Warning: This method exhausts the cursor and pulls all data into memory.
     * Do not use other than for very small (i.e., admin or metadata) collections.
     */
    StatusWith<Shard::QueryResponse> queryOnce(OperationContext* opCtx,
                                               const ReadPreferenceSetting& readPref,
                                               const repl::ReadConcernLevel& readConcernLevel,
                                               const NamespaceString& nss,
                                               const BSONObj& query,
                                               const BSONObj& sort,
                                               boost::optional<long long> limit,
                                               const boost::optional<BSONObj>& hint = boost::none);

    Status runAggregation(
        OperationContext* opCtx,
        const AggregateCommandRequest& aggRequest,
        std::function<bool(const std::vector<BSONObj>& batch,
                           const boost::optional<BSONObj>& postBatchResumeToken)> callback);

private:
    /**
     * Checks if an OpTime was set on the current Client (ie if the current operation performed a
     * write) and if so updates _lastOpTime to the OpTime from the write that was just performed.
     * The 'previousOpTimeOnClient' parameter is the optime that *was* the optime on this client
     * before we ran this command through this RSLocalClient. By the time this method is called,
     * if the optime set on the Client is different than 'previousOpTimeOnClient' then that means
     * the command just run did a write and we should update _lastOpTime to capture the optime of
     * the write we just did.  If the current optime on the client is the same as
     * 'previousOpTimeOnClient' then the command we just ran didn't do a write, and we should leave
     * _lastOpTime alone.
     */
    void _updateLastOpTimeFromClient(OperationContext* opCtx,
                                     const repl::OpTime& previousOpTimeOnClient);

    repl::OpTime _getLastOpTime();

    // Guards _lastOpTime below.
    stdx::mutex _mutex;

    // Stores the optime that was generated by the last operation to perform a write that was run
    // through _runCommand.  Used in _exhaustiveFindOnConfig for waiting for that optime to be
    // committed so that readConcern majority reads will read the writes that were performed without
    // a w:majority write concern.
    repl::OpTime _lastOpTime{};
};

}  // namespace mongo
