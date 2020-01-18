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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "H2Connection.h"

#include <fuerte/helper.h>
#include <fuerte/loop.h>
#include <fuerte/message.h>
#include <fuerte/types.h>
#include <velocypack/velocypack-aliases.h>

#include <regex>

#include "debugging.h"

namespace arangodb { namespace fuerte { inline namespace v1 { namespace http {
namespace fu = arangodb::fuerte::v1;
using arangodb::fuerte::v1::SocketType;
using arangodb::velocypack::StringRef;

template <SocketType T>
/*static*/ int H2Connection<T>::on_begin_headers(nghttp2_session* session,
                                                 const nghttp2_frame* frame,
                                                 void* user_data) {
  FUERTE_LOG_HTTPTRACE << "on_begin_headers " << frame->hd.stream_id << "\n";

  // only care about (first) response headers
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }

  H2Connection<T>* me = static_cast<H2Connection<T>*>(user_data);
  Stream* strm = me->findStream(frame->hd.stream_id);
  if (strm) {
    strm->response = std::make_unique<fuerte::Response>();
    return 0;
  } else {  // reset the stream
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
}

template <SocketType T>
/*static*/ int H2Connection<T>::on_header(nghttp2_session* session,
                                          const nghttp2_frame* frame,
                                          const uint8_t* name, size_t namelen,
                                          const uint8_t* value, size_t valuelen,
                                          uint8_t flags, void* user_data) {
  H2Connection<T>* me = static_cast<H2Connection<T>*>(user_data);
  int32_t stream_id = frame->hd.stream_id;
  FUERTE_LOG_HTTPTRACE << "on_header " << stream_id;

  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }

  FUERTE_LOG_HTTPTRACE << "got HEADER frame for stream " << stream_id << "\n";

  Stream* strm = me->findStream(stream_id);
  if (!strm) {
    FUERTE_LOG_HTTPTRACE << "HEADER frame for unkown stream " << stream_id
                         << "\n";
    return 0;
  }

  // handle pseudo headers
  // https://http2.github.io/http2-spec/#rfc.section.8.1.2.3
  StringRef field(reinterpret_cast<const char*>(name), namelen);
  StringRef val(reinterpret_cast<const char*>(value), valuelen);

  if (StringRef(":status") == field) {
    strm->response->header.responseCode =
        (StatusCode)std::stoul(val.toString());
  } else if (field == fu_content_length_key) {
    size_t len = std::min<size_t>(std::stoul(val.toString()), 1024 * 1024 * 64);
    strm->data.reserve(len);
  } else {  // fall through
    strm->response->header.addMeta(field.toString(), val.toString());
    // TODO limit max header size ??
  }

  return 0;
}

template <SocketType T>
int H2Connection<T>::on_frame_recv(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   void* user_data) {
  H2Connection<T>* me = static_cast<H2Connection<T>*>(user_data);

  const int32_t stream_id = frame->hd.stream_id;
  FUERTE_LOG_HTTPTRACE << "on_frame_recv " << stream_id << "\n";

  switch (frame->hd.type) {
    case NGHTTP2_DATA:
    case NGHTTP2_HEADERS: {
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        Stream* strm = me->findStream(stream_id);
        if (strm) {
          FUERTE_LOG_HTTPTRACE << "finalized response on stream " << stream_id
                               << "\n";
          strm->response->setPayload(std::move(strm->data), /*offset*/ 0);
          strm->callback(Error::NoError, std::move(strm->request),
                         std::move(strm->response));
          me->_streams.erase(stream_id);

          uint32_t cc =
              me->_streamCount.fetch_sub(1, std::memory_order_relaxed);
          FUERTE_ASSERT(cc > 0);
        }
      }
      break;
    }
  }

