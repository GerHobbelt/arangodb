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
/// @author Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "RandomMask.h"

#include "Maskings/Maskings.h"
#include "Random/RandomGenerator.h"

#include <memory>

using namespace arangodb;
using namespace arangodb::maskings;

ParseResult<AttributeMasking> RandomMask::create(Path path, Maskings* maskings,
                                                 velocypack::Slice /*def*/) {
  return ParseResult<AttributeMasking>(AttributeMasking(
      std::move(path), std::make_shared<RandomMask>(maskings)));
}

void RandomMask::mask(bool value, velocypack::Builder& out,
                      std::string&) const {
  int64_t result = RandomGenerator::interval(static_cast<int64_t>(0),
                                             static_cast<int64_t>(1));

  out.add(VPackValue(result % 2 == 0));
}

void RandomMask::mask(int64_t, velocypack::Builder& out, std::string&) const {
  int64_t result = RandomGenerator::interval(static_cast<int64_t>(-1000),
                                             static_cast<int64_t>(1000));

  out.add(VPackValue(result));
}

void RandomMask::mask(double, velocypack::Builder& out, std::string&) const {
  int64_t result = RandomGenerator::interval(static_cast<int64_t>(-1000),
                                             static_cast<int64_t>(1000));

  out.add(VPackValue(1.0 * result / 100));
}
