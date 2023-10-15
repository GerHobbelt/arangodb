////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#include "SimpleHttpResult.h"
#include "Basics/Exceptions.h"
#include "Basics/EncodingUtils.h"
#include "Basics/NumberUtils.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"

#include <velocypack/Parser.h>

#include <string_view>

using namespace arangodb::basics;

namespace {
constexpr std::string_view contentLength("content-length");
constexpr std::string_view contentEncoding("content-encoding");
constexpr std::string_view transferEncoding("transfer-encoding");
constexpr std::string_view chunked("chunked");
constexpr std::string_view deflate("deflate");
}  // namespace

namespace arangodb {
namespace httpclient {

////////////////////////////////////////////////////////////////////////////////
/// constructor and destructor
////////////////////////////////////////////////////////////////////////////////

SimpleHttpResult::SimpleHttpResult()
    : _returnMessage(),
      _contentLength(0),
      _returnCode(0),
      _foundHeader(false),
      _hasContentLength(false),
      _chunked(false),
      _deflated(false),
      _haveSentRequestFully(false),
      _requestResultType(UNKNOWN),
      _resultBody(false) {
  _resultBody.ensureNullTerminated();
}

SimpleHttpResult::~SimpleHttpResult() = default;

////////////////////////////////////////////////////////////////////////////////
/// public methods
////////////////////////////////////////////////////////////////////////////////

void SimpleHttpResult::clear() {
  _returnMessage = "";
  _contentLength = 0;
  _returnCode = 0;
  _foundHeader = false;
  _hasContentLength = false;
  _chunked = false;
  _deflated = false;
  _requestResultType = UNKNOWN;
  _headerFields.clear();
  _resultBody.clear();
  _resultBody.ensureNullTerminated();
  _haveSentRequestFully = false;
}

StringBuffer& SimpleHttpResult::getBody() { return _resultBody; }

StringBuffer const& SimpleHttpResult::getBody() const { return _resultBody; }

std::shared_ptr<VPackBuilder> SimpleHttpResult::getBodyVelocyPack() const {
  std::string uncompressed;
  std::string_view data(_resultBody.c_str(), _resultBody.length());

  // transparently handle compression
  bool found = false;
  std::string encoding = getHeaderField(StaticStrings::ContentEncoding, found);
  if (found) {
    if (encoding == StaticStrings::EncodingGzip) {
      auto res = encoding::gzipUncompress(
          reinterpret_cast<uint8_t const*>(data.data()), data.size(),
          uncompressed);
      if (res != TRI_ERROR_NO_ERROR) {
        THROW_ARANGO_EXCEPTION(res);
      }
      data = uncompressed;
    } else if (encoding == StaticStrings::EncodingDeflate) {
      auto res =
          encoding::gzipInflate(reinterpret_cast<uint8_t const*>(data.data()),
                                data.size(), uncompressed);
      if (res != TRI_ERROR_NO_ERROR) {
        THROW_ARANGO_EXCEPTION(res);
      }
      data = uncompressed;
    }
  }

  VPackParser parser(&VelocyPackHelper::looseRequestValidationOptions);
  parser.parse(data);
  return parser.steal();
}

void SimpleHttpResult::addHeaderField(char const* line, size_t length) {
  auto find = static_cast<char const*>(
      memchr(static_cast<void const*>(line), ':', length));

  if (find == nullptr) {
    find = static_cast<char const*>(
        memchr(static_cast<void const*>(line), ' ', length));
  }

  if (find != nullptr) {
    size_t l = find - line;
    addHeaderField(line, l, find + 1, length - l - 1);
  }
}

void SimpleHttpResult::addHeaderField(char const* key, size_t keyLength,
                                      char const* value, size_t valueLength) {
  // trim key
  {
    char const* end = key + keyLength;

    while (key < end && (*key == ' ' || *key == '\t')) {
      ++key;
      --keyLength;
    }
  }

  // lower-case key
  std::string keyString(key, keyLength);
  StringUtils::tolowerInPlace(keyString);

  // trim value
  {
    char const* end = value + valueLength;

    while (value < end && (*value == ' ' || *value == '\t')) {
      ++value;
      --valueLength;
    }
  }

  if (keyString[0] == 'h') {
    if (!_foundHeader && (keyString == "http/1.1" || keyString == "http/1.0")) {
      if (valueLength > 2) {
        _foundHeader = true;

        // we assume the status code is 3 chars long
        if ((value[0] >= '0' && value[0] <= '9') &&
            (value[1] >= '0' && value[1] <= '9') &&
            (value[2] >= '0' && value[2] <= '9')) {
          // set response code
          setHttpReturnCode(100 * (value[0] - '0') + 10 * (value[1] - '0') +
                            (value[2] - '0'));

          if (_returnCode == 204) {
            // HTTP 204 = No content. Assume we will have a content-length of 0.
            // note that the value can be overridden later if the response has
            // the content-length header set to some other value
            setContentLength(0);
          }
        }

        if (valueLength >= 4) {
          setHttpReturnMessage(std::string(value + 4, valueLength - 4));
        }
      }
    }
  }

  else if (keyString[0] == 'c') {
    if (keyString == ::contentLength) {
      setContentLength(
          NumberUtils::atoi_zero<size_t>(value, value + valueLength));
    } else if (keyString == ::contentEncoding) {
      if (valueLength == ::deflate.size() &&
          (value[0] == 'd' || value[0] == 'D') &&
          (value[1] == 'e' || value[1] == 'E') &&
          (value[2] == 'f' || value[2] == 'F') &&
          (value[3] == 'l' || value[3] == 'L') &&
          (value[4] == 'a' || value[4] == 'A') &&
          (value[5] == 't' || value[5] == 'T') &&
          (value[6] == 'e' || value[6] == 'E')) {
        _deflated = true;
      }
    }
  }

  else if (keyString[0] == 't') {
    if (keyString == ::transferEncoding) {
      if (valueLength == ::chunked.size() &&
          (value[0] == 'c' || value[0] == 'C') &&
          (value[1] == 'h' || value[1] == 'H') &&
          (value[2] == 'u' || value[2] == 'U') &&
          (value[3] == 'n' || value[3] == 'N') &&
          (value[4] == 'k' || value[4] == 'K') &&
          (value[5] == 'e' || value[5] == 'E') &&
          (value[6] == 'd' || value[6] == 'D')) {
        _chunked = true;
      }
    }
  }

  _headerFields[std::move(keyString)] = std::string(value, valueLength);
}

std::string SimpleHttpResult::getHeaderField(std::string const& name,
                                             bool& found) const {
  auto find = _headerFields.find(name);

  if (find == _headerFields.end()) {
    found = false;
    return StaticStrings::Empty;
  }

  found = true;
  return (*find).second;
}

bool SimpleHttpResult::hasHeaderField(std::string const& name) const {
  return _headerFields.find(name) != _headerFields.end();
}

}  // namespace httpclient
}  // namespace arangodb
