
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/s/at_cluster_time_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 *  Add atClusterTime to a non-empty readConcern.
 */
TEST(ClusterCommands, AddAtClusterTimeNormal) {
    BSONObj command = BSON("aggregate"
                           << "testColl"
                           << "readConcern"
                           << BSON("level"
                                   << "snapshot"));
    BSONObj expectedCommand = BSON("aggregate"
                                   << "testColl"
                                   << "readConcern"
                                   << BSON("level"
                                           << "snapshot"
                                           << "atClusterTime"
                                           << Timestamp(1, 0)));
    BSONObj newCommand =
        at_cluster_time_util::appendAtClusterTime(command, LogicalTime(Timestamp(1, 0)));
    ASSERT_BSONOBJ_EQ(expectedCommand, newCommand);
}

// Adding atClusterTime overwrites an existing afterClusterTime.
TEST(ClusterCommands, AddingAtClusterTimeOverwritesExistingAfterClusterTime) {
    const auto existingAfterClusterTime = Timestamp(1, 1);
    BSONObj command = BSON("aggregate"
                           << "testColl"
                           << "readConcern"
                           << BSON("level"
                                   << "snapshot"
                                   << "afterClusterTime"
                                   << existingAfterClusterTime));

    const auto computedAtClusterTime = Timestamp(2, 1);
    BSONObj expectedCommand = BSON("aggregate"
                                   << "testColl"
                                   << "readConcern"
                                   << BSON("level"
                                           << "snapshot"
                                           << "atClusterTime"
                                           << computedAtClusterTime));

    BSONObj newCommand =
        at_cluster_time_util::appendAtClusterTime(command, LogicalTime(computedAtClusterTime));
    ASSERT_BSONOBJ_EQ(expectedCommand, newCommand);
}

// Adding atClusterTime overwrites an existing afterClusterTime and will add level "snapshot" if it
// is not there.
TEST(ClusterCommands, AddingAtClusterTimeAddsLevelSnapshotIfNotThere) {
    const auto existingAfterClusterTime = Timestamp(1, 1);
    BSONObj command = BSON("aggregate"
                           << "testColl"
                           << "readConcern"
                           << BSON("afterClusterTime" << existingAfterClusterTime));

    const auto computedAtClusterTime = Timestamp(2, 1);
    BSONObj expectedCommand = BSON("aggregate"
                                   << "testColl"
                                   << "readConcern"
                                   << BSON("level"
                                           << "snapshot"
                                           << "atClusterTime"
                                           << computedAtClusterTime));

    BSONObj newCommand =
        at_cluster_time_util::appendAtClusterTime(command, LogicalTime(computedAtClusterTime));
    ASSERT_BSONOBJ_EQ(expectedCommand, newCommand);
}

}  // namespace
}  // namespace mongo
