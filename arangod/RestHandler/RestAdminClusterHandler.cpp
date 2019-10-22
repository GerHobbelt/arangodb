////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "RestAdminClusterHandler.h"

#include <chrono>

#include <velocypack/Iterator.h>
#include <velocypack/Buffer.h>
#include <velocypack/velocypack-aliases.h>

#include "Agency/AsyncAgencyComm.h"
#include "Agency/TimeString.h"
#include "Cluster/AgencyPaths.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ResultT.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/GeneralServer.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/RestHandlerFactory.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Logger/LogMacros.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Scheduler/SchedulerFeature.h"
#include "Sharding/ShardDistributionReporter.h"
#include "Utils/ExecContext.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

using namespace std::chrono_literals;

namespace {

struct agentConfigHealthResult {
  std::string endpoint, name;
  futures::Try<network::Response> response;
};

void removePlanServers (std::unordered_set<std::string> &servers, VPackSlice plan) {
  for (auto const& database : VPackObjectIterator(plan.get("Collections"))) {
    for (auto const& collection : VPackObjectIterator(database.value)) {
      VPackSlice shards = collection.value.get("shards");
      for (auto const& shard : VPackObjectIterator(shards)) {
        for (auto const& server : VPackArrayIterator(shard.value)) {
          servers.erase(server.copyString());
          if (servers.size() == 0) {
            return ;
          }
        }
      }
    }
  }
}

void removeCurrentServers (std::unordered_set<std::string> &servers, VPackSlice current) {
  for (auto const& database : VPackObjectIterator(current.get("Collections"))) {
    for (auto const& collection : VPackObjectIterator(database.value)) {
      for (auto const& shard : VPackObjectIterator(collection.value)) {
        for (auto const& server : VPackArrayIterator(shard.value.get("servers"))) {
          servers.erase(server.copyString());
          if (servers.size() == 0) {
            return ;
          }
        }
      }
    }
  }
}


template<typename T, typename F>
struct delayedCalculator {
  bool constructed;
  T content;
  F constructor;

  delayedCalculator(F &&c) : constructed(false), constructor(std::forward<F>(c)) {}

  T* operator->() {
    if (!constructed) {
      constructed = true;
      constructor(content);
    }
    return &content;
  }
};


void buildHealthResult(VPackBuilder &builder, std::vector<futures::Try<agentConfigHealthResult>> const& config, VPackSlice store) {
  auto rootPath = arangodb::cluster::paths::root()->arango();

  using server_set = std::unordered_set<std::string>;

  auto canBeDeletedConstructor = [&](server_set& set) {
    {
      VPackObjectIterator memberIter(store.get(rootPath->supervision()->health()->vec()));
      for (auto member : memberIter) {
        set.insert(member.key.copyString());
      }
    }

    removePlanServers (set, store.get(rootPath->plan()->vec()));
    removeCurrentServers (set, store.get(rootPath->current()->vec()));
  };
  delayedCalculator<server_set, decltype(canBeDeletedConstructor)> canBeDeleted(std::move(canBeDeletedConstructor));


  struct AgentInformation {
    bool leader;
    double lastAcked;
    AgentInformation() : leader(false), lastAcked(0.0) {}
  };

  std::unordered_map<std::string, AgentInformation> agents;

  // gather information about the agents
  for (auto const& agentTry : config) {
    if (!agentTry.hasValue()) {
      continue; // should never happen
    }

    auto& agent = agentTry.get();
    if (agent.response.hasValue()) {
      auto& response = agent.response.get();
      if (response.ok() && response.response->statusCode() == fuerte::StatusOK) {
        VPackSlice lastAcked = response.slice().get("lastAcked");
        if (lastAcked.isNone()) {
          continue;
        }
        agents[agent.name].leader = true;
        for (const auto& agent : VPackObjectIterator(lastAcked)) {
          agents[agent.key.copyString()].lastAcked = agent.value.get("lastAckedTime").getDouble();
        }
      }
    }
  }

  builder.add("ClusterId", store.get(rootPath->cluster()->vec()));
  {
    VPackObjectBuilder ob(&builder, "Health");

    VPackObjectIterator memberIter(store.get(rootPath->supervision()->health()->vec()));
    for (auto member : memberIter) {
      std::string serverId = member.key.copyString();

      {
        VPackObjectBuilder ob(&builder, serverId);

        builder.add(VPackObjectIterator(member.value));
        if (serverId.substr(0, 4) == "PRMR") {
          builder.add("Role", VPackValue("DBServer"));
          builder.add("CanBeDeleted", VPackValue(VPackValue(
            member.value.get("Status").isEqualString("FAILED") &&
            canBeDeleted->count(serverId) == 1
          )));
        } else if (serverId.substr(0, 4) == "CRDN") {
          builder.add("Role", VPackValue("Coordinator"));
          builder.add("CanBeDeleted", VPackValue(member.value.get("Status").isEqualString("FAILED")));
        }
      }
    }

    for (auto& memberTry : config) {
      if (!memberTry.hasValue()) {
        continue; // should never happen
      }

      auto& member = memberTry.get();

      {
        VPackObjectBuilder ob(&builder, member.name);

        builder.add("Role", VPackValue("Agent"));
        builder.add("Endpoint", VPackValue(member.endpoint));
        builder.add("CanBeDeleted", VPackValue(false));

        // check for additional information
        auto info = agents.find(member.name);
        if (info != agents.end()) {
          builder.add("Leading", VPackValue(info->second.leader));
          builder.add("LastAckedTime", VPackValue(info->second.lastAcked));
        }

        if (member.response.hasValue()) {
          auto& response = member.response.get();
          if (response.ok() && response.response->statusCode() == fuerte::StatusOK) {
            VPackSlice config = response.slice();
            builder.add("Engine", config.get("engine"));
            builder.add("Version", config.get("version"));
            builder.add("Leader", config.get("leaderId"));
            builder.add("Status", VPackValue("GOOD"));
          } else {
            builder.add("Status", VPackValue("BAD"));
          }
        } else {
          builder.add("Status", VPackValue("BAD"));
        }
      }
    }
  }
}

}