  return 0;
}

template <SocketType T>
/*static*/ int H2Connection<T>::on_data_chunk_recv(
    nghttp2_session* session, uint8_t flags, int32_t stream_id,
    const uint8_t* data, size_t len, void* user_data) {
  FUERTE_LOG_HTTPTRACE << "DATA frame for stream " << stream_id << "\n";

  H2Connection<T>* me = static_cast<H2Connection<T>*>(user_data);
  Stream* strm = me->findStream(stream_id);
  if (strm) {
    strm->data.append(data, len);
  }

  return 0;
}

template <SocketType T>
/*static*/ int H2Connection<T>::on_stream_close(nghttp2_session* session,
                                                int32_t stream_id,
                                                uint32_t error_code,
                                                void* user_data) {
  FUERTE_LOG_HTTPTRACE << "closing stream " << stream_id << " error ("
                       << error_code << ")\n";
  H2Connection<T>* me = static_cast<H2Connection<T>*>(user_data);

  if (error_code != NGHTTP2_NO_ERROR) {
    Stream* strm = me->findStream(stream_id);
    if (strm) {
      strm->invokeOnError(fuerte::Error::ProtocolError);
      uint32_t cc = me->_streamCount.fetch_sub(1, std::memory_order_relaxed);
      FUERTE_ASSERT(cc > 0);
    }
  }
  me->_streams.erase(stream_id);

  return 0;
}

template <SocketType T>
/*static*/ int H2Connection<T>::on_frame_not_send(nghttp2_session* session,
                                                  const nghttp2_frame* frame,
                                                  int lib_error_code,
                                                  void* user_data) {
  if (frame->hd.type != NGHTTP2_HEADERS) {
    return 0;
  }
  FUERTE_LOG_HTTPTRACE << "frame not send";

  // Issue RST_STREAM so that stream does not hang around.
  nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                            NGHTTP2_INTERNAL_ERROR);

  return 0;
}

namespace {
int on_error_callback(nghttp2_session* session, int lib_error_code,
                      const char* msg, size_t len, void*) {
  FUERTE_LOG_DEBUG << "http2 error: \"" << std::string(msg, len) << "\" ("
                   << lib_error_code << ")";
  return 0;
}

constexpr uint32_t window_size = (1 << 30) - 1;  // 1 GiB
void populateSettings(std::array<nghttp2_settings_entry, 3>& iv) {
  // 64 streams matches the queue capacity
  iv[0] = {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 64};
  // typically client is just a *sink* and just process data as
  // much as possible.  Use large window size by default.
  iv[1] = {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window_size};
  iv[2] = {NGHTTP2_SETTINGS_ENABLE_PUSH, 0};
}

void submitConnectionPreface(nghttp2_session* session) {
  std::array<nghttp2_settings_entry, 3> iv;
  populateSettings(iv);

  nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
  // increase connection window size up to window_size
  nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0, 1 << 30);
}

std::string makeAuthHeader(fu::detail::ConnectionConfiguration const& config) {
  std::string auth;
  // preemtively cache authentication
  if (config._authenticationType == AuthenticationType::Basic) {
    auth.append("Basic ");
    auth.append(fu::encodeBase64(config._user + ":" + config._password));
  } else if (config._authenticationType == AuthenticationType::Jwt) {
    if (config._jwtToken.empty()) {
      throw std::logic_error("JWT token is not set");
    }
    auth.append("bearer ");
    auth.append(config._jwtToken);
  }
  return auth;
}
}  // namespace

template <SocketType T>
H2Connection<T>::H2Connection(EventLoopService& loop,
                              fu::detail::ConnectionConfiguration const& config)
    : fuerte::GeneralConnection<T>(loop, config),
      _queue(),
      _authHeader(makeAuthHeader(config)) {}

template <SocketType T>
H2Connection<T>::~H2Connection() try {
  drainQueue(Error::Canceled);
  abortOngoingRequests(Error::Canceled);
  nghttp2_session_del(_session);
  _session = nullptr;
} catch (...) {
}

