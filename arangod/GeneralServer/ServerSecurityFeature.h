////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_APPLICATION_FEATURES_SERVER_SECURITY_FEATURE_H
#define ARANGODB_APPLICATION_FEATURES_SERVER_SECURITY_FEATURE_H 1

#include "ApplicationFeatures/ApplicationFeature.h"

namespace arangodb {

class ServerSecurityFeature final : public application_features::ApplicationFeature {
 public:
  explicit ServerSecurityFeature(application_features::ApplicationServer& server);

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;

  bool canAccessHardenedApi() const;
  bool isRestApiHardened() const;
  bool enableWebInterface() const;
  bool enableFoxxApi() const;
  bool enableFoxxStore() const;
  bool enableFoxxApps() const;
  bool enableJavaScriptTasksApi() const;
  bool enableJavaScriptTransactionsApi() const;

 private:
  bool _enableWebInterface;
  bool _enableFoxxApi;
  bool _enableFoxxStore;
  bool _enableFoxxApps;
  bool _enableJavaScriptTasksApi;
  bool _enableJavaScriptTransactionsApi;
  bool _hardenedRestApi;
};

}  // namespace arangodb

#endif
