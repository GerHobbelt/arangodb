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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <functional>
#include <iosfwd>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace arangodb::cluster::paths {

struct SkipComponents {
  constexpr SkipComponents() : num(0) {}
  explicit constexpr SkipComponents(std::size_t num) : num(num) {}

  std::size_t num;
};

class Path {
 public:
  // Call for each component on the path, starting with the topmost component,
  // excluding Root.
  virtual void forEach(
      std::function<void(char const* component)> const&) const = 0;

  // Fold the path.
  template<class T>
  auto fold(std::function<T(const char*, T)> const& callback, T init) const
      -> T {
    forEach(
        [&callback, &init](const char* component) { init = callback(init); });
    return std::move(init);
  }

  auto toStream(std::ostream& stream,
                SkipComponents skip = SkipComponents()) const -> std::ostream& {
    forEach([&stream, &skip](const char* component) {
      if (skip.num == 0) {
        stream << "/" << component;
      } else {
        --skip.num;
      }
    });
    return stream;
  }

  [[nodiscard]] auto vec(SkipComponents skip = SkipComponents()) const
      -> std::vector<std::string> {
    std::vector<std::string> res;
    forEach([&res, &skip](const char* component) {
      if (skip.num == 0) {
        res.emplace_back(component);
      } else {
        --skip.num;
      }
    });
    return res;
  }

  [[nodiscard]] auto str(SkipComponents skip = SkipComponents()) const
      -> std::string {
    auto stream = std::stringstream{};
    toStream(stream, skip);
    return stream.str();
  }

  virtual ~Path() = default;
};

template<class T, class P>
class StaticComponent : public std::enable_shared_from_this<T> /* (sic) */,
                        public Path {
 public:
  using ParentType = P;
  using BaseType = StaticComponent<T, P>;

  StaticComponent() = delete;

  void forEach(
      std::function<void(char const* component)> const& callback) const final {
    parent().forEach(callback);
    callback(child().component());
  }

  // Only the parent type P may instantiate a component, so make this protected
  // and P a friend.
 protected:
  friend P;
  explicit constexpr StaticComponent(std::shared_ptr<P const> parent) noexcept
      : _parent(std::move(parent)) {}

  // shared ptr constructor
  static auto make_shared(std::shared_ptr<P const> parent)
      -> std::shared_ptr<T const> {
    struct ConstructibleT : public T {
     public:
      explicit ConstructibleT(std::shared_ptr<P const> parent) noexcept
          : T(std::move(parent)) {}
    };
    return std::make_shared<ConstructibleT const>(std::move(parent));
  }

 private:
  // Accessor to our subclass
  auto child() const -> T const& { return static_cast<T const&>(*this); }

  // Accessor to our parent. Could be made public, but should then probably
  // return the shared_ptr.
  auto parent() const noexcept -> P const& { return *_parent; }

  std::shared_ptr<P const> const _parent;
};

template<class T, class P, class V>
class DynamicComponent : public std::enable_shared_from_this<T> /* (sic) */,
                         public Path {
 public:
  using ParentType = P;
  using BaseType = DynamicComponent<T, P, V>;

  DynamicComponent() = delete;

  void forEach(
      std::function<void(char const* component)> const& callback) const final {
    parent().forEach(callback);
    callback(child().component());
  }

  // Only the parent type P may instantiate a component, so make this protected
  // and P a friend.
 protected:
  friend P;
  explicit constexpr DynamicComponent(std::shared_ptr<P const> parent,
                                      V value) noexcept
      : _parent(std::move(parent)), _value(std::move(value)) {
    // cppcheck-suppress *
    static_assert(noexcept(V(std::move(value))),
                  "Move constructor of V is expected to be noexcept");
  }

  // shared ptr constructor
  static auto make_shared(std::shared_ptr<P const> parent, V value)
      -> std::shared_ptr<T const> {
    struct ConstructibleT : public T {
     public:
      explicit ConstructibleT(std::shared_ptr<P const> parent, V value) noexcept
          : T(std::move(parent), std::move(value)) {}
    };
    return std::make_shared<ConstructibleT const>(std::move(parent),
                                                  std::move(value));
  }

  auto value() const noexcept -> V const& { return _value; }

 private:
  // Accessor to our subclass
  auto child() const -> T const& { return static_cast<T const&>(*this); }

  // Accessor to our parent. Could be made public, but should then probably
  // return the shared_ptr.
  auto parent() const noexcept -> P const& { return *_parent; }

  std::shared_ptr<P const> const _parent;

  V const _value;
};

auto operator<<(std::ostream& stream, Path const& path) -> std::ostream&;

}  // namespace arangodb::cluster::paths