RestAdminClusterHandler::RestAdminClusterHandler(application_features::ApplicationServer& server,
                                                         GeneralRequest* request, GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

RestAdminClusterHandler::~RestAdminClusterHandler() {}

std::string const RestAdminClusterHandler::Health = "health";
std::string const RestAdminClusterHandler::NumberOfServers = "numberOfServers";
std::string const RestAdminClusterHandler::Maintenance = "maintenance";
std::string const RestAdminClusterHandler::NodeVersion = "nodeVersion";
std::string const RestAdminClusterHandler::NodeEngine = "nodeEngine";
std::string const RestAdminClusterHandler::NodeStatistics = "nodeStatistics";
std::string const RestAdminClusterHandler::Statistics = "statistics";
std::string const RestAdminClusterHandler::ShardDistribution = "shardDistribution";
std::string const RestAdminClusterHandler::CollectionShardDistribution = "collectionShardDistribution";
std::string const RestAdminClusterHandler::CleanoutServer = "cleanoutServer";
std::string const RestAdminClusterHandler::ResignLeadership = "resignLeadership";
std::string const RestAdminClusterHandler::MoveShard = "moveShard";
std::string const RestAdminClusterHandler::QueryJobStatus = "queryAgencyJob";
std::string const RestAdminClusterHandler::RemoveServer = "removeServer";

RestStatus RestAdminClusterHandler::execute() {

  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  auto const& suffixes = request()->suffixes();
  size_t const len = suffixes.size();

  if (len == 1) {
    std::string const& command = suffixes.at(0);

    if (command == Health) {
      return handleHealth();
    } else if (command == NumberOfServers) {
      return handleNumberOfServers();
    } else if (command == Maintenance) {
      return handleMaintenance();
    } else if (command == NodeVersion) {
      return handleNodeVersion();
    } else if (command == NodeEngine) {
      return handleNodeEngine();
    } else if (command == NodeStatistics) {
      return handleNodeStatistics();
    } else if (command == Statistics) {
      return handleStatistics();
    } else if (command == ShardDistribution) {
      return handleShardDistribution();
    } else if (command == CollectionShardDistribution) {
      return handleCollectionShardDistribution();
    } else if (command == CleanoutServer) {
      return handleCleanoutServer();
    } else if (command == ResignLeadership) {
      return handleResignLeadership();
    } else if (command == MoveShard) {
      return handleMoveShard();
    } else if (command == QueryJobStatus) {
      return handleQueryJobStatus();
    } else if (command == RemoveServer) {
      return handleRemoveServer();
    } else {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    std::string("invalid command '") + command + "'");
      return RestStatus::DONE;
    }
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_SUPERFLUOUS_SUFFICES,
                "expecting URL /_admin/cluster/<command>");
  return RestStatus::DONE;
}


