/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

#include <memory>

namespace mongo {

class ConfigServerCatalogCacheLoaderImpl : public ConfigServerCatalogCacheLoader {
public:
    ConfigServerCatalogCacheLoaderImpl();
    ~ConfigServerCatalogCacheLoaderImpl() override = default;

    void shutDown() override;

    SemiFuture<CollectionAndChangedChunks> getChunksSince(const NamespaceString& nss,
                                                          ChunkVersion version) override;
    SemiFuture<DatabaseType> getDatabase(const DatabaseName& dbName) override;

private:
    // Thread pool to be used to perform metadata load
    std::shared_ptr<ThreadPool> _executor;
};

}  // namespace mongo
