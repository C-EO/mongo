/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/auth/authorization_manager_factory_impl.h"

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_local.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authorization_client_handle_router.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_router_impl.h"

namespace mongo {

std::unique_ptr<AuthorizationManager> AuthorizationManagerFactoryImpl::createRouter(
    Service* service) {
    auto authzRouter = std::make_unique<AuthorizationRouterImpl>(
        service, std::make_unique<AuthorizationClientHandleRouter>());

    return std::make_unique<AuthorizationManagerImpl>(service, std::move(authzRouter));
}

std::unique_ptr<AuthorizationManager> AuthorizationManagerFactoryImpl::createShard(
    Service* service) {
    auto authzRouter = std::make_unique<AuthorizationRouterImpl>(
        service, std::make_unique<AuthorizationClientHandleShard>());
    return std::make_unique<AuthorizationManagerImpl>(service, std::move(authzRouter));
}

std::unique_ptr<auth::AuthorizationBackendInterface>
AuthorizationManagerFactoryImpl::createBackendInterface(Service* service) {
    invariant(service->role().has(ClusterRole::ShardServer) ||
              service->role().has(ClusterRole::ConfigServer));
    return std::make_unique<auth::AuthorizationBackendLocal>();
}

namespace {

MONGO_INITIALIZER(RegisterGlobalAuthzManagerFactory)(InitializerContext* initializer) {
    globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryImpl>();
}

}  // namespace

}  // namespace mongo
