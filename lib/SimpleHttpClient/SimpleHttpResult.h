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
/// @author Dr. Frank Celler
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_SIMPLE_HTTP_CLIENT_SIMPLE_HTTP_RESULT_H
#define ARANGODB_SIMPLE_HTTP_CLIENT_SIMPLE_HTTP_RESULT_H 1

#include "Basics/Common.h"
#include "Basics/StringBuffer.h"

#include <velocypack/Builder.h>
#include <velocypack/Options.h>
#include <velocypack/velocypack-aliases.h>

////////////////////////////////////////////////////////////////////////////////
/// @brief class for storing a request result
////////////////////////////////////////////////////////////////////////////////

namespace arangodb {
namespace httpclient {

class SimpleHttpResult {
 protected:
  SimpleHttpResult(SimpleHttpResult const&);
  SimpleHttpResult& operator=(SimpleHttpResult const&);

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief result types
  //////////////////////////////////////////////////////////////////////////////

  enum resultTypes {
    COMPLETE = 0,
    COULD_NOT_CONNECT,
    WRITE_ERROR,
    READ_ERROR,
    UNKNOWN
  };

 public:
  SimpleHttpResult();

  virtual ~SimpleHttpResult();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief clear result values
  //////////////////////////////////////////////////////////////////////////////

  void clear();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns whether the response contains an HTTP error
  //////////////////////////////////////////////////////////////////////////////

  virtual bool wasHttpError() const { return (_returnCode >= 400); }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the http return code
  //////////////////////////////////////////////////////////////////////////////

  virtual int getHttpReturnCode() const { return _returnCode; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the http return code
  //////////////////////////////////////////////////////////////////////////////

  void setHttpReturnCode(int returnCode) { this->_returnCode = returnCode; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the http return message
  //////////////////////////////////////////////////////////////////////////////

  virtual std::string getHttpReturnMessage() const { return _returnMessage; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the http return message
  //////////////////////////////////////////////////////////////////////////////

  void setHttpReturnMessage(std::string const& message) {
    _returnMessage = message;
  }

  void setHttpReturnMessage(std::string&& message) {
    _returnMessage = std::move(message);
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the response contained a content length header
  //////////////////////////////////////////////////////////////////////////////

  virtual bool hasContentLength() const { return _hasContentLength; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the content length
  //////////////////////////////////////////////////////////////////////////////

  virtual size_t getContentLength() const { return _contentLength; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the content length
  //////////////////////////////////////////////////////////////////////////////

  virtual void setContentLength(size_t len) {
    _contentLength = len;
    _hasContentLength = true;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the http body
  //////////////////////////////////////////////////////////////////////////////

  virtual arangodb::basics::StringBuffer& getBody();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the http body
  //////////////////////////////////////////////////////////////////////////////

  virtual arangodb::basics::StringBuffer const& getBody() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the http body as velocypack
  //////////////////////////////////////////////////////////////////////////////

  virtual std::shared_ptr<VPackBuilder> getBodyVelocyPack() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the request result type
  //////////////////////////////////////////////////////////////////////////////

  virtual enum resultTypes getResultType() const { return _requestResultType; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns true if result type == OK
  //////////////////////////////////////////////////////////////////////////////

  virtual bool isComplete() const { return _requestResultType == COMPLETE; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns true if "transfer-encoding: chunked"
  //////////////////////////////////////////////////////////////////////////////

  virtual bool isChunked() const { return _chunked; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns true if "content-encoding: deflate"
  //////////////////////////////////////////////////////////////////////////////

  virtual bool isDeflated() const { return _deflated; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the request result type
  //////////////////////////////////////////////////////////////////////////////

  virtual void setResultType(enum resultTypes requestResultType) {
    this->_requestResultType = requestResultType;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns a message for the result type
  //////////////////////////////////////////////////////////////////////////////

  virtual std::string getResultTypeMessage() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief add header field
  //////////////////////////////////////////////////////////////////////////////

  virtual void addHeaderField(char const*, size_t);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return the value of a single header
  //////////////////////////////////////////////////////////////////////////////

  virtual std::string getHeaderField(std::string const&, bool&) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief check if a header is present
  //////////////////////////////////////////////////////////////////////////////

  virtual bool hasHeaderField(std::string const&) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get all header fields
  //////////////////////////////////////////////////////////////////////////////

  virtual std::unordered_map<std::string, std::string> const& getHeaderFields() const {
    return _headerFields;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns whether the result is JSON-encoded
  //////////////////////////////////////////////////////////////////////////////

  virtual bool isJson() const { return _isJson; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns whether the request has been sent in its entirety, this
  /// is only meaningful if isComplete() returns false.
  //////////////////////////////////////////////////////////////////////////////

  virtual bool haveSentRequestFully() const { return _haveSentRequestFully; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set haveSentRequestFully
  //////////////////////////////////////////////////////////////////////////////

  virtual void setHaveSentRequestFully(bool b) { _haveSentRequestFully = b; }

 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief add header field
  //////////////////////////////////////////////////////////////////////////////

  void addHeaderField(char const*, size_t, char const*, size_t);

 protected:
  // header informtion
  std::string _returnMessage;
  size_t _contentLength;
  int _returnCode;
  bool _foundHeader;
  bool _isJson;
  bool _hasContentLength;
  bool _chunked;
  bool _deflated;

  // body content
  arangodb::basics::StringBuffer _resultBody;

  // request result type
  enum resultTypes _requestResultType;

  // header fields
  std::unordered_map<std::string, std::string> _headerFields;

  // flag which indicates whether or not the complete request has already be
  // sent (to the operating system):
  bool _haveSentRequestFully;
};
}  // namespace httpclient
}  // namespace arangodb
#endif