/// init h2 session
template <SocketType T>
void H2Connection<T>::initNgHttp2Session() {
  nghttp2_session_callbacks* callbacks;
  int rv = nghttp2_session_callbacks_new(&callbacks);
  if (rv != 0) {
    throw std::runtime_error("out ouf memory");
  }

  // Set ALPN "h2" advertisement on connection
  if constexpr (T == SocketType::Ssl) {
    SSL_set_alpn_protos(this->_proto.socket.native_handle(),
                        (const unsigned char*)"\x02h2", 3);
  }

  nghttp2_session_callbacks_set_on_begin_headers_callback(
      callbacks, H2Connection<T>::on_begin_headers);
  nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                   H2Connection<T>::on_header);
  nghttp2_session_callbacks_set_on_frame_recv_callback(
      callbacks, H2Connection<T>::on_frame_recv);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, H2Connection<T>::on_data_chunk_recv);
  nghttp2_session_callbacks_set_on_stream_close_callback(
      callbacks, H2Connection<T>::on_stream_close);
  //  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks,
  //  H2Connection<T>::on_frame_send);
  nghttp2_session_callbacks_set_on_frame_not_send_callback(
      callbacks, H2Connection<T>::on_frame_not_send);
  nghttp2_session_callbacks_set_error_callback2(callbacks, on_error_callback);

  if (_session) {  // this might be called again if we reconnect
    nghttp2_session_del(_session);
  }

  rv = nghttp2_session_client_new(&_session, callbacks, /*args*/ this);
  if (rv != 0) {
    nghttp2_session_callbacks_del(callbacks);
    throw std::runtime_error("out ouf memory");
  }
  nghttp2_session_callbacks_del(callbacks);
}

// sendRequest prepares a RequestItem for the given parameters
// and adds it to the send queue.
template <SocketType T>
void H2Connection<T>::sendRequest(std::unique_ptr<Request> req,
                                  RequestCallback cb) {
  // Create RequestItem from parameters
  auto item = std::make_unique<Stream>();
  item->callback = cb;
  item->request = std::move(req);
  // set the point-in-time when this request expires
  if (item->request->timeout().count() > 0) {
    item->expires = std::chrono::steady_clock::now() + item->request->timeout();
  } else {
    item->expires = std::chrono::steady_clock::time_point::max();
  }

  // Add item to send queue
  if (!_queue.push(item.get())) {
    FUERTE_LOG_ERROR << "connection queue capacity exceeded\n";
    item->invokeOnError(Error::QueueCapacityExceeded);
    return;
  }
  item.release();  // queue owns this now

  this->_numQueued.fetch_add(1, std::memory_order_relaxed);
  FUERTE_LOG_HTTPTRACE << "queued item: this=" << this << "\n";

  // _state.load() after queuing request, to prevent race with connect
  Connection::State state = this->_state.load(std::memory_order_acquire);
  if (state == Connection::State::Connected) {
    FUERTE_LOG_HTTPTRACE << "sendRequest (vst): start sending & reading\n";
    startWriting();  // try to start write loop

  } else if (state == Connection::State::Disconnected) {
    FUERTE_LOG_HTTPTRACE << "sendRequest (vst): not connected\n";
    this->startConnection();
  } else if (state == Connection::State::Failed) {
    FUERTE_LOG_ERROR << "queued request on failed connection\n";
    drainQueue(fuerte::Error::ConnectionClosed);
  }
}