RestAdminClusterHandler::futureVoid RestAdminClusterHandler::retryTryDeleteServer(std::unique_ptr<RemoveServerContext>&& ctx) {
  if (++ctx->tries < 60) {
    return SchedulerFeature::SCHEDULER->delay(1s).thenValue(
      [this, self = shared_from_this(), ctx = std::move(ctx)] (auto) mutable {
        return tryDeleteServer(std::move(ctx));
    });
  } else {
    generateError(rest::ResponseCode::REQUEST_TIMEOUT, TRI_ERROR_HTTP_PRECONDITION_FAILED, "server may not be deleted");
    return futures::makeFuture();
  }
}

RestAdminClusterHandler::futureVoid RestAdminClusterHandler::tryDeleteServer(std::unique_ptr<RemoveServerContext>&& ctx) {

  auto rootPath = arangodb::cluster::paths::root()->arango();
  AgencyReadTransaction trx(std::move(std::vector<std::string>{
    rootPath->supervision()->health()->str(),
    rootPath->plan()->str(),
    rootPath->current()->str()
  }));

  auto self(shared_from_this());
  return AsyncAgencyComm().sendTransaction(20s, std::move(trx))
    .thenValue([self, this, rootPath, ctx = std::move(ctx)](AsyncAgencyCommResult &&result) mutable {

      if (result.ok() && result.statusCode() == 200) {
        VPackSlice agency = result.slice().at(0);

        VPackSlice health = agency.get(rootPath->supervision()->health()->server(ctx->server)->status()->vec());
        if (!health.isNone()) {
          std::unordered_set<std::string> serverSet = {ctx->server};
          removePlanServers(serverSet, agency.get(rootPath->plan()->vec()));
          removeCurrentServers(serverSet, agency.get(rootPath->current()->vec()));

          if (serverSet.size() == 0) {

            auto rootPath = arangodb::cluster::paths::root()->arango();

            // do a write transaction if server is no longer used
            std::vector<AgencyOperation> ops{
              AgencyOperation(rootPath->plan()->coordinators()->server(ctx->server)->str(), AgencySimpleOperationType::DELETE_OP),
              AgencyOperation(rootPath->plan()->dBServers()->server(ctx->server)->str(), AgencySimpleOperationType::DELETE_OP),
              AgencyOperation(rootPath->current()->serversRegistered()->server(ctx->server)->str(), AgencySimpleOperationType::DELETE_OP),
              AgencyOperation(rootPath->current()->dBServers()->server(ctx->server)->str(), AgencySimpleOperationType::DELETE_OP),
              AgencyOperation(rootPath->supervision()->health()->server(ctx->server)->str(), AgencySimpleOperationType::DELETE_OP),
              AgencyOperation(rootPath->target()->mapUniqueToShortID()->server(ctx->server)->str(), AgencySimpleOperationType::DELETE_OP),
              AgencyOperation(rootPath->current()->serversKnown()->server(ctx->server)->str(), AgencySimpleOperationType::DELETE_OP),
              AgencyOperation(rootPath->target()->removedServers()->server(ctx->server)->str(), AgencyValueOperationType::SET,
                timepointToString(std::chrono::system_clock::now())),
            };

/*
      preconditions['/arango/Supervision/Health/' + serverId + '/Status'] = {'old': 'FAILED'};
      preconditions["/arango/Supervision/DBServers/" + serverId]
        = { "oldEmpty": true };
        */

            std::vector<AgencyPrecondition> precs{
              AgencyPrecondition(rootPath->supervision()->health()->server(ctx->server)->status()->str(), AgencyPrecondition::Type::VALUE, "FAILED"),
              AgencyPrecondition(rootPath->supervision()->dbServers()->server(ctx->server)->str(), AgencyPrecondition::Type::EMPTY, true),
            };

            return AsyncAgencyComm().sendTransaction(20s, AgencyWriteTransaction(ops, precs)).thenValue(
              [self, this, ctx = std::move(ctx)](AsyncAgencyCommResult &&result) mutable {
                if (result.ok()) {
                  if (result.statusCode() == fuerte::StatusOK) {
                    resetResponse(rest::ResponseCode::OK);
                    return futures::makeFuture();
                  } else if (result.statusCode() == fuerte::StatusPreconditionFailed) {
                    return retryTryDeleteServer(std::move(ctx));
                  }
                }
                generateError(result.asResult());
                return futures::makeFuture();
            });
          }

          return retryTryDeleteServer(std::move(ctx));
        } else {
          generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND);
        }
      } else {
        generateError(result.asResult());
      }

      return futures::makeFuture();
    });
}

