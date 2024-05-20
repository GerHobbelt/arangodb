////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
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

#pragma once

#include "Basics/Common.h"

#include "Endpoint/Endpoint.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/IoContext.h"

namespace arangodb::rest {

/// Abstract class handling the socket acceptor
class Acceptor {
 public:
  Acceptor(rest::GeneralServer& server, rest::IoContext& context,
           Endpoint* endpoint);
  virtual ~Acceptor() = default;

 public:
  virtual void open() = 0;
  virtual void close() = 0;
  virtual void cancel() = 0;

  /// start accepting connections
  virtual void asyncAccept() = 0;

 public:
  static std::unique_ptr<Acceptor> factory(rest::GeneralServer& server,
                                           rest::IoContext& context, Endpoint*);

 protected:
  void handleError(asio_ns::error_code const&);
  static constexpr int maxAcceptErrors = 128;

  rest::GeneralServer& _server;
  rest::IoContext& _ctx;
  Endpoint* _endpoint;

  bool _open;
  size_t _acceptFailures;
};
}  // namespace arangodb::rest
