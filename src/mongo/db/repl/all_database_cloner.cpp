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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplicationInitialSync

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/base/string_data.h"
#include "mongo/db/repl/all_database_cloner.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
namespace mongo {
namespace repl {

AllDatabaseCloner::AllDatabaseCloner(InitialSyncSharedData* sharedData,
                                     const HostAndPort& source,
                                     DBClientConnection* client,
                                     StorageInterface* storageInterface,
                                     ThreadPool* dbPool)
    : BaseCloner("AllDatabaseCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _connectStage("connect", this, &AllDatabaseCloner::connectStage),
      _listDatabasesStage("listDatabases", this, &AllDatabaseCloner::listDatabasesStage) {}

BaseCloner::ClonerStages AllDatabaseCloner::getStages() {
    return {&_connectStage, &_listDatabasesStage};
}

Status AllDatabaseCloner::ensurePrimaryOrSecondary(
    const executor::RemoteCommandResponse& isMasterReply) {
    if (!isMasterReply.isOK()) {
        LOGV2(21054, "Cannot reconnect because isMaster command failed.");
        return isMasterReply.status;
    }
    if (isMasterReply.data["ismaster"].trueValue() || isMasterReply.data["secondary"].trueValue())
        return Status::OK();

    // There is a window during startup where a node has an invalid configuration and will have
    // an isMaster response the same as a removed node.  So we must check to see if the node is
    // removed by checking local configuration.
    auto otherNodes =
        ReplicationCoordinator::get(getGlobalServiceContext())->getOtherNodesInReplSet();
    if (std::find(otherNodes.begin(), otherNodes.end(), getSource()) == otherNodes.end()) {
        Status status(ErrorCodes::NotMasterOrSecondary,
                      str::stream() << "Sync source " << getSource()
                                    << " has been removed from the replication configuration.");
        stdx::lock_guard<InitialSyncSharedData> lk(*getSharedData());
        // Setting the status in the shared data will cancel the initial sync.
        getSharedData()->setInitialSyncStatusIfOK(lk, status);
        return status;
    }
    return Status(ErrorCodes::NotMasterOrSecondary,
                  str::stream() << "Cannot connect because sync source " << getSource()
                                << " is neither primary nor secondary.");
}

BaseCloner::AfterStageBehavior AllDatabaseCloner::connectStage() {
    auto* client = getClient();
    // If the client already has the address (from a previous attempt), we must allow it to
    // handle the reconnect itself. This is necessary to get correct backoff behavior.
    if (client->getServerHostAndPort() != getSource()) {
        client->setHandshakeValidationHook(
            [this](const executor::RemoteCommandResponse& isMasterReply) {
                return ensurePrimaryOrSecondary(isMasterReply);
            });
        uassertStatusOK(client->connect(getSource(), StringData()));
    } else {
        client->checkConnection();
    }
    uassertStatusOK(replAuthenticate(client).withContext(
        str::stream() << "Failed to authenticate to " << getSource()));
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior AllDatabaseCloner::listDatabasesStage() {
    BSONObj res;
    auto databasesArray = getClient()->getDatabaseInfos(BSONObj(), true /* nameOnly */);
    for (const auto& dbBSON : databasesArray) {
        if (!dbBSON.hasField("name")) {
            LOGV2_DEBUG(21055,
                        1,
                        "Excluding database due to the 'listDatabases' response not containing a "
                        "'name' field for this entry: {dbBSON}",
                        "dbBSON"_attr = dbBSON);
            continue;
        }
        const auto& dbName = dbBSON["name"].str();
        if (dbName == "local") {
            LOGV2_DEBUG(21056,
                        1,
                        "Excluding database from the 'listDatabases' response: {dbBSON}",
                        "dbBSON"_attr = dbBSON);
            continue;
        } else {
            _databases.emplace_back(dbName);
            // Make sure "admin" comes first.
            if (dbName == "admin" && _databases.size() > 1) {
                std::swap(_databases.front(), _databases.back());
            }
        }
    }
    return kContinueNormally;
}

void AllDatabaseCloner::postStage() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.databasesCloned = 0;
        _stats.databaseStats.reserve(_databases.size());
        for (const auto& dbName : _databases) {
            _stats.databaseStats.emplace_back();
            _stats.databaseStats.back().dbname = dbName;
        }
    }
    for (const auto& dbName : _databases) {
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _currentDatabaseCloner = std::make_unique<DatabaseCloner>(dbName,
                                                                      getSharedData(),
                                                                      getSource(),
                                                                      getClient(),
                                                                      getStorageInterface(),
                                                                      getDBPool());
        }
        auto dbStatus = _currentDatabaseCloner->run();
        if (dbStatus.isOK()) {
            LOGV2_DEBUG(21057,
                        1,
                        "Database clone for '{dbName}' finished: {dbStatus}",
                        "dbName"_attr = dbName,
                        "dbStatus"_attr = dbStatus);
        } else {
            LOGV2_WARNING(21060,
                          "database '{dbName}' ({stats_databasesCloned_1} of {databases_size}) "
                          "clone failed due to {dbStatus}",
                          "dbName"_attr = dbName,
                          "stats_databasesCloned_1"_attr = (_stats.databasesCloned + 1),
                          "databases_size"_attr = _databases.size(),
                          "dbStatus"_attr = dbStatus.toString());
            setInitialSyncFailedStatus(dbStatus);
            return;
        }
        if (StringData(dbName).equalCaseInsensitive("admin")) {
            LOGV2_DEBUG(21058, 1, "Finished the 'admin' db, now validating it.");
            // Do special checks for the admin database because of auth. collections.
            auto adminStatus = Status(ErrorCodes::NotYetInitialized, "");
            {
                OperationContext* opCtx = cc().getOperationContext();
                ServiceContext::UniqueOperationContext opCtxPtr;
                if (!opCtx) {
                    opCtxPtr = cc().makeOperationContext();
                    opCtx = opCtxPtr.get();
                }
                adminStatus = getStorageInterface()->isAdminDbValid(opCtx);
            }
            if (!adminStatus.isOK()) {
                LOGV2_DEBUG(21059,
                            1,
                            "Validation failed on 'admin' db due to {adminStatus}",
                            "adminStatus"_attr = adminStatus);
                setInitialSyncFailedStatus(adminStatus);
                return;
            }
        }
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _stats.databaseStats[_stats.databasesCloned] = _currentDatabaseCloner->getStats();
            _currentDatabaseCloner = nullptr;
            _stats.databasesCloned++;
        }
    }
}

AllDatabaseCloner::Stats AllDatabaseCloner::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    AllDatabaseCloner::Stats stats = _stats;
    if (_currentDatabaseCloner) {
        stats.databaseStats[_stats.databasesCloned] = _currentDatabaseCloner->getStats();
    }
    return stats;
}

std::string AllDatabaseCloner::toString() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return str::stream() << "initial sync --"
                         << " active:" << isActive(lk) << " status:" << getStatus(lk).toString()
                         << " source:" << getSource()
                         << " db cloners completed:" << _stats.databasesCloned;
}

std::string AllDatabaseCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj AllDatabaseCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    append(&bob);
    return bob.obj();
}

void AllDatabaseCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("databasesCloned", databasesCloned);
    for (auto&& db : databaseStats) {
        BSONObjBuilder dbBuilder(builder->subobjStart(db.dbname));
        db.append(&dbBuilder);
        dbBuilder.doneFast();
    }
}

}  // namespace repl
}  // namespace mongo