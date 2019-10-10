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
////////////////////////////////////////////////////////////////////////////////

#include "RestSupervisionStateHandler.h"

#include <chrono>

#include "Cluster/AgencyPaths.h"
#include "Agency/AsyncAgencyComm.h"
#include "Cluster/ResultT.h"
#include "GeneralServer/GeneralServer.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/RestHandlerFactory.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Scheduler/SchedulerFeature.h"
#include "Utils/ExecContext.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

RestSupervisionStateHandler::RestSupervisionStateHandler(application_features::ApplicationServer& server,
                                                         GeneralRequest* request, GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

RestSupervisionStateHandler::~RestSupervisionStateHandler() {}

RestStatus RestSupervisionStateHandler::execute() {
  if (!ExecContext::current().isAdminUser()) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_HTTP_FORBIDDEN);
    return RestStatus::DONE;
  }

  if (_request->requestType() != rest::RequestType::GET) {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }

  auto self(shared_from_this());

  using namespace std::chrono_literals;

  return waitForFuture(AsyncAgencyComm().getValues(arangodb::cluster::paths::root()->arango()->target())
    .thenValue([this, self](AgencyReadResult &&result) {
      if (result.ok() && result.statusCode() == fuerte::StatusOK) {

        VPackBuffer<uint8_t> response;
        {
          VPackBuilder bodyBuilder(response);
          VPackObjectBuilder ob(&bodyBuilder);
          bodyBuilder.add("ToDo", result.value().get("ToDo"));
          bodyBuilder.add("Pending", result.value().get("Pending"));
          bodyBuilder.add("Finished", result.value().get("Finished"));
          bodyBuilder.add("Failed", result.value().get("Failed"));
        }

        resetResponse(rest::ResponseCode::OK);
        _response->setPayload(std::move(response), true);
      } else {
        generateError(result.asResult());
      }
    })
    .thenError<VPackException>([this, self](VPackException const& e) {
      generateError(Result{e.errorCode(), e.what()});
    })
    .thenError<std::exception>([this, self](std::exception const&) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_HTTP_SERVER_ERROR);
    }));
}
