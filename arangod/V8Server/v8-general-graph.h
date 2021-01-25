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
/// @author Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_V8_SERVER_V8_GENERAL_GRAPH_H
#define ARANGOD_V8_SERVER_V8_GENERAL_GRAPH_H 1

#include <v8.h>

struct TRI_vocbase_t;
struct TRI_v8_global_t;

void TRI_InitV8GeneralGraph(v8::Handle<v8::Context> context, TRI_vocbase_t* vocbase,
                            TRI_v8_global_t* v8g, v8::Isolate* isolate);

#endif
