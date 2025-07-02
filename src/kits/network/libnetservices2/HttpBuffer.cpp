/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Niels Sascha Reedijk, niels.reedijk@gmail.com
 */

#include "HttpBuffer.h"

#include <algorithm> // For std::search
#include <DataIO.h>
#include <NetServicesDefs.h>
#include <String.h>

using namespace BPrivate::Network;


/*!
	\brief Newline sequence

	As per the RFC, defined as \r\n
*/
// static constexpr std::array<std::byte, 2> kNewLine = {std::byte('\r'), std::byte('\n')}; // C++17
static constexpr std::array<char, 2> kNewLine = {'\r', '\n'};


/*!
	\brief Create a new HTTP buffer with \a capacity.
*/
HttpBuffer::HttpBuffer(size_t capacity)
{
	fBuffer.reserve(capacity);
};


/*!
	\brief Load data from \a source into the spare capacity of this buffer.

	\exception BNetworkRequestError When BDataIO::Read() returns any error other than B_WOULD_BLOCK

	\retval B_WOULD_BLOCK The read call on the \a source was unsuccessful because it would block.
	\retval >=0 The actual number of bytes read.
*/
ssize_t
HttpBuffer::ReadFrom(BDataIO* source, size_t maxSize)
{
	// Remove any unused bytes at the beginning of the buffer
	Flush();

	auto currentSize = fBuffer.size();
	auto remainingBufferSize = fBuffer.capacity() - currentSize;

	if (maxSize != SIZE_MAX && maxSize < remainingBufferSize)
		remainingBufferSize = maxSize;

	// Adjust the buffer to the maximum size
	fBuffer.resize(fBuffer.capacity());

	ssize_t bytesRead = B_INTERRUPTED;
	while (bytesRead == B_INTERRUPTED)
		bytesRead = source->Read(fBuffer.data() + currentSize, remainingBufferSize);

	if (bytesRead == B_WOULD_BLOCK || bytesRead == 0) {
		fBuffer.resize(currentSize);
		return bytesRead;
	} else if (bytesRead < 0) {
		throw BNetworkRequestError(
			"BDataIO::Read()", BNetworkRequestError::NetworkError, bytesRead);
	}

	// Adjust the buffer to the current size
	fBuffer.resize(currentSize + bytesRead);

	return bytesRead;
}


/*!
	\brief Write the contents of the buffer through the helper \a func.

	\param func Handle the actual writing. The function accepts a pointer and a size as inputs
		and should return the number of actual written bytes, which may be fewer than the number
		of available bytes.

	\returns the actual number of bytes written to the \a func.
*/
size_t
HttpBuffer::WriteTo(HttpTransferFunction func, size_t maxSize)
{
	if (RemainingBytes() == 0)
		return 0;

	auto size = RemainingBytes();
	if (maxSize != SIZE_MAX && maxSize < size)
		size = maxSize;

	auto bytesWritten = func(fBuffer.data() + fCurrentOffset, size);
	if (bytesWritten > size)
		throw BRuntimeError(__PRETTY_FUNCTION__, "More bytes written than were made available");

	fCurrentOffset += bytesWritten;

	return bytesWritten;
}


/*!
	\brief Get the next line from this buffer.

	This can be called iteratively until all lines in the current data are read. After using this
	method, you should use Flush() to make sure that the read lines are cleared from the beginning
	of the buffer.

	\retval std::nullopt There are no more lines in the buffer.
	\retval BString The next line.
*/
BString
HttpBuffer::GetNextLine(bool& hasLine)
{
	hasLine = false;
	auto offset = fBuffer.cbegin() + fCurrentOffset;
	auto result = std::search(offset, fBuffer.cend(), kNewLine.cbegin(), kNewLine.cend());
	if (result == fBuffer.cend())
		return BString(); // No line found

	BString line(
		reinterpret_cast<const char*>(std::addressof(*offset)), std::distance(offset, result));
	fCurrentOffset = std::distance(fBuffer.cbegin(), result) + 2;
	hasLine = true;
	return line;
}


/*!
	\brief Get the number of remaining bytes in this buffer.
*/
size_t
HttpBuffer::RemainingBytes() const noexcept
{
	return fBuffer.size() - fCurrentOffset;
}


/*!
	\brief Move data to the beginning of the buffer to clear at the back.

	The GetNextLine() increases the offset of the internal buffer. This call moves remaining data
	to the beginning of the buffer sets the correct size, making the remainder of the capacity
	available for further reading.
*/
void
HttpBuffer::Flush() noexcept
{
	if (fCurrentOffset > 0) {
		auto end = fBuffer.cbegin() + fCurrentOffset;
		fBuffer.erase(fBuffer.cbegin(), end);
		fCurrentOffset = 0;
	}
}


/*!
	\brief Clear the internal buffer
*/
void
HttpBuffer::Clear() noexcept
{
	fBuffer.clear();
	fCurrentOffset = 0;
}


/*!
	\brief Get a view over the current data
*/
const unsigned char*
HttpBuffer::Data(size_t& length) const noexcept
{
	length = RemainingBytes();
	if (length > 0) {
		return fBuffer.data() + fCurrentOffset;
	} else {
		// Return nullptr or a valid pointer to satisfy API, length will be 0
		// For consistency, returning fBuffer.data() which might be nullptr if capacity is 0
		// or a valid pointer if capacity > 0 but size is 0.
		// The key is that length is 0.
		return fBuffer.data();
	}
}


/*!
	\brief Load data into the buffer

	\exception BNetworkRequestError in case of a buffer overflow
*/
HttpBuffer&
HttpBuffer::operator<<(const BString& data)
{
	if (data.Length() > (fBuffer.capacity() - fBuffer.size())) { // Changed .length() to .Length()
		throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError,
			"No capacity left in buffer to append data.");
	}

	for (int i = 0; i < data.Length(); ++i)
		fBuffer.push_back(static_cast<unsigned char>(data[i]));

	return *this;
}


HttpBuffer&
HttpBuffer::operator<<(const char* data)
{
	size_t len = strlen(data);
	if (len > (fBuffer.capacity() - fBuffer.size())) {
		throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError,
			"No capacity left in buffer to append data.");
	}

	for (size_t i = 0; i < len; ++i)
		fBuffer.push_back(static_cast<unsigned char>(data[i]));

	return *this;
}
