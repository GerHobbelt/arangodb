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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "ExecContext.h"

#include "Auth/UserManager.h"
#include "Basics/StaticStrings.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "VocBase/vocbase.h"

using namespace arangodb;

thread_local std::shared_ptr<ExecContext const> ExecContext::CURRENT = nullptr;

std::shared_ptr<ExecContext const> const ExecContext::Superuser =
    std::make_shared<ExecContext const>(
        ExecContext::ConstructorToken{}, ExecContext::Type::Internal,
        /*name*/ "", /*db*/ "", auth::Level::RW, auth::Level::RW, true);

/// Should always contain a reference to current user context
/*static*/ ExecContext const& ExecContext::current() {
  if (ExecContext::CURRENT != nullptr) {
    return *ExecContext::CURRENT;
  }
  return *ExecContext::Superuser;
}
/// Note that this intentionally returns CURRENT, even if it is a nullptr: This
/// makes it suitable to set CURRENT in another thread.
/*static*/ std::shared_ptr<ExecContext const> ExecContext::currentAsShared() {
  return ExecContext::CURRENT;
}

/// @brief an internal superuser context, is
///        a singleton instance, deleting is an error
/*static*/ ExecContext const& ExecContext::superuser() {
  return *ExecContext::Superuser;
}
/*static*/ std::shared_ptr<ExecContext const> ExecContext::superuserAsShared() {
  return ExecContext::Superuser;
}

ExecContext::ExecContext(ConstructorToken, ExecContext::Type type,
                         std::string const& user, std::string const& database,
                         auth::Level systemLevel, auth::Level dbLevel,
                         bool isAdminUser)
    : _user(user),
      _database(database),
      _type(type),
      _isAdminUser(isAdminUser),
      _systemDbAuthLevel(systemLevel),
      _databaseAuthLevel(dbLevel) {
  TRI_ASSERT(_systemDbAuthLevel != auth::Level::UNDEFINED);
  TRI_ASSERT(_databaseAuthLevel != auth::Level::UNDEFINED);
}

/*static*/ bool ExecContext::isAuthEnabled() {
  AuthenticationFeature* af = AuthenticationFeature::instance();
  TRI_ASSERT(af != nullptr);
  return af->isActive();
}

std::shared_ptr<ExecContext> ExecContext::create(std::string const& user,
                                                 std::string const& dbname) {
  AuthenticationFeature* af = AuthenticationFeature::instance();
  TRI_ASSERT(af != nullptr);
  auth::Level dbLvl = auth::Level::RW;
  auth::Level sysLvl = auth::Level::RW;
  bool isAdminUser = true;
  if (af->isActive()) {
    auth::UserManager* um = af->userManager();
    TRI_ASSERT(um != nullptr);
    if (um == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unable to find userManager instance");
    }
    dbLvl = sysLvl = um->databaseAuthLevel(user, dbname, false);
    if (dbname != StaticStrings::SystemDatabase) {
      sysLvl =
          um->databaseAuthLevel(user, StaticStrings::SystemDatabase, false);
    }
    isAdminUser = (sysLvl == auth::Level::RW);
    if (!isAdminUser && ServerState::readOnly()) {
      isAdminUser = um->databaseAuthLevel(user, StaticStrings::SystemDatabase,
                                          true) == auth::Level::RW;
    }
  }

  return std::make_shared<ExecContext>(ExecContext::ConstructorToken{},
                                       ExecContext::Type::Default, user, dbname,
                                       sysLvl, dbLvl, isAdminUser);
}

bool ExecContext::canUseDatabase(std::string const& db,
                                 auth::Level requested) const {
  if (isInternal() || _database == db) {
    // should be RW for superuser, RO for read-only
    return requested <= _databaseAuthLevel;
  }

  AuthenticationFeature* af = AuthenticationFeature::instance();
  TRI_ASSERT(af != nullptr);
  if (af->isActive()) {
    auth::UserManager* um = af->userManager();
    TRI_ASSERT(um != nullptr);
    if (um == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unable to find userManager instance");
    }
    auth::Level allowed = um->databaseAuthLevel(_user, db);
    return requested <= allowed;
  }
  return true;
}

/// @brief returns auth level for user
auth::Level ExecContext::collectionAuthLevel(std::string const& dbname,
                                             std::string_view coll) const {
  if (isInternal()) {
    // should be RW for superuser, RO for read-only
    return _databaseAuthLevel;
  }

  AuthenticationFeature* af = AuthenticationFeature::instance();
  TRI_ASSERT(af != nullptr);
  if (!af->isActive()) {
    return auth::Level::RW;
  }

  if (coll.starts_with('_')) {
    // handle fixed permissions here outside auth module.
    // TODO: move this block above, such that it takes effect
    //       when authentication is disabled
    if (dbname == StaticStrings::SystemDatabase &&
        coll == StaticStrings::UsersCollection) {
      // _users (only present in _system database)
      return auth::Level::NONE;
    }
    if (coll == StaticStrings::QueuesCollection) {
      // _queues
      return auth::Level::RO;
    }
    if (coll == StaticStrings::FrontendCollection) {
      // _frontend
      return auth::Level::RW;
    }  // intentional fall through
  }

  auth::UserManager* um = af->userManager();
  TRI_ASSERT(um != nullptr);
  if (um == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "unable to find userManager instance");
  }
  return um->collectionAuthLevel(_user, dbname, coll);
}

ExecContextScope::ExecContextScope(std::shared_ptr<ExecContext const> exe)
    : _old(std::move(exe)) {
  std::swap(ExecContext::CURRENT, _old);
}

ExecContextScope::~ExecContextScope() { std::swap(ExecContext::CURRENT, _old); }
