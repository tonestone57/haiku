/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _B_HTTP_SERIALIZER_H_
#define _B_HTTP_SERIALIZER_H_


#include <functional>
// #include <optional> // C++17, replaced

class BDataIO;

namespace BPrivate {

namespace Network {

class BHttpRequest;
class HttpBuffer;

// using HttpTransferFunction = std::function<size_t(const std::byte*, size_t)>; // C++17
using HttpTransferFunction = std::function<size_t(const unsigned char*, size_t)>;


enum class HttpSerializerState { Uninitialized, Header, ChunkHeader, Body, Done };


class HttpSerializer
{
public:
								HttpSerializer(){};

			void				SetTo(HttpBuffer& buffer, const BHttpRequest& request);
			bool				IsInitialized() const noexcept;

			size_t				Serialize(HttpBuffer& buffer, BDataIO* target);

			// std::optional<off_t> BodyBytesTotal() const noexcept; // C++17
			off_t				BodyBytesTotal(bool& hasTotal) const noexcept;
			off_t				BodyBytesTransferred() const noexcept;
			bool				Complete() const noexcept;

private:
			bool				_IsChunked() const noexcept;
			size_t				_WriteToTarget(HttpBuffer& buffer, BDataIO* target) const;

private:
			HttpSerializerState	fState = HttpSerializerState::Uninitialized;
			BDataIO*			fBody = nullptr;
			off_t				fTransferredBodySize = 0;
			// std::optional<off_t> fBodySize; // C++17
			off_t				fBodySizeValue;
			bool				fHasBodySize;
};


inline bool
HttpSerializer::IsInitialized() const noexcept
{
	return fState != HttpSerializerState::Uninitialized;
}


inline off_t
HttpSerializer::BodyBytesTotal(bool& hasTotal) const noexcept
{
	hasTotal = fHasBodySize;
	if (fHasBodySize)
		return fBodySizeValue;
	return -1; // Or some other indicator for "not known"
}


inline off_t
HttpSerializer::BodyBytesTransferred() const noexcept
{
	return fTransferredBodySize;
}


inline bool
HttpSerializer::Complete() const noexcept
{
	return fState == HttpSerializerState::Done;
}


} // namespace Network

} // namespace BPrivate

#endif // _B_HTTP_SERIALIZER_H_
