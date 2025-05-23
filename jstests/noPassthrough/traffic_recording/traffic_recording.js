// tests for the traffic_recording commands.
// @tags: [requires_auth]
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getDB(client) {
    let db = client.getDB("admin");
    db.auth("admin", "pass");

    return db;
}

function runTest(client, restartCommand) {
    let db = getDB(client);

    let res = db.runCommand({'startRecordingTraffic': 1, 'filename': 'notARealPath'});
    assert.eq(res.ok, false);
    assert.eq(res["errmsg"], "Traffic recording directory not set");

    const path = MongoRunner.toRealDir("$dataDir/traffic_recording/");
    mkdir(path);

    if (!jsTest.isMongos(client)) {
        TestData.enableTestCommands = false;
        client = restartCommand({
            trafficRecordingDirectory: path,
            AlwaysRecordTraffic: "notARealPath",
            enableTestCommands: 0,
        });
        TestData.enableTestCommands = true;
        assert.eq(null, client, "AlwaysRecordTraffic and not enableTestCommands should fail");
    }

    client = restartCommand({
        trafficRecordingDirectory: path,
        AlwaysRecordTraffic: "notARealPath",
        enableTestCommands: 1
    });
    assert.neq(null, client, "AlwaysRecordTraffic and with enableTestCommands should suceed");
    db = getDB(client);

    assert(db.runCommand({"serverStatus": 1}).trafficRecording.running);

    client = restartCommand({trafficRecordingDirectory: path});
    db = getDB(client);

    res = db.runCommand({'startRecordingTraffic': 1, 'filename': 'notARealPath'});
    assert.eq(res.ok, true);

    // Running the command again should fail
    res = db.runCommand({'startRecordingTraffic': 1, 'filename': 'notARealPath'});
    assert.eq(res.ok, false);
    assert.eq(res["errmsg"], "Traffic recording already active");

    // Running the serverStatus command should return the relevant information
    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    let trafficStats = res["trafficRecording"];
    assert.eq(trafficStats["running"], true);

    // Assert that the current file size is growing
    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    let trafficStats2 = res["trafficRecording"];
    assert.eq(trafficStats2["running"], true);
    assert(trafficStats2["currentFileSize"] >= trafficStats["currentFileSize"]);

    // Running the stopRecordingTraffic command should succeed
    res = db.runCommand({'stopRecordingTraffic': 1});
    assert.eq(res.ok, true);

    // Running the stopRecordingTraffic command again should fail
    res = db.runCommand({'stopRecordingTraffic': 1});
    assert.eq(res.ok, false);
    assert.eq(res["errmsg"], "Traffic recording not active");

    // Running the serverStatus command should return running is false
    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    trafficStats = res["trafficRecording"];
    assert.eq(trafficStats["running"], false);

    return client;
}

{
    let m = MongoRunner.runMongod({auth: ""});

    let db = m.getDB("admin");

    db.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    db.auth("admin", "pass");

    m = runTest(m, function(setParams) {
        if (m) {
            MongoRunner.stopMongod(m, null, {user: 'admin', pwd: 'pass'});
        }
        try {
            m = MongoRunner.runMongod({auth: "", setParameter: setParams});
        } catch (e) {
            return null;
        }

        m.getDB("admin").createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});

        return m;
    });

    MongoRunner.stopMongod(m, null, {user: 'admin', pwd: 'pass'});
}

{
    let shardTest = new ShardingTest({
        config: 1,
        mongos: 1,
        shards: 0,
    });

    runTest(shardTest.s, function(setParams) {
        shardTest.restartMongos(0, {
            restart: true,
            setParameter: setParams,
        });

        return shardTest.s;
    });

    shardTest.stop();
}

// Test that the Recorder can generate a new recording file when the current recording reaches the
// max file size.
{
    const path = MongoRunner.toRealDir("$dataDir/traffic_recording/");
    mkdir(path + '/recordings/');

    let m = MongoRunner.runMongod(
        {auth: "", setParameter: {trafficRecordingDirectory: path, enableTestCommands: 1}});

    let db = m.getDB("admin");

    db.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    db.auth("admin", "pass");

    let coll = db[jsTestName()];
    let res = db.runCommand(
        {'startRecordingTraffic': 1, 'filename': 'recordings', 'maxFileSize': NumberLong(100)});
    assert.eq(res.ok, true, res);

    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    let trafficStats = res["trafficRecording"];
    assert.eq(trafficStats["running"], true);
    assert.eq(trafficStats["maxFileSize"], NumberLong(100));
    let recordingFile = trafficStats["recordingFile"];

    // Run a few commands to cause hitting the max file size and creating a new recording file.
    assert.commandWorked(coll.insert({"foo": "bar"}));
    coll.find({"foo": "bar"});
    coll.find({"a": 1});
    // The name of the recordings uses timestamp/date. Forwarding the system time to prevent opening
    // the same recording file.
    sleep(100);

    assert.commandWorked(coll.insert({"foo": "bar", "a": 1}));
    coll.find({"a": 1});
    coll.find({"a": 2});
    coll.find({"foo": "bar"});

    // Check the server status again to ensure a different recording file was generated.
    res = db.runCommand({"serverStatus": 1});
    assert("trafficRecording" in res);
    trafficStats = res["trafficRecording"];
    assert.neq(
        recordingFile, trafficStats["recordingFile"], "A new recording file should be created");

    res = db.runCommand({'stopRecordingTraffic': 1});
    assert.eq(res.ok, true);

    MongoRunner.stopMongod(m, null, {user: 'admin', pwd: 'pass'});
}