template <SocketType T>
std::size_t H2Connection<T>::requestsLeft() const {
  uint32_t qd = this->_numQueued.load(std::memory_order_relaxed);
  qd += _streamCount.load(std::memory_order_relaxed);
  return qd;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

// socket connection is up without TLS
template <SocketType T>
void H2Connection<T>::finishConnect() {
  FUERTE_LOG_HTTPTRACE << "finishInitialization (h2)\n";

  std::array<nghttp2_settings_entry, 3> iv;
  populateSettings(iv);

  std::string packed(3 * 6, ' ');
  ssize_t nwrite = nghttp2_pack_settings_payload(
      (uint8_t*)packed.data(), packed.size(), iv.data(), iv.size());
  if (nwrite < 0) {
    this->shutdownConnection(Error::ProtocolError);
    return;
  }
  packed.resize(static_cast<size_t>(nwrite));
  std::string encoded = fu::encodeBase64(packed);

  // lets do the HTTP2 session upgrade right away
  initNgHttp2Session();

  // this will submit the settings field for us
  ssize_t rv = nghttp2_session_upgrade2(_session, (uint8_t const*)packed.data(),
                                        packed.size(), /*head*/ 0, nullptr);
  if (rv < 0) {
    this->shutdownConnection(Error::ProtocolError);
    return;
  }

  auto req = std::make_shared<std::string>();
  req->append("GET / HTTP/1.1\r\nConnection: Upgrade, HTTP2-Settings\r\n");
  req->append("Upgrade: h2c\r\nHTTP2-Settings: ");
  req->append(encoded);
  req->append("\r\n\r\n");

  std::cout << "sending request '" << *req << "'\n";

  auto self = Connection::shared_from_this();
  asio_ns::async_write(
      this->_proto.socket, asio_ns::buffer(req->data(), req->size()),
      [self](asio_ns::error_code const& ec, std::size_t nsend) {
        auto& me = static_cast<H2Connection<T>&>(*self);
        if (ec) {
          me.shutdownConnection(Error::WriteError, ec.message());
        } else {
          me.readSwitchingProtocolsResponse();
        }
      });
}

template <SocketType T>
void H2Connection<T>::readSwitchingProtocolsResponse() {
  auto self = Connection::shared_from_this();
  this->_proto.timer.expires_after(std::chrono::seconds(5));
  this->_proto.timer.async_wait([self](auto ec) {
    if (!ec) {
      self->cancel();
    }
  });
  asio_ns::async_read_until(
      this->_proto.socket, this->_receiveBuffer, "\r\n\r\n",
      [self](asio_ns::error_code const& ec, size_t nread) {
        auto& me = static_cast<H2Connection<T>&>(*self);
        me._proto.timer.cancel();
        if (ec) {
          me.shutdownConnection(Error::ProtocolError);
          return;
        }

        // server should respond with 101 and "Upgrade: h2c"
        auto it = asio_ns::buffers_begin(me._receiveBuffer.data());
        std::string header(it, it + static_cast<ptrdiff_t>(nread));
        if (header.compare(0, 12, "HTTP/1.1 101") == 0 &&
            header.find("Upgrade: h2c\r\n") != std::string::npos) {
          FUERTE_ASSERT(nread == header.size());
          me._receiveBuffer.consume(nread);
          me._state.store(Connection::State::Connected);

          // submit a ping so the connection is not closed right away
          nghttp2_submit_ping(me._session, NGHTTP2_FLAG_NONE, nullptr);
          me.startWriting();  // starts writing queue if non-empty
        } else {
          FUERTE_ASSERT(false);
          me.shutdownConnection(Error::ProtocolError);
        }
      });
}

// socket connection is up (with optional SSL), now initiate the VST protocol.
template <>
void H2Connection<SocketType::Ssl>::finishConnect() {
  this->_state.store(Connection::State::Connected);

  initNgHttp2Session();

  // send client connection preface
  submitConnectionPreface(_session);

  // submit a ping so the connection is not closed right away
  nghttp2_submit_ping(_session, NGHTTP2_FLAG_NONE, nullptr);

  startWriting();  // starts writing queue if non-empty
}

// ------------------------------------
// Writing data
// ------------------------------------

// Thread-Safe: activate the writer loop (if off and items are queud)
template <SocketType T>
void H2Connection<T>::startWriting() {
  FUERTE_ASSERT(this->_state.load(std::memory_order_acquire) ==
                Connection::State::Connected);
  FUERTE_LOG_HTTPTRACE << "startWriting: this=" << this << "\n";
  bool tmp = _signaledWrite.load();
  if (!tmp && !_signaledWrite.exchange(true)) {
    this->_io_context->dispatch([self = Connection::shared_from_this(), this] {
      _signaledWrite.store(false);
      // we have been in a race with shutdownConnection()
      Connection::State state = this->_state.load();
      if (state != Connection::State::Connected) {
        //        this->_writing.store(false);
        if (state == Connection::State::Disconnected) {
          this->startConnection();
        }
      } else {
        this->doWrite();
        this->asyncReadSome();
      }
    });
  }
}

// queue the response onto the session, call only on IO thread
template <SocketType T>
void H2Connection<T>::queueHttp2Requests() {
  int numQueued = 0;  // make sure we do send too many request

  Stream* tmp = nullptr;
  while (numQueued++ < 4 && _queue.pop(tmp)) {
    std::unique_ptr<Stream> strm(tmp);

    FUERTE_LOG_HTTPTRACE << "queued request " << this << "\n";

    fuerte::Request& req = *strm->request;
    // we need a continous block of memory for headers
    std::vector<nghttp2_nv> nva;
    nva.reserve(4 + req.header.meta().size());

    std::string verb = fuerte::to_string(req.header.restVerb);
    nva.push_back({(uint8_t*)":method", (uint8_t*)verb.data(), 7, verb.size(),
                   NGHTTP2_NV_FLAG_NO_COPY_NAME});

    if constexpr (T == SocketType::Tcp) {
      nva.push_back(
          {(uint8_t*)":scheme", (uint8_t*)"http", 7, 4,
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});
    } else {
      nva.push_back(
          {(uint8_t*)":scheme", (uint8_t*)"https", 7, 5,
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});
    }

    nva.push_back(
        {(uint8_t*)":path", (uint8_t*)req.header.path.data(), 5,
         req.header.path.size(),
         NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});

    nva.push_back(
        {(uint8_t*)":authority", (uint8_t*)this->_config._host.c_str(), 10,
         this->_config._host.size(),
         NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});

    std::string type;
    if (req.header.restVerb != RestVerb::Get &&
        req.contentType() != ContentType::Custom) {
      type = to_string(req.contentType());
      nva.push_back({(uint8_t*)"content-type", (uint8_t*)type.c_str(), 12,
                     type.length(), NGHTTP2_NV_FLAG_NO_COPY_NAME});
    }
    std::string accept;
    if (req.acceptType() != ContentType::Custom) {
      accept = to_string(req.acceptType());
      nva.push_back({(uint8_t*)"accept", (uint8_t*)accept.c_str(), 6,
                     type.length(), NGHTTP2_NV_FLAG_NO_COPY_NAME});
    }

    bool haveAuth = false;
    for (auto const& pair : req.header.meta()) {
      if (pair.first == fu_content_length_key) {
        continue;  // skip content-length header
      }

      if (pair.first == fu_authorization_key) {
        haveAuth = true;
      }
      nva.push_back(
          {(uint8_t*)pair.first.data(), (uint8_t*)pair.second.data(),
           pair.first.size(), pair.second.size(),
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});
    }

    if (!haveAuth && !_authHeader.empty()) {
      nva.push_back(
          {(uint8_t*)"authorization", (uint8_t*)_authHeader.data(), 13,
           _authHeader.size(),
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});
    }

    nghttp2_data_provider *prd_ptr = nullptr, prd;

    std::string len;
    if (req.header.restVerb != RestVerb::Get &&
        req.header.restVerb != RestVerb::Head) {
      len = std::to_string(req.payloadSize());
      nva.push_back({(uint8_t*)"content-length", (uint8_t*)len.c_str(), 14,
                     len.length(), NGHTTP2_NV_FLAG_NO_COPY_NAME});

      prd.source.ptr = strm.get();
      prd.read_callback = [](nghttp2_session* session, int32_t stream_id,
                             uint8_t* buf, size_t length, uint32_t* data_flags,
                             nghttp2_data_source* source,
                             void* user_data) -> ssize_t {
        auto strm = static_cast<H2Connection<T>::Stream*>(source->ptr);

        auto payload = strm->request->payload();

        // TODO do not copy the body if it is > 16kb
        FUERTE_ASSERT(payload.size() > strm->responseOffset);
        const uint8_t* src = reinterpret_cast<uint8_t const*>(payload.data()) +
                             strm->responseOffset;
        size_t len = std::min(length, payload.size() - strm->responseOffset);
        FUERTE_ASSERT(len > 0);
        std::copy_n(src, len, buf);

        strm->responseOffset += len;
        if (strm->responseOffset == payload.size()) {
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }

        return static_cast<ssize_t>(len);
      };
      prd_ptr = &prd;
    }

    int32_t sid = nghttp2_submit_request(_session, /*pri_spec*/ nullptr,
                                         nva.data(), nva.size(), prd_ptr,
                                         /*stream_user_data*/ nullptr);
    if (sid < 0) {
      this->shutdownConnection(Error::ProtocolError);
      return;
    }
    FUERTE_LOG_HTTPTRACE << "enqueuing stream " << sid << " to "
                         << req.header.path << "\n";
    _streams.emplace(sid, std::move(strm));
    _streamCount.fetch_add(1, std::memory_order_relaxed);
  }
}

// writes data from task queue to network using asio_ns::async_write
template <SocketType T>
void H2Connection<T>::doWrite() {
  FUERTE_LOG_HTTPTRACE << "doWrite\n";

  if (_writing) {
    return;
  }
  _writing = true;

  queueHttp2Requests();

  std::array<asio_ns::const_buffer, 2> outBuffers;

  size_t len = 0;
  while (true) {
    const uint8_t* data;
    ssize_t rv = nghttp2_session_mem_send(_session, &data);
    if (rv < 0) {  // error
      this->shutdownConnection(Error::ProtocolError);
      return;
    }
    if (rv == 0) {  // done
      break;
    }

    const size_t nread = static_cast<size_t>(rv);
    // if the data is long we just pass it to async_write
    if (len + nread > _outbuffer.size()) {
      outBuffers[1] = asio_ns::buffer(data, nread);
      break;
    }

    std::copy_n(data, nread, std::begin(_outbuffer) + len);
    len += nread;
  }
  outBuffers[0] = asio_ns::buffer(_outbuffer, len);

  if (asio_ns::buffer_size(outBuffers) == 0) {
    if (shouldStop()) {
      this->shutdownConnection(Error::CloseRequested);
    }
    _writing = false;
    return;
  }

  // Reset read timer here, because normally client is sending
  // something, it does not expect timeout while doing it.
  setTimeout();

  asio_ns::async_write(this->_proto.socket, outBuffers,
                       [self = this->shared_from_this()](
                           asio_ns::error_code const& ec, std::size_t) {
                         auto& me = static_cast<H2Connection<T>&>(*self);
                         me._writing = false;
                         if (ec) {
                           me.restartConnection(Error::WriteError);
                         } else {
                           me.doWrite();
                         }
                       });

  FUERTE_LOG_HTTPTRACE << "doWrite: done\n";
}

// ------------------------------------
// Reading data
// ------------------------------------

// asyncReadCallback is called when asyncReadSome is resulting in some data.
template <SocketType T>
void H2Connection<T>::asyncReadCallback(asio_ns::error_code const& ec) {
  if (ec) {
    FUERTE_LOG_VSTTRACE
        << "asyncReadCallback: Error while reading form socket: "
        << ec.message();
    this->restartConnection(translateError(ec, Error::ReadError));
    return;
  }

  // Inspect the data we've received so far.
  size_t parsedBytes = 0;
  for (auto const& buffer : this->_receiveBuffer.data()) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.data());

    ssize_t rv = nghttp2_session_mem_recv(_session, data, buffer.size());
    if (rv < 0) {
      this->shutdownConnection(Error::ProtocolError);
      return;  // stop read loop
    }

    parsedBytes += static_cast<size_t>(rv);
  }

  FUERTE_ASSERT(parsedBytes < std::numeric_limits<size_t>::max());
  // Remove consumed data from receive buffer.
  this->_receiveBuffer.consume(parsedBytes);

  doWrite();

  if (!_writing && shouldStop()) {
    this->shutdownConnection(Error::CloseRequested);
    return;  // stop read loop
  }

  setTimeout();

  this->asyncReadSome();  // Continue read loop
}

