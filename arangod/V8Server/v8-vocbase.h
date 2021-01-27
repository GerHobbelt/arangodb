////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_V8_SERVER_V8_VOCBASE_H
#define ARANGOD_V8_SERVER_V8_VOCBASE_H 1

#include "Basics/Common.h"
#include "V8/v8-globals.h"

struct TRI_vocbase_t;

namespace arangodb {

namespace aql {

class QueryRegistry;
}

class CollectionNameResolver;
class JSLoader;

}  // namespace arangodb

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a TRI_vocbase_t global context
////////////////////////////////////////////////////////////////////////////////

void TRI_InitV8VocBridge(v8::Isolate* isolate, v8::Handle<v8::Context> context,
                         arangodb::aql::QueryRegistry* queryRegistry,
                         TRI_vocbase_t& vocbase, size_t threadNumber);

#endif