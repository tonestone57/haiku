/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _B_HTTP_PARSER_H_
#define _B_HTTP_PARSER_H_


#include <functional>
// #include <optional> // C++17, removed

#include <HttpResult.h>

#include "HttpBuffer.h"

class BMallocIO;

namespace BPrivate {

namespace Network {

// using HttpTransferFunction = std::function<size_t(const std::byte*, size_t)>; // C++17
using HttpTransferFunction = std::function<size_t(const unsigned char*, size_t)>;


enum class HttpInputStreamState { StatusLine, Fields, Body, Done };


enum class HttpBodyType { NoContent, Chunked, FixedSize, VariableSize };


struct BodyParseResult {
			size_t		bytesParsed;
			size_t		bytesWritten;
			bool		complete;
};


class HttpBodyParser;


class HttpParser
{
public:
								HttpParser(){};

	// Explicitly mark request as having no content
			void				SetNoContent() noexcept;

	// Parse data from response
			bool				ParseStatus(HttpBuffer& buffer, BHttpStatus& status);
			bool				ParseFields(HttpBuffer& buffer, BHttpFields& fields);
			size_t				ParseBody(HttpBuffer& buffer, HttpTransferFunction writeToBody,
									bool readEnd);
			HttpInputStreamState State() const noexcept { return fStreamState; }

	// Details on the body status
			bool				HasContent() const noexcept;
			// std::optional<off_t> BodyBytesTotal() const noexcept; // C++17
			off_t				BodyBytesTotal(bool& hasTotal) const noexcept; // Returns -1 if no total, hasTotal will be false.
			off_t				BodyBytesTransferred() const noexcept;
			bool				Complete() const noexcept;

private:
			off_t				fHeaderBytes = 0;
			BHttpStatus			fStatus;
			HttpInputStreamState fStreamState = HttpInputStreamState::StatusLine;

	// Body
			HttpBodyType		fBodyType = HttpBodyType::VariableSize;
			std::unique_ptr<HttpBodyParser> fBodyParser = nullptr;
};


class HttpBodyParser
{
public:
	virtual						BodyParseResult ParseBody(HttpBuffer& buffer,
									HttpTransferFunction writeToBody, bool readEnd) = 0;

	// virtual	std::optional<off_t> TotalBodySize() const noexcept; // C++17
	virtual	off_t				TotalBodySize(bool& hasTotal) const noexcept; // Returns -1 if no total, hasTotal will be false.

			off_t				TransferredBodySize() const noexcept;

protected:
			off_t				fTransferredBodySize = 0;
};


class HttpRawBodyParser : public HttpBodyParser
{
public:
								HttpRawBodyParser();
								HttpRawBodyParser(off_t bodyBytesTotal, bool hasTotal);
	virtual	BodyParseResult		ParseBody(HttpBuffer& buffer, HttpTransferFunction writeToBody,
									bool readEnd) override;
	// virtual	std::optional<off_t> TotalBodySize() const noexcept override; // C++17
	virtual	off_t				TotalBodySize(bool& hasTotal) const noexcept override; // Returns -1 if no total, hasTotal will be false.

private:
			// std::optional<off_t> fBodyBytesTotal; // C++17
			off_t fBodyBytesTotalValue;
			bool fHasBodyBytesTotal;
};


class HttpChunkedBodyParser : public HttpBodyParser
{
public:
	virtual BodyParseResult ParseBody(
		HttpBuffer& buffer, HttpTransferFunction writeToBody, bool readEnd) override;

private:
	enum { ChunkSize, ChunkEnd, Chunk, Trailers, Complete } fChunkParserState = ChunkSize;
	off_t fRemainingChunkSize = 0;
	bool fLastChunk = false;
};


class HttpBodyDecompression : public HttpBodyParser
{
public:
								HttpBodyDecompression(std::unique_ptr<HttpBodyParser> bodyParser);
	virtual	BodyParseResult		ParseBody(HttpBuffer& buffer, HttpTransferFunction writeToBody,
									bool readEnd) override;

	// virtual	std::optional<off_t> TotalBodySize() const noexcept; // C++17
	virtual	off_t				TotalBodySize(bool& hasTotal) const noexcept; // Returns -1 if no total, hasTotal will be false.

private:
			std::unique_ptr<HttpBodyParser> fBodyParser;
			std::unique_ptr<BMallocIO> fDecompressorStorage;
			std::unique_ptr<BDataIO> fDecompressingStream;
};


} // namespace Network

} // namespace BPrivate

#endif // _B_HTTP_PARSER_H_
