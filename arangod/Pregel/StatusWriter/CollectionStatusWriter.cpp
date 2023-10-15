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
/// @author Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////

#include "CollectionStatusWriter.h"

#include "Aql/Query.h"
#include "Basics/StaticStrings.h"
#include "Transaction/Hints.h"
#include "Transaction/V8Context.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/AccessMode.h"

namespace arangodb {
namespace pregel {
struct ExecutionNumber;
}
}  // namespace arangodb

namespace arangodb::pregel::statuswriter {

CollectionStatusWriter::CollectionStatusWriter(TRI_vocbase_t& vocbase,
                                               ExecutionNumber& executionNumber)
    : _vocbaseGuard(vocbase), _executionNumber(executionNumber) {
  CollectionNameResolver resolver(_vocbaseGuard.database());
  auto logicalCollection =
      resolver.getCollection(StaticStrings::PregelCollection);
  if (logicalCollection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                   StaticStrings::PregelCollection);
  }
  _logicalCollection = std::move(logicalCollection);
};

CollectionStatusWriter::CollectionStatusWriter(TRI_vocbase_t& vocbase)
    : _vocbaseGuard(vocbase) {
  CollectionNameResolver resolver(_vocbaseGuard.database());
  auto logicalCollection =
      resolver.getCollection(StaticStrings::PregelCollection);
  if (logicalCollection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                   StaticStrings::PregelCollection);
  }
  _logicalCollection = std::move(logicalCollection);
};

auto CollectionStatusWriter::createResult(velocypack::Slice data)
    -> OperationResult {
  if (_executionNumber.value == 0) {
    return OperationResult(Result(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND), {});
  }
  OperationData opData(_executionNumber.value, data);

  auto accessModeType = AccessMode::Type::WRITE;
  SingleCollectionTransaction trx(ctx(), StaticStrings::PregelCollection,
                                  accessModeType);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);
  OperationOptions options(ExecContext::current());

  Result transactionResult = trx.begin();
  if (transactionResult.fail()) {
    return OperationResult{std::move(transactionResult), options};
  }

  auto payload = inspection::serializeWithErrorT(opData);
  return handleOperationResult(
      trx, options, transactionResult,
      trx.insert(StaticStrings::PregelCollection, payload->slice(), {}));
}

auto CollectionStatusWriter::readResult() -> OperationResult {
  if (_executionNumber.value == 0) {
    return OperationResult(Result(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND), {});
  }
  OperationData opData(_executionNumber.value);
  auto accessModeType = AccessMode::Type::READ;
  SingleCollectionTransaction trx(ctx(), StaticStrings::PregelCollection,
                                  accessModeType);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);
  OperationOptions options(ExecContext::current());

  // begin transaction
  Result transactionResult = trx.begin();
  if (transactionResult.fail()) {
    return OperationResult{std::move(transactionResult), options};
  }
  auto payload = inspection::serializeWithErrorT(opData);
  return handleOperationResult(
      trx, options, transactionResult,
      trx.documentAsync(StaticStrings::PregelCollection, payload->slice(), {})
          .get());
}

auto CollectionStatusWriter::readAllNonExpiredResults() -> OperationResult {
  // TODO: GORDO-1607
  // Note: As soon as we introduce an inspectable struct to the data we actually
  // write into the pregel collection, we can remove change "entry.data" to
  // just "entry".
  std::string queryString = R"(
    FOR entry IN _pregel_queries
      FILTER DATE_DIFF(DATE_NOW(), DATE_TIMESTAMP(entry.data.expires), "s") >= 0
      OR entry.data.expires == null
    RETURN entry.data
  )";

  return executeQuery(queryString);
}

auto CollectionStatusWriter::readAllResults() -> OperationResult {
  // TODO: GORDO-1607
  std::string queryString = "FOR entry IN _pregel_queries RETURN entry";
  return executeQuery(queryString);
}

auto CollectionStatusWriter::updateResult(velocypack::Slice data)
    -> OperationResult {
  if (_executionNumber.value == 0) {
    return OperationResult(Result(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND), {});
  }
  OperationData opData(_executionNumber.value, data);

  auto accessModeType = AccessMode::Type::WRITE;
  SingleCollectionTransaction trx(ctx(), StaticStrings::PregelCollection,
                                  accessModeType);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);
  OperationOptions options(ExecContext::current());

  Result transactionResult = trx.begin();
  if (transactionResult.fail()) {
    return OperationResult{std::move(transactionResult), options};
  }
  auto payload = inspection::serializeWithErrorT(opData);
  return handleOperationResult(
      trx, options, transactionResult,
      trx.update(StaticStrings::PregelCollection, payload->slice(), {}));
}

auto CollectionStatusWriter::deleteResult() -> OperationResult {
  if (_executionNumber.value == 0) {
    return OperationResult(Result(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND), {});
  }
  OperationData opData(_executionNumber.value);

  auto accessModeType = AccessMode::Type::WRITE;
  SingleCollectionTransaction trx(ctx(), StaticStrings::PregelCollection,
                                  accessModeType);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);
  OperationOptions options(ExecContext::current());

  Result transactionResult = trx.begin();
  if (transactionResult.fail()) {
    return OperationResult{std::move(transactionResult), options};
  }
  auto payload = inspection::serializeWithErrorT(opData);
  return handleOperationResult(
      trx, options, transactionResult,
      trx.remove(StaticStrings::PregelCollection, payload->slice(), {}));
}

auto CollectionStatusWriter::deleteAllResults() -> OperationResult {
  auto accessModeType = AccessMode::Type::WRITE;
  SingleCollectionTransaction trx(ctx(), StaticStrings::PregelCollection,
                                  accessModeType);
  trx.addHint(transaction::Hints::Hint::NONE);
  OperationOptions options(ExecContext::current());

  Result transactionResult = trx.begin();
  if (transactionResult.fail()) {
    return OperationResult{std::move(transactionResult), options};
  }
  return handleOperationResult(
      trx, options, transactionResult,
      trx.truncateAsync(StaticStrings::PregelCollection, options).get());
}

auto CollectionStatusWriter::executeQuery(std::string queryString)
    -> OperationResult {
  auto query = arangodb::aql::Query::create(
      ctx(), arangodb::aql::QueryString(queryString), nullptr);
  query->queryOptions().skipAudit = true;
  aql::QueryResult queryResult = query->executeSync();
  if (queryResult.result.fail()) {
    if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
        (queryResult.result.is(TRI_ERROR_QUERY_KILLED))) {
      return OperationResult(Result(TRI_ERROR_REQUEST_CANCELED), {});
    }
    return OperationResult(queryResult.result, {});
  }

  return OperationResult(Result(TRI_ERROR_NO_ERROR), queryResult.data->buffer(),
                         {});
}

auto CollectionStatusWriter::handleOperationResult(
    SingleCollectionTransaction& trx, OperationOptions& options,
    Result& transactionResult, OperationResult&& opRes) const
    -> OperationResult {
  transactionResult = trx.finish(opRes.result);
  if (transactionResult.fail() && opRes.ok()) {
    return OperationResult{std::move(transactionResult), options};
  }
  return opRes;
};

auto CollectionStatusWriter::ctx()
    -> std::shared_ptr<transaction::Context> const {
  return transaction::V8Context::CreateWhenRequired(_vocbaseGuard.database(),
                                                    false);
}

}  // namespace arangodb::pregel::statuswriter
