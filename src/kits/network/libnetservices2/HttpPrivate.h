/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _B_HTTP_PRIVATE_H_
#define _B_HTTP_PRIVATE_H_

// #include <string_view> // C++17, removed

#include <HttpRequest.h>
#include <Url.h>


namespace BPrivate {

namespace Network {

/*!
	\brief Validate whether the string conforms to a HTTP token value

	RFC 7230 section 3.2.6 determines that valid tokens for the header name are:
	!#$%&'*+=.^_`|~, any digits or alpha.

	\returns \c true if the string is valid, or \c false if it is not.
*/
static inline bool
// validate_http_token_string(const std::string_view& string) // C++17
validate_http_token_string(const BString& string)
{
	// for (auto it = string.cbegin(); it < string.cend(); it++) { // C++11 iterators for BString
	for (int32 i = 0; i < string.Length(); i++) {
		char ch = string[i];
		if (ch <= 31 || ch == 127 || ch == '(' || ch == ')' || ch == '<' || ch == '>'
			|| ch == '@' || ch == ',' || ch == ';' || ch == '\\' || ch == '"' || ch == '/'
			|| ch == '[' || ch == ']' || ch == '?' || ch == '=' || ch == '{' || ch == '}'
			|| ch == ' ') // Corrected from *it to ch
			return false;
	}
	return true;
}


} // namespace Network

} // namespace BPrivate

#endif // _B_HTTP_PRIVATE_H_
