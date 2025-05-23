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

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/hasher.h"

namespace mongo {
namespace sbe {
namespace vm {
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinHash(ArityType arity) {
    auto hashVal = value::hashInit();
    for (ArityType idx = 0; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);
        hashVal = value::hashCombine(hashVal, value::hashValue(tag, val));
    }

    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<decltype(hashVal)>(hashVal)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinShardHash(ArityType arity) {
    invariant(arity == 1);

    auto [ownedShardKey, shardKeyTag, shardKeyValue] = getFromStack(0);

    // Compute the shard key hash value by round-tripping it through BSONObj as it is currently the
    // only way to do it if we do not want to duplicate the hash computation code.
    // TODO SERVER-55622
    BSONObjBuilder input;
    bson::appendValueToBsonObj<BSONObjBuilder>(input, ""_sd, shardKeyTag, shardKeyValue);
    auto hashVal =
        BSONElementHasher::hash64(input.obj().firstElement(), BSONElementHasher::DEFAULT_HASH_SEED);
    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<decltype(hashVal)>(hashVal)};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
