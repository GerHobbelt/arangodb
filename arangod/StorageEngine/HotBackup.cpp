////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "HotBackup.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/ServerState.h"
#include "StorageEngine/EngineSelectorFeature.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/RocksDBEngine/RocksDBHotBackup.h"
#include "Enterprise/StorageEngine/HotBackupFeature.h"
#endif

namespace arangodb {

HotBackup::HotBackup(ArangodServer& server)
#ifdef USE_ENTERPRISE
    : _server(server) {
#else
{
#endif
  if (ServerState::instance()->isCoordinator()) {
    _engine = BACKUP_ENGINE::CLUSTER;
  } else {
    _engine = BACKUP_ENGINE::ROCKSDB;
  }
}

arangodb::Result HotBackup::execute(std::string const& command,
                                    VPackSlice const payload,
                                    VPackBuilder& report) {
  switch (_engine) {
    case BACKUP_ENGINE::ROCKSDB:
      return executeDBServer(command, payload, report);
    case BACKUP_ENGINE::CLUSTER:
      return executeCoordinator(command, payload, report);
  }

  return arangodb::Result(TRI_ERROR_NOT_IMPLEMENTED,
                          "hot backup not implemented for this storage engine");
}

arangodb::Result HotBackup::executeDBServer(std::string const& command,
                                            VPackSlice const payload,
                                            VPackBuilder& report) {
  TRI_ASSERT(_engine != BACKUP_ENGINE::CLUSTER || command == "lock" ||
             command == "unlock");
#ifdef USE_ENTERPRISE
  auto& feature = _server.getFeature<HotBackupFeature>();
  auto operation =
      RocksDBHotBackup::operationFactory(feature, command, payload, report);

  if (operation->valid()) {
    operation->execute();
  }

  operation->doAuditLog();

  // if !valid() then !success() already set
  if (!operation->success()) {
    return {operation->restResponseError(), operation->errorMessage()};
  }
#endif

  return {};
}

arangodb::Result HotBackup::executeCoordinator(std::string const& command,
                                               VPackSlice const payload,
                                               VPackBuilder& report) {
#ifdef USE_ENTERPRISE
  auto& feature = _server.getFeature<ClusterFeature>();
  if (command == "create") {
    return hotBackupCoordinator(feature, payload, report);
  } else if (command == "lock" || command == "unlock") {
    return executeDBServer(command, payload, report);
  } else if (command == "restore") {
    return hotRestoreCoordinator(feature, payload, report);
  } else if (command == "delete") {
    return deleteHotBackupsOnCoordinator(feature, payload, report);
  } else if (command == "list") {
    return listHotBackupsOnCoordinator(feature, payload, report);
  } else if (command == "upload") {
    return uploadBackupsOnCoordinator(feature, payload, report);
  } else if (command == "download") {
    return downloadBackupsOnCoordinator(feature, payload, report);
  } else {
    return arangodb::Result(TRI_ERROR_NOT_IMPLEMENTED,
                            command + " is not implemented on coordinators");
  }
#endif

  // We'll never get here
  return arangodb::Result();
}

}  // namespace arangodb
