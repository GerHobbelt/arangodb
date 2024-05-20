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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "StorageEngine/TransactionCollection.h"
#include "Transaction/Methods.h"
#include "VocBase/AccessMode.h"
#include "VocBase/voc-types.h"

namespace arangodb {

namespace transaction {

class Context;
}

class SingleCollectionTransaction final : public transaction::Methods {
 public:
  /// @brief create the transaction, using a data-source
  SingleCollectionTransaction(
      std::shared_ptr<transaction::Context> ctx,
      LogicalDataSource const& collection, AccessMode::Type accessType,
      transaction::Options const& options = transaction::Options());

  /// @brief create the transaction, using a collection name
  SingleCollectionTransaction(
      std::shared_ptr<transaction::Context> ctx, std::string const& name,
      AccessMode::Type,
      transaction::Options const& options = transaction::Options());

  /// @brief end the transaction
  ~SingleCollectionTransaction() = default;

  /// @brief get the underlying document collection
  /// note that we have two identical versions because this is called
  /// in two different situations
  LogicalCollection* documentCollection();

  /// @brief get the underlying collection's id
  DataSourceId cid() const noexcept { return _cid; }

#ifdef USE_ENTERPRISE
  using transaction::Methods::addCollectionAtRuntime;
#endif
  /// @brief add a collection to the transaction for read, at runtime
  /// note that this can only be ourselves
  futures::Future<DataSourceId> addCollectionAtRuntime(
      std::string_view name, AccessMode::Type type) override final;

  /// @brief get the underlying collection's name
  std::string name();

 private:
  /// @brief get the underlying transaction collection
  TransactionCollection* resolveTrxCollection();

  /// @brief collection id
  DataSourceId _cid;

  /// @brief trxCollection cache
  TransactionCollection* _trxCollection;

  /// @brief LogicalCollection* cache
  LogicalCollection* _documentCollection;

  /// @brief collection access type
  AccessMode::Type _accessType;
};

}  // namespace arangodb