RestStatus RestAdminClusterHandler::handlePostRemoveServer(std::string const& server) {

  auto ctx = std::make_unique<RemoveServerContext>(server);

  auto self(shared_from_this());
  return waitForFuture(tryDeleteServer(std::move(ctx))
    .thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));
}

RestStatus RestAdminClusterHandler::handleRemoveServer() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::POST) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (body.isObject()) {
    VPackSlice server = body.get("server");
    if (server.isString()) {
      std::string serverId = server.copyString();
      // TODO translate serverId
      return handlePostRemoveServer(serverId);
    }
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "object with key `server`");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleCleanoutServer() {
  return handleSingleServerJob("cleanOutServer");
}

RestStatus RestAdminClusterHandler::handleResignLeadership() {
  return handleSingleServerJob("resignLeadership");
}

RestStatus RestAdminClusterHandler::handleMoveShard() {
  generateError(rest::ResponseCode::NOT_IMPLEMENTED, TRI_ERROR_NOT_IMPLEMENTED);
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleQueryJobStatus() {

  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN, "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  std::string jobId = request()->value("id");
  if (jobId.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "missing id parameter");
    return RestStatus::DONE;
  }

  auto targetPath = arangodb::cluster::paths::root()->arango()->target();
  std::vector<std::string> paths = {
    targetPath->pending()->job(jobId)->str(),
    targetPath->failed()->job(jobId)->str(),
    targetPath->finished()->job(jobId)->str(),
    targetPath->toDo()->job(jobId)->str(),
  };

  auto self(shared_from_this());
  return waitForFuture(AsyncAgencyComm().sendTransaction(20s, AgencyReadTransaction{std::move(paths)})
    .thenValue([self, this, targetPath, jobId](AsyncAgencyCommResult &&result) {
      if (result.ok() && result.statusCode() == fuerte::StatusOK) {
        auto paths = std::vector{
          targetPath->pending()->job(jobId)->vec(),
          targetPath->failed()->job(jobId)->vec(),
          targetPath->finished()->job(jobId)->vec(),
          targetPath->toDo()->job(jobId)->vec(),
        };


        for (auto const& path : paths) {
          VPackSlice job = result.slice().at(0).get(path);

          if (job.isObject()) {
            VPackBuffer<uint8_t> payload;
            {
              VPackBuilder builder(payload);
              VPackObjectBuilder ob(&builder);

              // append all the job keys
              builder.add(VPackObjectIterator(job));
              builder.add("error", VPackValue(false));
              builder.add("job", VPackValue(jobId));
              builder.add("status", VPackValue(path[2]));
            }

            resetResponse(rest::ResponseCode::OK);
            response()->setPayload(std::move(payload), true);
            return;
          }
        }

        generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND);
      } else {
        generateError(result.asResult());
      }
    }).thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));
}

