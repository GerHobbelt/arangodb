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
/// @author Esteban Lombeyda
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>

namespace arangodb {
class Completer;

////////////////////////////////////////////////////////////////////////////////
/// @brief ShellBase
////////////////////////////////////////////////////////////////////////////////

class ShellBase {
 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief state of the console
  //////////////////////////////////////////////////////////////////////////////

  enum console_state_e { STATE_NONE = 0, STATE_OPENED = 1, STATE_CLOSED = 2 };

 public:
  enum EofType { EOF_NONE = 0, EOF_ABORT = 1, EOF_FORCE_ABORT = 2 };

  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates a shell
  //////////////////////////////////////////////////////////////////////////////

  static ShellBase* buildShell(std::string const& history, Completer*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sort the alternatives results vector
  //////////////////////////////////////////////////////////////////////////////

  static void sortAlternatives(std::vector<std::string>&);

 public:
  ShellBase(std::string const& history, Completer*);

  virtual ~ShellBase();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief line editor prompt
  //////////////////////////////////////////////////////////////////////////////

  std::string prompt(std::string const& prompt, std::string const& begin,
                     EofType& eof);

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief handle a signal
  //////////////////////////////////////////////////////////////////////////////

  virtual void signal();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief line editor open
  //////////////////////////////////////////////////////////////////////////////

  virtual bool open(bool autoComplete) = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief line editor shutdown
  //////////////////////////////////////////////////////////////////////////////

  virtual bool close() = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief add to history
  //////////////////////////////////////////////////////////////////////////////

  virtual void addHistory(std::string const&) = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief save the history
  //////////////////////////////////////////////////////////////////////////////

  virtual bool writeHistory() = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get next line
  //////////////////////////////////////////////////////////////////////////////

  virtual std::string getLine(std::string const& prompt, EofType& eof) = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the shell implementation supports colors
  //////////////////////////////////////////////////////////////////////////////

  virtual bool supportsColors() const { return false; }

 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief current text
  //////////////////////////////////////////////////////////////////////////////

  std::string _current;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief history filename
  //////////////////////////////////////////////////////////////////////////////

  std::string _historyFilename;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief current console state
  //////////////////////////////////////////////////////////////////////////////

  console_state_e _state;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief object which defines when the input is finished
  //////////////////////////////////////////////////////////////////////////////

  Completer* _completer;
};
}  // namespace arangodb
