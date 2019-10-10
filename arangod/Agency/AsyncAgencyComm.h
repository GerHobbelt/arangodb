////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Andreas Streichardt
////////////////////////////////////////////////////////////////////////////////
#include <fuerte/message.h>

#include <deque>
#include <mutex>
#include <memory>

#include "Network/Methods.h"
#include "Futures/Future.h"
#include "Agency/AgencyComm.h"
#include "Cluster/ResultT.h"
#include "Cluster/PathComponent.h"


namespace arangodb {

struct AsyncAgencyCommResult {
  arangodb::fuerte::Error error;
  std::unique_ptr<arangodb::fuerte::Response> response;

  bool ok() const {
    return arangodb::fuerte::Error::NoError == this->error;
  }

  VPackSlice slice() {
    return response->slice();
  }

  arangodb::fuerte::StatusCode statusCode() {
    return response->statusCode();
  }

  Result asResult() {
    if (!ok()) {
      return Result{int(error), arangodb::fuerte::to_string(error)};
    } else if(200 <= statusCode() && statusCode() <= 299) {
      return Result{};
    } else {
      return Result{int(statusCode())};
    }
  }
};

struct AgencyReadResult : public AsyncAgencyCommResult {
  AgencyReadResult(AsyncAgencyCommResult &&result, VPackSlice value) : AsyncAgencyCommResult(std::move(result)), _value(value) {}
  VPackSlice value() { return this->_value; }
private:
  VPackSlice _value;
};


class AsyncAgencyComm;

class AsyncAgencyCommManager final {
public:
  static std::unique_ptr<AsyncAgencyCommManager> INSTANCE;

  static void initialize() {
    INSTANCE = std::make_unique<AsyncAgencyCommManager>();
  };

  void addEndpoint(std::string const& endpoint);
  void updateEndpoints(std::vector<std::string> const& endpoints);

  std::deque<std::string> const& endpoints() { return _endpoints; }

  std::string getCurrentEndpoint();
  void reportError(std::string const& endpoint);
  void reportRedirect(std::string const& endpoint, std::string const& redirectTo);

  network::ConnectionPool *pool() const { return _pool; };
  void pool(network::ConnectionPool *pool) { _pool = pool; };

private:
  std::mutex _lock;
  std::deque<std::string> _endpoints;
  network::ConnectionPool *_pool;
};

class AsyncAgencyComm final {
public:
  using FutureResult = arangodb::futures::Future<AsyncAgencyCommResult>;
  using FutureReadResult = arangodb::futures::Future<AgencyReadResult>;

  FutureResult getValues(std::string const& path) const;
  FutureReadResult getValues(std::shared_ptr<const arangodb::cluster::paths::Path> const& path) const;

public:
  FutureResult sendWithFailover(arangodb::fuerte::RestVerb method, std::string const& url, network::Timeout timeout, velocypack::Buffer<uint8_t>&& body) const;
  FutureResult sendWithFailover(arangodb::fuerte::RestVerb method, std::string const& url, network::Timeout timeout, AgencyTransaction const& trx) const;

  AsyncAgencyComm() : _manager(AsyncAgencyCommManager::INSTANCE.get()) {}
  AsyncAgencyComm(AsyncAgencyCommManager *manager) : _manager(manager) {}
  AsyncAgencyComm(AsyncAgencyCommManager &manager) : _manager(&manager) {}
private:
  AsyncAgencyCommManager *_manager;
};


}
