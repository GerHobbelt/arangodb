#pragma once

#include "Async/Registry/promise.h"
#include "Async/coro-utils.h"
#include "Async/expected.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Utils/ExecContext.h"

#include <coroutine>
#include <atomic>
#include <stdexcept>
#include <utility>
#include <source_location>

namespace arangodb {

template<typename T>
struct async_promise;
template<typename T>
struct async;

template<typename T>
struct async_promise_base : async_registry::AddToAsyncRegistry {
  using promise_type = async_promise<T>;

  async_promise_base(std::source_location loc)
      : async_registry::AddToAsyncRegistry{std::move(loc)} {}

  std::suspend_never initial_suspend() noexcept {
    promise_in_registry->state.store(async_registry::State::Running);
    _callerExecContext = ExecContext::currentAsShared();
    return {};
  }
  auto final_suspend() noexcept {
    struct awaitable {
      bool await_ready() noexcept { return false; }
      std::coroutine_handle<> await_suspend(
          std::coroutine_handle<> self) noexcept {
        auto addr = _promise->_continuation.exchange(self.address(),
                                                     std::memory_order_acq_rel);
        if (addr == nullptr) {
          return std::noop_coroutine();
        } else if (addr == self.address()) {
          self.destroy();
          return std::noop_coroutine();
        } else {
          return std::coroutine_handle<>::from_address(addr);
        }
      }
      void await_resume() noexcept {}
      async_promise_base* _promise;
    };
    ExecContext::set(_callerExecContext);
    return awaitable{this};
  }
  template<typename U>
  auto await_transform(
      U&& co_awaited_expression,
      std::source_location loc = std::source_location::current()) noexcept {
    using inner_awaitable_type =
        decltype(get_awaitable_object(std::forward<U>(co_awaited_expression)));

    struct awaitable {
      bool await_ready() { return inner_awaitable.await_ready(); }
      auto await_suspend(std::coroutine_handle<> handle) {
        outer_promise->promise_in_registry->state.store(
            async_registry::State::Suspended);
        ExecContext::set(outer_promise->_callerExecContext);
        return inner_awaitable.await_suspend(handle);
      }
      auto await_resume() {
        auto old_state = outer_promise->promise_in_registry->state.exchange(
            async_registry::State::Running);
        if (old_state == async_registry::State::Suspended) {
          outer_promise->_callerExecContext = ExecContext::currentAsShared();
        }
        ExecContext::set(_myExecContext);
        return inner_awaitable.await_resume();
      }
      async_promise_base<T>* outer_promise;
      inner_awaitable_type inner_awaitable;
      std::shared_ptr<ExecContext const> _myExecContext;
    };

    // update promises in registry
    if constexpr (CanSetPromiseWaiter<U>) {
      co_awaited_expression.set_promise_waiter(this->id());
    }
    update_source_location(loc);

    return awaitable{
        this, get_awaitable_object(std::forward<U>(co_awaited_expression)),
        ExecContext::currentAsShared()};
  }
  void unhandled_exception() { _value.set_exception(std::current_exception()); }
  auto get_return_object() {
    return async<T>{std::coroutine_handle<promise_type>::from_promise(
        *static_cast<promise_type*>(this))};
  }

  std::atomic<void*> _continuation = nullptr;
  expected<T> _value;
  std::shared_ptr<ExecContext const> _callerExecContext;
};

template<typename T>
struct async_promise : async_promise_base<T> {
  async_promise(std::source_location loc = std::source_location::current())
      : async_promise_base<T>(std::move(loc)) {}
  template<typename V = T>
  void return_value(
      V&& v, std::source_location loc = std::source_location::current()) {
    async_registry::AddToAsyncRegistry::update_state(
        async_registry::State::Resolved);
    async_registry::AddToAsyncRegistry::update_source_location(loc);
    async_promise_base<T>::_value.emplace(std::forward<V>(v));
  }
};

template<>
struct async_promise<void> : async_promise_base<void> {
  async_promise(std::source_location loc = std::source_location::current())
      : async_promise_base<void>(std::move(loc)) {}
  void return_void(std::source_location loc = std::source_location::current()) {
    async_registry::AddToAsyncRegistry::update_state(
        async_registry::State::Resolved);
    async_registry::AddToAsyncRegistry::update_source_location(loc);
    async_promise_base<void>::_value.emplace();
  }
};

template<typename T>
struct async {
  using promise_type = async_promise<T>;

  auto operator co_await() && {
    struct awaitable {
      bool await_ready() noexcept {
        return _handle.promise()._continuation.load(
                   std::memory_order_acquire) != nullptr;
      }
      bool await_suspend(std::coroutine_handle<> c) noexcept {
        return _handle.promise()._continuation.exchange(
                   c.address(), std::memory_order_acq_rel) == nullptr;
      }
      auto await_resume() {
        auto& promise = _handle.promise();
        expected<T> r = std::move(promise._value);
        _handle.destroy();
        return std::move(r).get();
      }
      explicit awaitable(std::coroutine_handle<promise_type> handle)
          : _handle(handle) {}

     private:
      std::coroutine_handle<promise_type> _handle;
    };

    auto r = awaitable{_handle};
    _handle = nullptr;
    return r;
  }

  void reset() {
    if (_handle) {
      auto& promise = _handle.promise();
      if (promise._continuation.exchange(
              _handle.address(), std::memory_order_release) != nullptr) {
        _handle.destroy();
      }
      _handle = nullptr;
    }
  }

  bool valid() const noexcept { return _handle != nullptr; }
  operator bool() const noexcept { return valid(); }

  auto set_promise_waiter(void* waiter) {
    _handle.promise().set_promise_waiter(waiter);
  }
  auto id() -> void* { return _handle.promise()->id(); }

  ~async() { reset(); }

  explicit async(std::coroutine_handle<promise_type> h) : _handle(h) {}

  async(async const&) = delete;
  async& operator=(async const&) = delete;

  async(async&& o) noexcept : _handle(std::move(o._handle)) {
    o._handle = nullptr;
  }

  async& operator=(async&& o) noexcept {
    reset();
    std::swap(o._handle, _handle);
    return *this;
  }

 private:
  std::coroutine_handle<promise_type> _handle;
};

}  // namespace arangodb