RestStatus RestAdminClusterHandler::handleSingleServerJob(std::string const& job) {

  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN, "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::POST) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (body.isObject()) {
    VPackSlice server = body.get("server");
    if (server.isString()) {
      std::string serverId = server.copyString();
      // TODO translate serverId
      return handleCreateSingleServerJob(job, serverId);
    }
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "object with key `server`");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleCreateSingleServerJob(std::string const& job, std::string const& serverId) {

  std::string jobId = std::to_string(server().getFeature<ClusterFeature>().clusterInfo().uniqid());
  auto jobToDoPath = arangodb::cluster::paths::root()->arango()->target()->toDo()->job(jobId);

  VPackBuilder builder;
  {
    VPackObjectBuilder ob(&builder);
    builder.add("type", VPackValue(job));
    builder.add("server", VPackValue(serverId));
    builder.add("jobId", VPackValue(jobId));
    builder.add("creator", VPackValue(ServerState::instance()->getId()));
    builder.add("timeCreated", VPackValue(timepointToString(std::chrono::system_clock::now())));
  }

  auto self(shared_from_this());

  return waitForFuture(AsyncAgencyComm().setValue(20s, jobToDoPath, builder.slice())
    .thenValue([self, this, jobId = std::move(jobId)](AsyncAgencyCommResult &&result) {
      if (result.ok() && result.statusCode() == 200) {
        VPackBuffer<uint8_t> payload;
        {
          VPackBuilder builder(payload);
          VPackObjectBuilder ob(&builder);
          builder.add("error", VPackValue(false));
          builder.add("code", VPackValue(int(ResponseCode::ACCEPTED)));
          builder.add("job", VPackValue(jobId));
        }

        resetResponse(rest::ResponseCode::ACCEPTED);
        response()->setPayload(std::move(payload), true);
      } else {
        generateError(result.asResult());
      }
    }).thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));

  generateError(rest::ResponseCode::NOT_IMPLEMENTED, TRI_ERROR_NOT_IMPLEMENTED);
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleProxyGetRequest(std::string const& url, std::string const& serverFromParameter) {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN, "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  std::string const& serverId = request()->value(serverFromParameter);
  if (serverId.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  std::string("missing parameter `") + serverFromParameter + "`");
    return RestStatus::DONE;
  }

  auto* pool = server().getFeature<NetworkFeature>().pool();

  auto frequest = network::sendRequestRetry(pool, "server:" + serverId, fuerte::RestVerb::Get, url, VPackBuffer<uint8_t>(), 10s);
  auto self(shared_from_this());
  return waitForFuture(std::move(frequest).thenValue([this, self](network::Response &&result) {
    if (result.ok()) {
      if (result.statusCode() == 200) {
        resetResponse(rest::ResponseCode::OK);
        auto payload = result.response->stealPayload();
        response()->setPayload(std::move(*payload), true);
      } else {

      }
    } else {
      switch (result.error) {
        case fuerte::Error::Canceled:
          generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER, "unknown server");
          break;
        case fuerte::Error::CouldNotConnect:
        case fuerte::Error::Timeout:
          generateError(rest::ResponseCode::REQUEST_TIMEOUT, TRI_ERROR_HTTP_GATEWAY_TIMEOUT, "server did not answer");
          break;
        default:
          LOG_DEVEL << "got error: " << int(result.error) << " statuscode: "<< result.statusCode();
          generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR);
      }
    }
    }).thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));
}

RestStatus RestAdminClusterHandler::handleNodeVersion() {
  return handleProxyGetRequest("/_api/version", "ServerID");
}

RestStatus RestAdminClusterHandler::handleNodeStatistics() {
  return handleProxyGetRequest("/_admin/statistics", "ServerID");
}

RestStatus RestAdminClusterHandler::handleNodeEngine() {
  return handleProxyGetRequest("/_api/engine", "ServerID");
}

RestStatus RestAdminClusterHandler::handleStatistics() {
  return handleProxyGetRequest("/_admin/statistics", "DBserver");
}


