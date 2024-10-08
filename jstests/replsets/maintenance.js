

import {ReplSetTest} from "jstests/libs/replsettest.js";

var replTest = new ReplSetTest({name: 'unicomplex', nodes: 2});
var conns = replTest.startSet({verbose: 1});
var config = replTest.getReplSetConfig();
config.members[0].priority = 2;
replTest.initiate(config);
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

// Make sure we have a primary
var primary = replTest.getPrimary();

for (i = 0; i < 20; i++) {
    primary.getDB("bar").foo.insert({x: 1, y: i, abc: 123, str: "foo bar baz"});
}
for (i = 0; i < 20; i++) {
    primary.getDB("bar").foo.update({y: i}, {$push: {foo: "barrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr"}});
}

replTest.awaitReplication();

assert.soon(function() {
    return conns[1].getDB("admin").hello().secondary;
});

let join =
    startParallelShell("db.getSiblingDB('bar').runCommand({compact : 'foo'});", replTest.ports[1]);

print("joining");
join();

print("check secondary becomes a secondary again");
var secondarySoon = function() {
    var x = 0;
    assert.soon(function() {
        var helloRes = conns[1].getDB("admin").hello();
        if (x++ % 5 == 0)
            printjson(helloRes);
        return helloRes.secondary;
    });
};

secondarySoon();

print("make sure compact works on a secondary (SERVER-3923)");
primary.getDB("foo").bar.drop();
replTest.awaitReplication();
var result = conns[1].getDB("foo").runCommand({compact: "bar"});
assert.eq(result.ok, 0, tojson(result));

secondarySoon();

print("use replSetMaintenance command to go in/out of maintence mode");

print("primary cannot go into maintence mode");
result = primary.getDB("admin").runCommand({replSetMaintenance: 1});
assert.eq(result.ok, 0, tojson(result));

print("check getMore works on a secondary, not on a recovering node");
var cursor = conns[1].getDB("bar").foo.find().batchSize(2);
for (var i = 0; i < 5; i++) {
    cursor.next();
}

print("secondary can");
result = conns[1].getDB("admin").runCommand({replSetMaintenance: 1});
assert.eq(result.ok, 1, tojson(result));

print("make sure secondary goes into recovering");
var x = 0;
assert.soon(function() {
    var helloRes = conns[1].getDB("admin").hello();
    if (x++ % 5 == 0)
        printjson(helloRes);
    return !helloRes.secondary && !helloRes.isWritablePrimary;
});

var recv = conns[1].getDB("admin").runCommand({find: "foo"});
assert.commandFailed(recv);
assert.eq(recv.errmsg, "node is recovering");

print("now getmore shouldn't work");
var ex = assert.throws(function() {
    let lastDoc = null;
    while (cursor.hasNext()) {
        lastDoc = cursor.next();
    }
}, [] /*no params*/, "getmore didn't fail");

assert(ex.message.match("13436"), "wrong error code -- " + ex);

result = conns[1].getDB("admin").runCommand({replSetMaintenance: 0});
assert.eq(result.ok, 1, tojson(result));

secondarySoon();
replTest.stopSet();