// adjust the timeouts (only call from IO-Thread)
template <SocketType T>
void H2Connection<T>::setTimeout() {
  // set to smallest point in time
  auto expires = std::chrono::steady_clock::time_point::max();
  if (_streams.empty()) {  // use default connection timeout
    expires = std::chrono::steady_clock::now() + this->_config._idleTimeout;
  } else {
    for (auto const& pair : _streams) {
      expires = std::max(expires, pair.second->expires);
    }
  }

  this->_proto.timer.expires_at(expires);
  this->_proto.timer.async_wait(
      [self = Connection::weak_from_this()](asio_ns::error_code const& ec) {
        std::shared_ptr<Connection> s;
        if (ec || !(s = self.lock())) {  // was canceled / deallocated
          return;
        }

        auto& me = static_cast<H2Connection<T>&>(*s);
        // cancel expired requests
        auto now = std::chrono::steady_clock::now();
        auto it = me._streams.begin();
        while (it != me._streams.end()) {
          if (it->second->expires < now) {
            FUERTE_LOG_DEBUG << "HTTP2-Request timeout\n";
            it->second->invokeOnError(Error::Timeout);
            it = me._streams.erase(it);
          } else {
            it++;
          }
        }

        if (me._streams.empty()) {  // no more messages to wait on
          FUERTE_LOG_DEBUG << "HTTP2-Connection timeout\n";
          // shouldWrite() == false after GOAWAY frame is send
          nghttp2_session_terminate_session(me._session, 0);
          me.doWrite();
        } else {
          me.setTimeout();
        }
      });
}