RestStatus RestAdminClusterHandler::handleShardDistribution() {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN, "only allowed on coordinators");
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  auto reporter = cluster::ShardDistributionReporter::instance(server());
  VPackBuffer<uint8_t> resultBody;
  {
    VPackBuilder result(resultBody);
    VPackObjectBuilder body(&result);
    result.add(VPackValue("results"));
    reporter->getDistributionForDatabase(_vocbase.name(), result);
    result.add("error", VPackValue(false));
    result.add("code", VPackValue(200));
  }
  resetResponse(rest::ResponseCode::OK);
  response()->setPayload(std::move(resultBody), true);
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleGetCollectionShardDistribution(std::string const& collection) {

  if (collection.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "expected nonempty `collection` parameter");
    return RestStatus::DONE;
  }

  auto reporter = cluster::ShardDistributionReporter::instance(server());
  VPackBuffer<uint8_t> resultBody;
  {
    VPackBuilder result(resultBody);
    VPackObjectBuilder body(&result);
    result.add(VPackValue("results"));
    reporter->getCollectionDistributionForDatabase(_vocbase.name(), collection, result);
    result.add("error", VPackValue(false));
    result.add("code", VPackValue(200));
  }
  resetResponse(rest::ResponseCode::OK);
  response()->setPayload(std::move(resultBody), true);
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleCollectionShardDistribution() {
  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN, "only allowed on coordinators");
    return RestStatus::DONE;
  }

  switch (request()->requestType()) {
    case rest::RequestType::GET:
      return handleGetCollectionShardDistribution(request()->value("collection"));
    case rest::RequestType::PUT:
      break;
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
      return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (body.isObject()) {
    VPackSlice collection = body.get("collection");
    if (collection.isString()) {
      return handleGetCollectionShardDistribution(collection.copyString());
    }
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "object with key `collection`");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleGetMaintenance() {

  auto self(shared_from_this());
  auto maintenancePath = arangodb::cluster::paths::root()->arango()->supervision()->state()->mode();

  return waitForFuture(AsyncAgencyComm().getValues(maintenancePath).thenValue(
    [self, this](AgencyReadResult &&result) {
      if (result.ok() && result.statusCode() == fuerte::StatusOK) {
        VPackBuffer<uint8_t> body;
        {
          VPackBuilder bodyBuilder(body);
          VPackObjectBuilder ob(&bodyBuilder);
          bodyBuilder.add("error", VPackValue(false));
          bodyBuilder.add("result", result.value());
        }// use generateOk instead

        resetResponse(rest::ResponseCode::OK);
        response()->setPayload(std::move(body), true);
      } else {
        generateError(result.asResult());
      }
    }).thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));
}

RestAdminClusterHandler::futureVoid RestAdminClusterHandler::waitForSupervisionState(bool state, clock::time_point startTime) {

  auto self(shared_from_this());

  if (startTime == clock::time_point()) {
    startTime = clock::now();
  }

  return SchedulerFeature::SCHEDULER->delay(1s).thenValue(
    [this, self] (auto) {
      return AsyncAgencyComm()
        .getValues(arangodb::cluster::paths::root()->arango()->supervision()->state()->mode());
    })
    .thenValue([this, self, state, startTime] (AgencyReadResult &&result) {
      auto waitFor = state ? "Maintenance" : "Normal";
      if (result.ok() && result.statusCode() == fuerte::StatusOK) {
        if (false == result.value().isEqualString(waitFor)) {
          if (clock::now() - startTime < 120.0s) {
            // wait again
            return waitForSupervisionState(state);
          }

          generateError(rest::ResponseCode::REQUEST_TIMEOUT, TRI_ERROR_HTTP_GATEWAY_TIMEOUT,
            std::string{"timed out while waiting for supervision to go into maintenance mode"});
        } else {

          auto msg = state
            ? "Cluster supervision deactivated. It will be reactivated automatically in 60 minutes unless this call is repeated until then."
            : "Cluster supervision reactivated.";
          VPackBuffer<uint8_t> body;
          {
            VPackBuilder bodyBuilder(body);
            VPackObjectBuilder ob(&bodyBuilder);
            bodyBuilder.add("error", VPackValue(false));
            bodyBuilder.add("warning", VPackValue(msg));
          }

          resetResponse(rest::ResponseCode::OK);
          response()->setPayload(std::move(body), true);
        }
      } else {
        generateError(result.asResult());
      }

      return futures::makeFuture();
    });
}

