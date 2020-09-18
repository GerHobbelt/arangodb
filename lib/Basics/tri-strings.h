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
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BASICS_TRI__STRINGS_H
#define ARANGODB_BASICS_TRI__STRINGS_H 1

#include <cstdint>
#include <cstdlib>

#include "Basics/Common.h"

////////////////////////////////////////////////////////////////////////////////
/// @brief convert an ASCII string to lower case
///
/// Note: this works with ASCII characters only. No umlauts, no UTF-8 characters
/// tolower and toupper of ctype.h are not used because they depend on locale
////////////////////////////////////////////////////////////////////////////////

char* TRI_LowerAsciiString(char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief convert an ASCII string to upper case
///
/// Note: this works with ASCII characters only. No umlauts, no UTF-8 characters
/// tolower and toupper of ctype.h are not used because they depend on locale
////////////////////////////////////////////////////////////////////////////////

char* TRI_UpperAsciiString(char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief tests if ASCII strings are equal
////////////////////////////////////////////////////////////////////////////////

bool TRI_EqualString(char const* left, char const* right);

////////////////////////////////////////////////////////////////////////////////
/// @brief tests if ASCII strings are equal
////////////////////////////////////////////////////////////////////////////////

bool TRI_EqualString(char const* left, char const* right, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief tests if ASCII strings are equal ignoring case
////////////////////////////////////////////////////////////////////////////////

bool TRI_CaseEqualString(char const* left, char const* right);

////////////////////////////////////////////////////////////////////////////////
/// @brief tests if ASCII strings are equal ignoring case
////////////////////////////////////////////////////////////////////////////////

bool TRI_CaseEqualString(char const* left, char const* right, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief tests if second string is prefix of the first
////////////////////////////////////////////////////////////////////////////////

bool TRI_IsPrefixString(char const* full, char const* prefix);

////////////////////////////////////////////////////////////////////////////////
/// @brief tests if second string is contained in the first, byte-safe
////////////////////////////////////////////////////////////////////////////////

char* TRI_IsContainedMemory(char const* full, size_t fullLength,
                            char const* part, size_t partLength);

////////////////////////////////////////////////////////////////////////////////
/// @brief duplicates a string
////////////////////////////////////////////////////////////////////////////////

char* TRI_DuplicateString(char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief duplicates a string of given length
////////////////////////////////////////////////////////////////////////////////

char* TRI_DuplicateString(char const*, size_t length);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies a string
///
/// Copy string of maximal length. Always append a '\0'.
////////////////////////////////////////////////////////////////////////////////

void TRI_CopyString(char* dst, char const* src, size_t length);

////////////////////////////////////////////////////////////////////////////////
/// @brief concatenate three strings using a memory zone
////////////////////////////////////////////////////////////////////////////////

char* TRI_Concatenate3String(char const*, char const*, char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief frees a string
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeString(char*);

////////////////////////////////////////////////////////////////////////////////
/// @brief sha256 of a string
////////////////////////////////////////////////////////////////////////////////

char* TRI_SHA256String(char const* source, size_t sourceLen, size_t* dstLen);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the maximum result length for an escaped string
/// (4 * inLength) + 2 bytes!
////////////////////////////////////////////////////////////////////////////////

constexpr size_t TRI_MaxLengthEscapeControlsCString(size_t inLength) {
  return (4 * inLength) + 2;  // for newline and 0 byte
}

////////////////////////////////////////////////////////////////////////////////
/// @brief escapes special characters using C escapes
/// the target buffer must have been allocated already and big enough to hold
/// the result of at most (4 * inLength) + 2 bytes!
////////////////////////////////////////////////////////////////////////////////

char* TRI_EscapeControlsCString(char const* in, size_t inLength, char* out,
                                size_t* outLength, bool appendNewline);

////////////////////////////////////////////////////////////////////////////////
/// @brief escapes special characters using unicode escapes
///
/// This method escapes an UTF-8 character string by replacing the unprintable
/// characters by a \\uXXXX sequence. Set escapeSlash to true in order to also
/// escape the character '/'.
////////////////////////////////////////////////////////////////////////////////

char* TRI_EscapeUtf8String(char const* in, size_t inLength, bool escapeSlash,
                           size_t* outLength, bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief unescapes unicode escape sequences
///
/// This method decodes a UTF-8 character string by replacing the \\uXXXX
/// sequence by unicode characters and representing them as UTF-8 sequences.
////////////////////////////////////////////////////////////////////////////////

char* TRI_UnescapeUtf8String(char const* in, size_t inLength, size_t* outLength, bool normalize);

////////////////////////////////////////////////////////////////////////////////
/// @brief unescapes unicode escape sequences into buffer "buffer".
/// the buffer must be big enough to hold at least inLength + 1 bytes of chars
/// returns the length of the unescaped string, excluding the trailing null
/// byte
////////////////////////////////////////////////////////////////////////////////

size_t TRI_UnescapeUtf8StringInPlace(char* buffer, char const* in, size_t inLength);

////////////////////////////////////////////////////////////////////////////////
/// @brief determine the number of characters in a UTF-8 string
////////////////////////////////////////////////////////////////////////////////

size_t TRI_CharLengthUtf8String(char const*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief get the string end position for a leftmost prefix of a UTF-8 string
/// eg. when specifying (müller, 2), the return value will be a pointer to the
/// first "l".
/// the UTF-8 string must be well-formed and end with a NUL terminator
////////////////////////////////////////////////////////////////////////////////

char* TRI_PrefixUtf8String(char const*, const uint32_t);

#endif
