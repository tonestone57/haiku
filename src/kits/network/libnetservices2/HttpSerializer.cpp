/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Niels Sascha Reedijk, niels.reedijk@gmail.com
 */

#include "HttpSerializer.h"

#include <DataIO.h>
#include <HttpRequest.h>
#include <NetServicesDefs.h>

#include "HttpBuffer.h"

// using namespace std::literals; // C++17, for ""sv suffix, no longer needed
using namespace BPrivate::Network;


/*!
	\brief Set the \a request to serialize, and load the initial data into the \a buffer.
*/
void
HttpSerializer::SetTo(HttpBuffer& buffer, const BHttpRequest& request)
{
	buffer.Clear();
	request.SerializeHeaderTo(buffer);
	fState = HttpSerializerState::Header;
	fHasBodySize = false; // Initialize

	if (auto requestBody = request.RequestBody()) {
		fBody = requestBody->input.get();
		if (requestBody->hasSize) {
			fBodySizeValue = requestBody->sizeValue;
			fHasBodySize = true;
		}
		// If !requestBody->hasSize, fHasBodySize remains false, meaning chunked or close-delimited.
	}
	// If no requestBody, fBody remains nullptr, fHasBodySize remains false.
}


/*!
	\brief Transfer the HTTP request to \a target while using \a buffer for intermediate storage.

	\returns The number of body bytes written during the call.
*/
size_t
HttpSerializer::Serialize(HttpBuffer& buffer, BDataIO* target)
{
	bool finishing = false;
	size_t bodyBytesWritten = 0;
	while (!finishing) {
		switch (fState) {
			case HttpSerializerState::Uninitialized:
				throw BRuntimeError(__PRETTY_FUNCTION__, "Invalid state: Uninitialized");

			case HttpSerializerState::Header:
				_WriteToTarget(buffer, target);
				if (buffer.RemainingBytes() > 0) {
					// There are more bytes to be processed; wait for the next iteration
					return 0;
				}

				if (fBody == nullptr) {
					fState = HttpSerializerState::Done;
					return 0;
				} else if (_IsChunked())
					// fState = HttpSerializerState::ChunkHeader;
					throw BRuntimeError(
						__PRETTY_FUNCTION__, "Chunked serialization not implemented");
				else
					fState = HttpSerializerState::Body;
				break;

			case HttpSerializerState::Body:
			{
				auto bytesWritten = _WriteToTarget(buffer, target);
				bodyBytesWritten += bytesWritten;
				fTransferredBodySize += bytesWritten;
				if (buffer.RemainingBytes() > 0) {
					// did not manage to write all the bytes in the buffer; continue in the next
					// round
					finishing = true;
					break;
				}

				if (fHasBodySize && fBodySizeValue == fTransferredBodySize) {
					fState = HttpSerializerState::Done;
					finishing = true;
				}
				break;
			}

			case HttpSerializerState::Done:
			default:
				finishing = true;
				continue;
		}

		// Load more data into the buffer
		size_t readSizeHint = SIZE_MAX; // from <cstddef>
		if (fHasBodySize) {
			if (fBodySizeValue > fTransferredBodySize) {
				readSizeHint = fBodySizeValue - fTransferredBodySize;
			} else {
				readSizeHint = 0;
			}
		}

		if (fState != HttpSerializerState::Done) {
			if (fHasBodySize && readSizeHint == 0) {
				// If body size is known and we've transferred it all, mark as done.
				fState = HttpSerializerState::Done;
			} else {
				buffer.ReadFrom(fBody, readSizeHint);
			}
		}
	}

	return bodyBytesWritten;
}


bool
HttpSerializer::_IsChunked() const noexcept
{
	return !fHasBodySize;
}


size_t
HttpSerializer::_WriteToTarget(HttpBuffer& buffer, BDataIO* target) const
{
	size_t bytesWritten = 0;
	buffer.WriteTo([target, &bytesWritten](const unsigned char* buffer, size_t size) { // Replaced std::byte
		ssize_t result = B_INTERRUPTED;
		while (result == B_INTERRUPTED) {
			result = target->Write(buffer, size);
		}

		if (result <= 0 && result != B_WOULD_BLOCK) {
			throw BNetworkRequestError(
				__PRETTY_FUNCTION__, BNetworkRequestError::NetworkError, result);
		} else if (result > 0) {
			bytesWritten += result;
			return size_t(result);
		} else {
			return size_t(0);
		}
	});

	return bytesWritten;
}