RestStatus RestAdminClusterHandler::handlePutMaintenance(bool state) {

  auto maintenancePath = arangodb::cluster::paths::root()->arango()->supervision()->maintenance();

  auto sendTransaction = [&] {
    if (state) {
      return AsyncAgencyComm().setValue(60s, maintenancePath, VPackValue(true), 3600);
    } else {
      return AsyncAgencyComm().deleteKey(60s, maintenancePath);
    }
  };

  auto self(shared_from_this());

  return waitForFuture(sendTransaction().thenValue(
    [this, self, state](AsyncAgencyCommResult &&result) {
      if (result.ok() && result.statusCode() == 200) {
        return waitForSupervisionState(state);
      } else {
        generateError(result.asResult());
      }
      return futures::makeFuture();
    }).thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));
}

RestStatus RestAdminClusterHandler::handleMaintenance() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator() && !ServerState::instance()->isSingleServer()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN, "only allowed on single server and coordinators");
    return RestStatus::DONE;
  }

  switch (request()->requestType()) {
    case rest::RequestType::GET:
      return handleGetMaintenance();
    case rest::RequestType::PUT:
      break;
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
      return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (body.isString()) {
    if (body.isEqualString("on")) {
      return handlePutMaintenance(true);
    } else if(body.isEqualString("off")) {
      return handlePutMaintenance(false);
    }
  }

  generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "string expected with value `on` or `off`");
  return RestStatus::DONE;
}

RestStatus RestAdminClusterHandler::handleGetNumberOfServers() {

  auto targetPath = arangodb::cluster::paths::root()->arango()->target();
  AgencyReadTransaction trx(std::move(std::vector<std::string>{
    targetPath->numberOfDBServers()->str(),
    targetPath->numberOfCoordinators()->str(),
    targetPath->cleanedServers()->str()
  }));

  auto self(shared_from_this());

  return waitForFuture(AsyncAgencyComm().sendTransaction(10.0s, trx)
    .thenValue([this, self, targetPath] (AsyncAgencyCommResult &&result) {
      if (result.ok() && result.statusCode() == fuerte::StatusOK) {

        VPackBuffer<uint8_t> body;
        {
          VPackBuilder builder(body);
          VPackObjectBuilder ob(&builder);
          builder.add("numberOfDBServers", result.slice().at(0).get(targetPath->numberOfDBServers()->vec()));
          builder.add("numberOfCoordinators", result.slice().at(0).get(targetPath->numberOfCoordinators()->vec()));
          builder.add("cleanedServers", result.slice().at(0).get(targetPath->cleanedServers()->vec()));
          builder.add("error", VPackValue(false));
          builder.add("code", VPackValue(200));
        }

        resetResponse(rest::ResponseCode::OK);
        response()->setPayload(std::move(body), true);
      } else {
        generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, "agency communication failed");
      }
    }).thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));
}

RestStatus RestAdminClusterHandler::handlePutNumberOfServers() {

  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  bool parseSuccess;
  VPackSlice body = parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }

  if (!body.isObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "object expected");
    return RestStatus::DONE;
  }

  std::vector<AgencyOperation> ops;
  auto targetPath = arangodb::cluster::paths::root()->arango()->target();

  VPackSlice numberOfCoordinators = body.get("numberOfCoordinators");
  if (numberOfCoordinators.isNumber()) {
    ops.emplace_back(targetPath->numberOfCoordinators()->str(), AgencyValueOperationType::SET, numberOfCoordinators);
  } else if (!numberOfCoordinators.isNone()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "numberOfCoordinators: number expected");
    return RestStatus::DONE;
  }

  VPackSlice numberOfDBServers = body.get("numberOfDBServers");
  if (numberOfDBServers.isNumber()) {
    ops.emplace_back(targetPath->numberOfDBServers()->str(), AgencyValueOperationType::SET, numberOfDBServers);
  } else if (!numberOfDBServers.isNone()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "numberOfDBServers: number expected");
    return RestStatus::DONE;
  }

  VPackSlice cleanedServers = body.get("cleanedServers");
  if (cleanedServers.isArray()) {

    bool allStrings = true;
    for (auto server : VPackArrayIterator(cleanedServers)) {
      if (false == server.isString()) {
        allStrings = false;
      }
    }

    if (allStrings) {
      ops.emplace_back(targetPath->cleanedServers()->str(), AgencyValueOperationType::SET, cleanedServers);
    } else {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "cleanedServers: array of strings expected");
      return RestStatus::DONE;
    }
  } else if (!cleanedServers.isNone()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "cleanedServers: array expected");
    return RestStatus::DONE;
  }

  auto self(shared_from_this());
  AgencyWriteTransaction trx(std::move(ops));
  return waitForFuture(AsyncAgencyComm().sendTransaction(20s, std::move(trx))
    .thenValue([this, self](AsyncAgencyCommResult &&result) {
      if (result.ok() && result.statusCode() == fuerte::StatusOK) {
        resetResponse(rest::ResponseCode::OK);
      } else {
        generateError(result.asResult());
      }
    }).thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));
}

