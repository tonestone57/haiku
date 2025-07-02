/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _B_HTTP_BUFFER_H_
#define _B_HTTP_BUFFER_H_

#include <functional>
// #include <optional> // C++17, removed
// #include <string_view> // C++17, removed. Will use BString or const char* + length.
#include <vector>
#include <cstddef> // For SIZE_MAX

class BDataIO;
class BString;


namespace BPrivate {

namespace Network {

// using HttpTransferFunction = std::function<size_t(const std::byte*, size_t)>; // C++17
using HttpTransferFunction = std::function<size_t(const unsigned char*, size_t)>;


class HttpBuffer
{
public:
								HttpBuffer(size_t capacity = 8 * 1024);

			ssize_t				ReadFrom(BDataIO* source,
									size_t maxSize = SIZE_MAX);
			size_t				WriteTo(HttpTransferFunction func,
									size_t maxSize = SIZE_MAX);
			void				WriteExactlyTo(HttpTransferFunction func,
									size_t maxSize = SIZE_MAX);
			// std::optional<BString> GetNextLine(); // C++17
			BString GetNextLine(bool& hasLine); // Returns empty string if no line, hasLine will be false.

			size_t				RemainingBytes() const noexcept;

			void				Flush() noexcept;
			void				Clear() noexcept;

			// std::string_view	Data() const noexcept; // C++17
			const unsigned char* Data(size_t& length) const noexcept; // Returns pointer and length

	// load data into the buffer
			// HttpBuffer&			operator<<(const std::string_view& data); // C++17
			HttpBuffer&			operator<<(const BString& data);
			HttpBuffer&			operator<<(const char* data); // For string literals

private:
			// std::vector<std::byte> fBuffer; // C++17
			std::vector<unsigned char> fBuffer;
			size_t				fCurrentOffset = 0;
};


} // namespace Network

} // namespace BPrivate

#endif // _B_HTTP_BUFFER_H_