/// abort ongoing / unfinished requests (locally)
template <SocketType T>
void H2Connection<T>::abortOngoingRequests(const fuerte::Error err) {
  FUERTE_LOG_HTTPTRACE << "aborting ongoing requests";
  // Cancel all streams
  _streams.clear();
  _streamCount.store(0);
}

/// abort all requests lingering in the queue
template <SocketType T>
void H2Connection<T>::drainQueue(const fuerte::Error ec) {
  Stream* item = nullptr;
  while (_queue.pop(item)) {
    std::unique_ptr<Stream> guard(item);
    this->_numQueued.fetch_sub(1, std::memory_order_relaxed);
    guard->invokeOnError(ec);
  }
}

template <SocketType T>
typename H2Connection<T>::Stream* H2Connection<T>::findStream(
    int32_t sid) const {
  auto const& it = _streams.find(sid);
  if (it != _streams.end()) {
    return it->second.get();
  }
  return nullptr;
}

/// should close connection
template <SocketType T>
bool H2Connection<T>::shouldStop() const {
  return !nghttp2_session_want_read(_session) &&
         !nghttp2_session_want_write(_session);
}

template class arangodb::fuerte::v1::http::H2Connection<SocketType::Tcp>;
template class arangodb::fuerte::v1::http::H2Connection<SocketType::Ssl>;
#ifdef ASIO_HAS_LOCAL_SOCKETS
template class arangodb::fuerte::v1::http::H2Connection<SocketType::Unix>;
#endif

}}}}  // namespace arangodb::fuerte::v1::http
