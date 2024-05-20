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
/// @author Max Neunhoeffer
/// @author Jan Steemann
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "AqlValueMaterializer.h"

#include "Basics/debugging.h"
#include "Transaction/Context.h"
#include "Transaction/Methods.h"

#include <velocypack/Slice.h>

using namespace arangodb;
using namespace aql;

AqlValueMaterializer::AqlValueMaterializer(velocypack::Options const* options)
    : options(options), materialized(), hasCopied(false) {}

AqlValueMaterializer::AqlValueMaterializer(AqlValueMaterializer const& other)
    : options(other.options),
      materialized(other.materialized),
      hasCopied(other.hasCopied) {
  if (other.hasCopied) {
    // copy other's slice
    materialized = other.materialized.clone();
  }
}

AqlValueMaterializer& AqlValueMaterializer::operator=(
    AqlValueMaterializer const& other) {
  if (this != &other) {
    TRI_ASSERT(options == other.options);  // must be from same transaction
    options = other.options;
    if (hasCopied) {
      // destroy our own slice
      materialized.destroy();
      hasCopied = false;
    }
    // copy other's slice
    materialized = other.materialized.clone();
    hasCopied = other.hasCopied;
  }
  return *this;
}

AqlValueMaterializer::AqlValueMaterializer(
    AqlValueMaterializer&& other) noexcept
    : options(other.options),
      materialized(other.materialized),
      hasCopied(other.hasCopied) {
  // reset other
  other.hasCopied = false;
  // cppcheck-suppress *
  other.materialized = AqlValue();
}

AqlValueMaterializer& AqlValueMaterializer::operator=(
    AqlValueMaterializer&& other) noexcept {
  if (this != &other) {
    TRI_ASSERT(options == other.options);  // must be from same transaction
    options = other.options;
    if (hasCopied) {
      // destroy our own slice
      materialized.destroy();
    }
    // reset other
    materialized = other.materialized;
    hasCopied = other.hasCopied;
    other.materialized = AqlValue();
  }
  return *this;
}

AqlValueMaterializer::~AqlValueMaterializer() {
  if (hasCopied) {
    materialized.destroy();
  }
}

velocypack::Slice AqlValueMaterializer::slice(AqlValue const& value) {
  materialized = value.materialize(options, hasCopied);
  return materialized.slice();
}