RestStatus RestAdminClusterHandler::handleNumberOfServers() {

  if (!ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN, "only allowed on coordinators");
    return RestStatus::DONE;
  }

  switch (request()->requestType()) {
    case rest::RequestType::GET:
      return handleGetNumberOfServers();
    case rest::RequestType::PUT:
      return handlePutNumberOfServers();
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
      return RestStatus::DONE;
  }
}

RestStatus RestAdminClusterHandler::handleHealth() {

  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (request()->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  if (!ServerState::instance()->isCoordinator() && !ServerState::instance()->isSingleServer()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN, "only allowed on single server and coordinators");
    return RestStatus::DONE;
  }

  // TODO handle timeout parameter

  auto self(shared_from_this());

  // query the agency config
  auto fConfig = AsyncAgencyComm().sendWithFailover(fuerte::RestVerb::Get, "/_api/agency/config", 60.0s, VPackBuffer<uint8_t>())
    .thenValue([this, self] (AsyncAgencyCommResult &&result) {

      if (result.fail() || result.statusCode() != fuerte::StatusOK) {
        THROW_ARANGO_EXCEPTION(result.asResult());
      }

      // now connect to all the members and ask for their engine and version
      std::vector<futures::Future<::agentConfigHealthResult>> fs;

      auto* pool = server().getFeature<NetworkFeature>().pool();
      for (auto member : VPackObjectIterator(result.slice().get(std::vector<std::string>{"configuration", "pool"}))) {

        std::string endpoint = member.value.copyString();
        std::string memberName = member.key.copyString();

        auto future = network::sendRequest(pool, endpoint,
          fuerte::RestVerb::Get, "/_api/agency/config", VPackBuffer<uint8_t>(), 5s)
        .then([endpoint = std::move(endpoint), memberName = std::move(memberName)](futures::Try<network::Response> &&resp) {
          return futures::makeFuture(::agentConfigHealthResult{
            std::move(endpoint), std::move(memberName), std::move(resp)});
        });

        fs.emplace_back(std::move(future));
      }

      return futures::collectAll(fs);
    });

  // query information from the store
  auto rootPath = arangodb::cluster::paths::root()->arango();
  AgencyReadTransaction trx(std::move(std::vector<std::string>{
    rootPath->cluster()->str(),
    rootPath->supervision()->health()->str(),
    rootPath->plan()->str(),
    rootPath->current()->str()
  }));
  auto fStore = AsyncAgencyComm().sendTransaction(60.0s, trx);

  return waitForFuture(futures::collect(std::move(fConfig), std::move(fStore)).thenValue(
    [this, self, rootPath] (auto&& result) {
      auto &configResult = std::get<0>(result);
      auto &storeResult = std::get<1>(result);
      if (storeResult.ok() && storeResult.statusCode() == fuerte::StatusOK) {

        VPackBuffer<uint8_t> responseBody;
        {
          VPackBuilder builder(responseBody);
          VPackObjectBuilder ob(&builder);
          ::buildHealthResult (builder, configResult, storeResult.slice().at(0));
          builder.add("error", VPackValue(false));
          builder.add("code", VPackValue(200));
        }
        resetResponse(rest::ResponseCode::OK);
        response()->setPayload(std::move(responseBody), true);
      } else {
        generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, "agency communication failed");
      }
    }).thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    }).thenError<std::exception>([this, self](std::exception const& e) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR, e.what());
    }));
}
