////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <v8.h>

#include "Auth/AuthUser.h"
#include "Auth/DatabaseResource.h"
#include "Auth/User.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Utils/ExecContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"

struct TRI_vocbase_t;
struct TRI_v8_global_t;

namespace arangodb {
struct ViewFactory;

namespace tests {
namespace mocks {
class MockServer;
class MockAqlServer;
}

class TestHelper {
 public:
  static void v8Init();

  // ---------------------------------------------------------------------------
  // Mock Servers
  // ---------------------------------------------------------------------------

 public:
  arangodb::tests::mocks::MockAqlServer* mockAqlServerInit();
  arangodb::tests::mocks::MockAqlServer* mockAqlServer();

 protected:
  std::unique_ptr<arangodb::tests::mocks::MockAqlServer> _mockAqlServer;

  // ---------------------------------------------------------------------------
  // V8
  // ---------------------------------------------------------------------------

 public:
  void v8Setup(TRI_vocbase_t*);
  v8::Isolate* v8Isolate();
  v8::Local<v8::Context> v8Context();
  TRI_v8_global_t* v8Globals();


  void callFunction(v8::Handle<v8::Value>, std::vector<v8::Local<v8::Value>>&);

  void callFunctionThrow(v8::Handle<v8::Value>,
                         std::vector<v8::Local<v8::Value>>&, int errorCode);

 protected:
  std::shared_ptr<v8::Isolate> _v8Isolate;
  v8::Local<v8::Context> _v8Context;
  std::unique_ptr<TRI_v8_global_t> _v8Globals;

  // -----------------------------------------------------------------------------
  // ExecContext
  // -----------------------------------------------------------------------------

 public:
  arangodb::ExecContext* createExecContext(auth::AuthUser const&,
                                           auth::DatabaseResource const&);

  void createUser(std::string const& username, std::function<void(auth::User*)> callback);

  TRI_vocbase_t* createDatabase(std::string const& dbName);

  std::shared_ptr<arangodb::LogicalCollection> createCollection(TRI_vocbase_t*,
                                                                auth::CollectionResource const&);

  std::shared_ptr<arangodb::LogicalView> createView(TRI_vocbase_t*,
                                                    auth::CollectionResource const&);

 protected:
  std::unique_ptr<arangodb::ExecContext> exec;

  // -----------------------------------------------------------------------------
  // Views
  // -----------------------------------------------------------------------------

 public:
  void viewFactoryInit(mocks::MockServer*);

 protected:
  std::unique_ptr<arangodb::ViewFactory> _viewFactory;
};
}  // namespace tests
}  // namespace arangodb
