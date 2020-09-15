////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
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

#include "Strings.h"
#include <velocypack/Collection.h>
#include <velocypack/velocypack-aliases.h>

#include "../Extractor.h"

using namespace arangodb::greenspun;

EvalResult Prim_StringHuh(Machine& ctx, VPackSlice const slice, VPackBuilder& result) {
  auto res = extract<VPackSlice>(slice);
  if (res.fail()) {
    return res.error();
  }

  auto&& [value] = res.value();
  result.add(VPackValue(value.isString()));
  return {};
}

EvalResult Prim_StringLength(Machine& ctx, VPackSlice const slice, VPackBuilder& result) {
  auto res = extract<std::string_view>(slice);
  if (res.fail()) {
    return res.error();
  }

  auto&& [str] = res.value();
  result.add(VPackValue(str.length()));
  return {};
}

EvalResult Prim_StringRef(Machine& ctx, VPackSlice const slice, VPackBuilder& result) {
  auto res = extract<std::string_view, double>(slice);
  if (res.fail()) {
    return res.error();
  }

  auto&& [str, idx] = res.value();
  if (idx >= str.length()) {
    return EvalError("index out of bounds");
  }

  char x = str.operator[](idx);
  result.add(VPackValue(&x));
  return {};
}

EvalResult Prim_StringSet(Machine& ctx, VPackSlice const slice, VPackBuilder& result) {
  auto res = extract<std::string, double, std::string_view>(slice);
  if (res.fail()) {
    return res.error();
  }

  auto&& [str, idx, c] = res.value();
  if (idx >= str.length()) {
    return EvalError("index out of bounds");
  }

  if (c.length() != 1) {
    return EvalError("expected single character to set");
  }

  str.operator[](idx) = c.at(0);
  result.add(VPackValue(std::move(str)));
  return {};
}

EvalResult Prim_StringCopy(Machine& ctx, VPackSlice const slice, VPackBuilder& result) {
  return EvalError("not implemented");
}

EvalResult Prim_StringAppend(Machine& ctx, VPackSlice const slice, VPackBuilder& result) {
  return EvalError("not implemented");
}

EvalResult Prim_ListJoin(Machine& ctx, VPackSlice const slice, VPackBuilder& result) {
  auto res = extract<VPackArrayIterator, std::string_view>(slice);
  if (res.fail()) {
    return res.error();
  }

  std::string resStr;

  auto&& [iter, delim] = res.value();
  for (auto&& str : iter) {
    if (!str.isString()) {
      return EvalError("expected string, found: " + str.toJson());
    }

    if (!resStr.empty()) {
      resStr += delim;
    }
    resStr += str.stringView();
  }
  result.add(VPackValue(resStr));
  return {};
}

void arangodb::greenspun::RegisterAllStringFunctions(Machine& ctx) {
  ctx.setFunction("string?", Prim_StringHuh);
  ctx.setFunction("string-length", Prim_StringLength);
  ctx.setFunction("string-ref", Prim_StringRef);
  ctx.setFunction("string-set", Prim_StringSet);
  ctx.setFunction("string-copy", Prim_StringCopy);
  ctx.setFunction("string-append", Prim_StringAppend);
  ctx.setFunction("string-cat", Prim_StringAppend);
  ctx.setFunction("list-join", Prim_ListJoin);
}
