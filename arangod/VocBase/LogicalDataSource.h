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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "Basics/Result.h"
#include "VocBase/Identifiers/DataSourceId.h"

struct TRI_vocbase_t;

namespace arangodb {
namespace velocypack {

class Builder;
class Slice;

}  // namespace velocypack

class LogicalCollection;
class LogicalView;

// A common ancestor to all database objects proving access to documents
// e.g. LogicalCollection / LogicalView
class LogicalDataSource {
 public:
  enum class Category : uint8_t {
    kCollection = 1,
    kView = 2,
  };

  LogicalDataSource(LogicalDataSource const& other) = delete;
  LogicalDataSource& operator=(LogicalDataSource const& other) = delete;

  virtual ~LogicalDataSource() = default;

  Category category() const noexcept { return _category; }

  [[nodiscard]] bool deleted() const noexcept {
    return _deleted.load(std::memory_order_relaxed);
  }

  void setDeleted() noexcept {
    // relaxed here and in load ok because we don't need
    // happens before between them.
    _deleted.store(true, std::memory_order_relaxed);
  }

  virtual Result drop() = 0;

  std::string const& guid() const noexcept { return _guid; }

  DataSourceId id() const noexcept { return _id; }

  std::string const& name() const noexcept { return _name; }

  DataSourceId planId() const noexcept { return _planId; }

  enum class Serialization : uint8_t {
    // object properties will be shown in a list
    List = 0,
    // object properties will be shown
    Properties,
    // object will be saved in storage engine
    Persistence,
    // object will be saved in storage engine
    PersistenceWithInProgress,
    // object will be replicated or dumped/restored
    Inventory
  };

  //////////////////////////////////////////////////////////////////////////////
  /// @brief append a jSON definition of the data-source to the 'builder'
  /// @param build the buffer to append to, must be an open object
  /// @param ctx defines which properties to serialize
  /// @param safe true only for internal, recursive, under lock, usage
  /// @return success
  //////////////////////////////////////////////////////////////////////////////
  Result properties(velocypack::Builder& build, Serialization ctx,
                    bool safe = false) const;

  virtual Result rename(std::string&& newName) = 0;
  bool system() const noexcept { return _system; }
  TRI_vocbase_t& vocbase() const noexcept { return _vocbase; }

 protected:
  // revert a setDeleted() call later. currently only used by LogicalView.
  // TODO: should be removed
  void setUndeleted() noexcept {
    _deleted.store(false, std::memory_order_seq_cst);
  }

  template<typename DataSource, typename... Args>
  explicit LogicalDataSource(DataSource const& /*self*/, Args&&... args)
      : LogicalDataSource{DataSource::category(), std::forward<Args>(args)...} {
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief append implementation-specific values to the data-source definition
  //////////////////////////////////////////////////////////////////////////////
  virtual Result appendVPack(velocypack::Builder&, Serialization, bool) const {
    return {};
  }

  void name(std::string&& name) noexcept { _name = std::move(name); }

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructor for a logical data-source
  /// @note 'id' autogenerated IFF 'id' == 0
  /// @note 'planId' taken from evaluated value of 'id' IFF 'planId' == 0
  /// @note 'guid' autogenerated IFF 'guid'.empty()
  //////////////////////////////////////////////////////////////////////////////
  LogicalDataSource(Category category, TRI_vocbase_t& vocbase, DataSourceId id,
                    std::string&& guid, DataSourceId planId, std::string&& name,
                    bool system, bool deleted);

  // members ordered by sizeof(decltype(..)) except for '_guid'
  std::string _name;           // data-source name
  TRI_vocbase_t& _vocbase;     // the database where the data-source resides
  DataSourceId const _id;      // local data-source id (current database node)
  DataSourceId const _planId;  // global data-source id (cluster-wide)
  // globally unique data-source id (cluster-wide) for proper
  // initialization must be positioned after '_name' and '_planId'
  // since they are autogenerated
  std::string const _guid;
  std::atomic_bool _deleted;  // data-source marked as deleted
  Category const _category;   // the category of the logical data-source
  bool const _system;         // this instance represents a system data-source
};

}  // namespace arangodb
