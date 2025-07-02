/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Niels Sascha Reedijk, niels.reedijk@gmail.com
 */

#include "HttpParser.h"

#include <stdexcept>
#include <string>

#include <HttpFields.h>
#include <NetServicesDefs.h>
#include <ZlibCompressionAlgorithm.h>

// using namespace std::literals; // For sv suffix, removed
using namespace BPrivate::Network;


// #pragma mark -- HttpParser


/*!
	\brief Explicitly mark the response as having no content.

	This is done in cases where the request was a HEAD request. Setting it to no content, will
	instruct the parser to move to completion after all the header fields have been parsed.
*/
void
HttpParser::SetNoContent() noexcept
{
	if (fStreamState > HttpInputStreamState::Fields)
		debugger("Cannot set the parser to no content after parsing of the body has started");
	fBodyType = HttpBodyType::NoContent;
};


/*!
	\brief Parse the status from the \a buffer and store it in \a status.

	\retval true The status was succesfully parsed
	\retval false There is not enough data in the buffer for a full status.

	\exception BNetworkRequestException The status does not conform to the HTTP spec.
*/
bool
HttpParser::ParseStatus(HttpBuffer& buffer, BHttpStatus& status)
{
	if (fStreamState != HttpInputStreamState::StatusLine)
		debugger("The Status line has already been parsed");

	bool hasStatusLine;
	BString statusLineString = buffer.GetNextLine(hasStatusLine);
	if (!hasStatusLine)
		return false;

	auto codeStart = statusLineString.FindFirst(' ') + 1;
	if (codeStart < 0)
		throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError);

	auto codeEnd = statusLineString.FindFirst(' ', codeStart);

	if (codeEnd < 0 || (codeEnd - codeStart) != 3)
		throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError);

	std::string statusCodeString(statusLineString.String() + codeStart, 3);

	// build the output
	try {
		status.code = std::stol(statusCodeString);
	} catch (...) {
		throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError);
	}

	status.text = statusLineString; // statusLineString is already a BString
	fStatus.code = status.code; // cache the status code
	fStreamState = HttpInputStreamState::Fields;
	return true;
}


/*!
	\brief Parse the fields from the \a buffer and store it in \a fields.

	The fields are parsed incrementally, meaning that even if the full header is not yet in the
	\a buffer, it will still parse all complete fields and store them in the \a fields.

	After all fields have been parsed, it will determine the properties of the request body.
	This means it will determine whether there is any content compression, if there is a body,
	and if so if it has a fixed size or not.

	\retval true All fields were succesfully parsed
	\retval false There is not enough data in the buffer to complete parsing of fields.

	\exception BNetworkRequestException The fields not conform to the HTTP spec.
*/
bool
HttpParser::ParseFields(HttpBuffer& buffer, BHttpFields& fields)
{
	if (fStreamState != HttpInputStreamState::Fields)
		debugger("The parser is not expecting header fields at this point");

	bool hasFieldLine;
	BString fieldLineString = buffer.GetNextLine(hasFieldLine);

	while (hasFieldLine && !fieldLineString.IsEmpty()) {
		// Parse next header line
		fields.AddField(fieldLineString);
		fieldLineString = buffer.GetNextLine(hasFieldLine);
	}

	if (!hasFieldLine || (hasFieldLine && !fieldLineString.IsEmpty())) {
		// there is more to parse or buffer ended mid-header
		return false;
	}

	// Determine the properties for the body
	// RFC 7230 section 3.3.3 has a prioritized list of 7 rules around determining the body:
	// std::optional<off_t> bodyBytesTotal = std::nullopt; // C++17
	off_t bodyBytesTotalValue = -1;
	bool hasBodyBytesTotal = false;

	if (fBodyType == HttpBodyType::NoContent || fStatus.StatusCode() == BHttpStatusCode::NoContent
		|| fStatus.StatusCode() == BHttpStatusCode::NotModified) {
		// [1] In case of HEAD (set previously), status codes 1xx (TODO!), status code 204 or 304,
		// no content [2] NOT SUPPORTED: when doing a CONNECT request, no content
		fBodyType = HttpBodyType::NoContent;
		fStreamState = HttpInputStreamState::Done;
	} else {
		const BString transferEncodingFieldName("Transfer-Encoding");
		auto header = fields.FindField(transferEncodingFieldName);
		if (header != fields.end() && header->Value() == "chunked") {
		// [3] If there is a Transfer-Encoding heading set to 'chunked'
		// TODO: support the more advanced rules in the RFC around the meaning of this field
		fBodyType = HttpBodyType::Chunked;
		fStreamState = HttpInputStreamState::Body;
	} else if (fields.CountFields(BString("Content-Length")) > 0) {
		// [4] When there is no Transfer-Encoding, then look for Content-Encoding:
		//	- If there are more than one, the values must match
		//	- The value must be a valid number
		// [5] If there is a valid value, then that is the expected size of the body
		try {
			BString contentLengthBStr;
			const BString contentLengthFieldName("Content-Length");
			for (const auto& field: fields) {
				if (field.Name() == contentLengthFieldName) {
					if (contentLengthBStr.Length() == 0)
						contentLengthBStr = field.Value();
					else if (contentLengthBStr != field.Value()) {
						throw BNetworkRequestError(__PRETTY_FUNCTION__,
							BNetworkRequestError::ProtocolError,
							"Multiple Content-Length fields with differing values");
					}
				}
			}
			// bodyBytesTotal = std::stol(contentLength); // C++17
			if (contentLengthBStr.Length() > 0) {
				// Convert BString to std::string for std::stol
				std::string clStdString(contentLengthBStr.String());
				bodyBytesTotalValue = std::stol(clStdString);
				hasBodyBytesTotal = true;
			} else {
				// This case should ideally not be reached if CountFields("Content-Length") > 0
				// and the field has an empty value, which might be a protocol error.
				// Or, if no Content-Length field was actually found (e.g. due to case issues if FindField is case-sensitive).
				// For now, we'll treat it as Content-Length not definitively set.
				hasBodyBytesTotal = false;
			}

			if (hasBodyBytesTotal && bodyBytesTotalValue == 0) {
				fBodyType = HttpBodyType::NoContent;
				fStreamState = HttpInputStreamState::Done;
			} else if (hasBodyBytesTotal) { // Only FixedSize if Content-Length was valid and > 0
				fBodyType = HttpBodyType::FixedSize;
				fStreamState = HttpInputStreamState::Body;
			} else {
				// If Content-Length was present but invalid/empty, or multiple different values.
				// RFC 7230 Section 3.3.3 Rule 5 indicates if Content-Length is invalid, it should be rejected or recovered.
				// Here, we might fall through to chunked/variable size or error out.
				// For now, let's assume if it's not a valid fixed size, it's variable (or an error later).
				// This part of the logic might need refinement based on strict RFC adherence.
				// The original code would throw if stol failed.
				// If hasBodyBytesTotal is false here, it means std::stol would have failed.
				// So we should re-throw or handle as per original logic.
				if (fields.CountFields("Content-Length") > 0 && !hasBodyBytesTotal) {
					// This means Content-Length was found, but was not parsable by previous logic.
					throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError,
						"Cannot parse Content-Length field value");
				}
				// If no valid Content-Length, proceed to rule 7 (variable size / close delimited)
				fBodyType = HttpBodyType::VariableSize;
				fStreamState = HttpInputStreamState::Body;
			}
		} catch (const std::logic_error& e) {
			// This catch is for std::stol failing (invalid_argument, out_of_range)
			throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError,
				"Cannot parse Content-Length field value (logic_error)");
		}
	} else {
		// [6] Applies to request messages only (this is a response)
		// [7] If nothing else then the received message is all data until connection close
		// (this is the default)
		fStreamState = HttpInputStreamState::Body;
		fBodyType = HttpBodyType::VariableSize; // Explicitly set for clarity
	}

	// Set up the body parser based on the logic above.
	switch (fBodyType) {
		case HttpBodyType::VariableSize:
			fBodyParser = std::make_unique<HttpRawBodyParser>();
			break;
		case HttpBodyType::FixedSize:
			if (hasBodyBytesTotal)
				fBodyParser = std::make_unique<HttpRawBodyParser>(bodyBytesTotalValue, true);
			else {
				// This should not be reached if fBodyType is FixedSize, implies logic error above.
				// For safety, create a variable size parser or throw.
				// Original code might have thrown before this.
				throw BRuntimeError(__PRETTY_FUNCTION__, "FixedSize body type without a valid size.");
				// fBodyParser = std::make_unique<HttpRawBodyParser>(); // Fallback, but indicates issue
			}
			break;
		case HttpBodyType::Chunked:
			fBodyParser = std::make_unique<HttpChunkedBodyParser>();
			break;
		case HttpBodyType::NoContent:
		default:
			// For NoContent, fBodyParser remains nullptr, and we should return true.
			return true;
	}

	// Check Content-Encoding for compression
	const BString contentEncodingFieldName("Content-Encoding");
	auto compressionHeader = fields.FindField(contentEncodingFieldName);
	if (compressionHeader != fields.end() && (compressionHeader->Value() == "gzip" || compressionHeader->Value() == "deflate")) {
		if (!fBodyParser) {
			// This can happen if fBodyType was NoContent but Content-Encoding is present.
			// This is unusual, but technically the body parser should wrap a "no-op" parser.
			// For simplicity, if there's no content, compression is irrelevant.
			// However, if there IS a body parser, we wrap it.
		} else {
			fBodyParser = std::make_unique<HttpBodyDecompression>(std::move(fBodyParser));
		}
	}
	return true;
}


/*!
	\brief Parse the body from the \a buffer and use \a writeToBody function to save.

	The \a readEnd parameter indicates to the parser that the buffer currently contains all the
	expected data for this request.
*/
size_t
HttpParser::ParseBody(HttpBuffer& buffer, HttpTransferFunction writeToBody, bool readEnd)
{
	if (fStreamState < HttpInputStreamState::Body || fStreamState == HttpInputStreamState::Done)
		debugger("The parser is not in the correct state to parse a body");

	auto parseResult = fBodyParser->ParseBody(buffer, writeToBody, readEnd);

	if (parseResult.complete)
		fStreamState = HttpInputStreamState::Done;

	return parseResult.bytesParsed;
}


/*!
	\brief Return if the body is currently expecting to having content.

	This may change if the header fields have not yet been parsed, as these may contain
	instructions about the body having no content.
*/
bool
HttpParser::HasContent() const noexcept
{
	return fBodyType != HttpBodyType::NoContent;
}


/*!
	\brief Return the total size of the body, if known.
*/
off_t
HttpParser::BodyBytesTotal(bool& hasTotal) const noexcept
{
	hasTotal = false;
	if (fBodyParser) {
		return fBodyParser->TotalBodySize(hasTotal);
	}
	return -1; // Or some other indicator of not-known when no parser
}


/*!
	\brief Return the number of body bytes transferred from the response.
*/
off_t
HttpParser::BodyBytesTransferred() const noexcept
{
	if (fBodyParser)
		return fBodyParser->TransferredBodySize();
	return 0;
}


/*!
	\brief Check if the body is fully parsed.
*/
bool
HttpParser::Complete() const noexcept
{
	return fStreamState == HttpInputStreamState::Done;
}


// #pragma mark -- HttpBodyParser


/*!
	\brief Default implementation to return no total.
*/
off_t
HttpBodyParser::TotalBodySize(bool& hasTotal) const noexcept
{
	hasTotal = false;
	return -1;
}


/*!
	\brief Return the number of body bytes read from the stream so far.

	For chunked transfers, this excludes the chunk headers and other metadata.
*/
off_t
HttpBodyParser::TransferredBodySize() const noexcept
{
	return fTransferredBodySize;
}


// #pragma mark -- HttpRawBodyParser
/*!
	\brief Construct a HttpRawBodyParser with an unknown content size.
*/
HttpRawBodyParser::HttpRawBodyParser()
	:
	fBodyBytesTotalValue(-1),
	fHasBodyBytesTotal(false)
{
}


/*!
	\brief Construct a HttpRawBodyParser with expected \a bodyBytesTotal size.
*/
HttpRawBodyParser::HttpRawBodyParser(off_t bodyBytesTotal, bool hasTotal)
	:
	fBodyBytesTotalValue(bodyBytesTotal),
	fHasBodyBytesTotal(hasTotal)
{
}


/*!
	\brief Parse a regular (non-chunked) body from a buffer.

	The buffer is parsed into a target using the \a writeToBody function.

	The \a readEnd argument indicates whether the current \a buffer contains all the expected data.
	In case the total body size is known, and the remaining bytes in the buffer are smaller than
	the expected remainder, a ProtocolError will be raised. The data in the buffer will *not* be
	copied to the target.

	Also, if the body size is known, and the data in the \a buffer is larger than the expected
	expected length, then it will only read the bytes needed and leave the remainder in the buffer.

	It is required that the \a writeToBody function writes all the bytes it is asked to; this
	method does not support partial writes and throws an exception when it fails.

	\exception BNetworkRequestError In case the buffer contains too little or invalid data.

	\returns The number of bytes parsed from the \a buffer.
*/
BodyParseResult
HttpRawBodyParser::ParseBody(HttpBuffer& buffer, HttpTransferFunction writeToBody, bool readEnd)
{
	auto bytesToRead = buffer.RemainingBytes();
	if (fHasBodyBytesTotal) {
		auto expectedRemainingBytes = fBodyBytesTotalValue - fTransferredBodySize;
		if (expectedRemainingBytes < 0) expectedRemainingBytes = 0; // Already read enough or too much

		if (static_cast<off_t>(bytesToRead) > expectedRemainingBytes)
			bytesToRead = expectedRemainingBytes;
		else if (readEnd && expectedRemainingBytes > static_cast<off_t>(buffer.RemainingBytes())) {
			// This condition means: we expected more bytes (expectedRemainingBytes > current buffer content)
			// AND this is the end of the input stream. So, data is incomplete.
			throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError,
				"Message body is incomplete; less data received than expected");
		}
	}

	// Copy the data
	auto bytesRead = buffer.WriteTo(writeToBody, bytesToRead);
	fTransferredBodySize += bytesRead;

	if (bytesRead != bytesToRead) {
		// Fail if not all expected bytes are written.
		throw BNetworkRequestError(__PRETTY_FUNCTION__, BNetworkRequestError::SystemError,
			"Could not write all available body bytes to the target.");
	}

	if (fHasBodyBytesTotal) {
		if (fBodyBytesTotalValue == fTransferredBodySize)
			return {bytesRead, bytesRead, true};
		else
			return {bytesRead, bytesRead, false};
	} else
		return {bytesRead, bytesRead, readEnd};
}


/*!
	\brief Override default implementation and return known body size (or indicate if not known)
*/
off_t
HttpRawBodyParser::TotalBodySize(bool& hasTotal) const noexcept
{
	hasTotal = fHasBodyBytesTotal;
	if (fHasBodyBytesTotal)
		return fBodyBytesTotalValue;
	return -1;
}


// #pragma mark -- HttpChunkedBodyParser
/*!
	\brief Parse a chunked body from a buffer.

	The contents of the cunks are copied into a target using the \a writeToBody function.

	The \a readEnd argument indicates whether the current \a buffer contains all the expected data.
	In case the chunk argument indicates that more data was to come, an exception is thrown.

	It is required that the \a writeToBody function writes all the bytes it is asked to; this
	method does not support partial writes and throws an exception when it fails.

	\exception BNetworkRequestError In case there is an error parsing the buffer, or there is too
		little data.

	\returns The number of bytes parsed from the \a buffer.
*/
BodyParseResult
HttpChunkedBodyParser::ParseBody(HttpBuffer& buffer, HttpTransferFunction writeToBody, bool readEnd)
{
	size_t totalBytesRead = 0;
	while (buffer.RemainingBytes() > 0) {
		switch (fChunkParserState) {
			case ChunkSize:
			{
				// Read the next chunk size from the buffer; if unsuccesful wait for more data
				bool hasChunkSizeLine;
				BString chunkSizeBString = buffer.GetNextLine(hasChunkSizeLine);
				if (!hasChunkSizeLine)
					return {totalBytesRead, totalBytesRead, false}; // Pass through existing bytesWritten if any

				// auto chunkSizeStr = std::string(chunkSizeString.value().String()); // C++17
				std::string chunkSizeStdString(chunkSizeBString.String());
				try {
					size_t pos = 0;
					fRemainingChunkSize = std::stoll(chunkSizeStdString, &pos, 16);
					if (pos < chunkSizeStdString.size() && chunkSizeStdString[pos] != ';') {
						throw BNetworkRequestError(
							__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError, "Invalid char after chunk size");
					}
				} catch (const std::invalid_argument& e) {
					throw BNetworkRequestError(
						__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError, "Invalid chunk size format (invalid_argument)");
				} catch (const std::out_of_range& e) {
					throw BNetworkRequestError(
						__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError, "Chunk size out of range");
				}

				if (fRemainingChunkSize > 0)
					fChunkParserState = Chunk;
				else { // Zero chunk size means this is the last chunk
					fLastChunk = true; // Mark that we've seen the 0-size chunk
					fChunkParserState = Trailers; // Proceed to trailers
				}
				break;
			}

			case Chunk:
			{
				size_t bytesToRead;
				if (fRemainingChunkSize > static_cast<off_t>(buffer.RemainingBytes()))
					bytesToRead = buffer.RemainingBytes();
				else
					bytesToRead = fRemainingChunkSize;

				auto bytesRead = buffer.WriteTo(writeToBody, bytesToRead);
				// The writeToBody function itself should handle errors by throwing.
				// Here we just assert that it wrote what we asked, or less if buffer was smaller (already handled by bytesToRead calc).
				// The HttpRawBodyParser's WriteTo might throw if the func doesn't write all.

				fTransferredBodySize += bytesRead;
				totalBytesRead += bytesRead; // totalBytesRead is for this ParseBody call only
				fRemainingChunkSize -= bytesRead;
				if (fRemainingChunkSize == 0)
					fChunkParserState = ChunkEnd; // Expect CRLF after chunk data
				break;
			}

			case ChunkEnd: // After chunk data, expect CRLF
			{
				if (buffer.RemainingBytes() < 2) { // Need at least CRLF
					return {totalBytesRead, totalBytesRead, false};
				}
				bool hasChunkEndLine;
				BString chunkEndString = buffer.GetNextLine(hasChunkEndLine);
				if (!hasChunkEndLine) { // Should find CRLF
					// Not enough data for CRLF, or malformed
					return {totalBytesRead, totalBytesRead, false};
				}
				if (!chunkEndString.IsEmpty()) { // Expecting an empty line (just CRLF)
					throw BNetworkRequestError(
						__PRETTY_FUNCTION__, BNetworkRequestError::ProtocolError, "Chunk data not followed by CRLF");
				}
				// Successfully read CRLF
				if (fLastChunk) // If this CRLF was after the 0-size chunk
					fChunkParserState = Trailers;
				else
					fChunkParserState = ChunkSize; // Go look for next chunk size
				break;
			}

			case Trailers:
			{
				bool hasTrailerLine;
				BString trailerString = buffer.GetNextLine(hasTrailerLine);
				if (!hasTrailerLine) {
					return {totalBytesRead, totalBytesRead, false};
				}

				if (trailerString.IsEmpty()) { // Empty line signifies end of trailers
					fChunkParserState = Complete;
					return {totalBytesRead, totalBytesRead, true};
				}
				// TODO: Process trailerString if needed (RFC 7230 allows them)
				// For now, we just consume them until an empty line.
				break;
			}

			case Complete:
				return {totalBytesRead, totalBytesRead, true}; // Already complete
		}
	}
	return {totalBytesRead, totalBytesRead, false};
}


// #pragma mark -- HttpBodyDecompression
/*!
	\brief Set up a decompression stream that decompresses the data read by \a bodyParser.
*/
HttpBodyDecompression::HttpBodyDecompression(std::unique_ptr<HttpBodyParser> bodyParser)
{
	fDecompressorStorage = std::make_unique<BMallocIO>();

	BDataIO* stream = nullptr;
	auto result = BZlibCompressionAlgorithm().CreateDecompressingOutputStream(
		fDecompressorStorage.get(), nullptr, stream);

	if (result != B_OK) {
		throw BNetworkRequestError("BZlibCompressionAlgorithm().CreateCompressingOutputStream",
			BNetworkRequestError::SystemError, result);
	}

	fDecompressingStream = std::unique_ptr<BDataIO>(stream);
	fBodyParser = std::move(bodyParser);
}


/*!
	\brief Read a compressed body into a target..

	The stream captures chunked or raw data, and decompresses it. The decompressed data is then
	copied into a target using the \a writeToBody function.

	The \a readEnd argument indicates whether the current \a buffer contains all the expected data.
	It is up for the underlying parser to determine if more data was expected, and therefore, if
	there is an error.

	It is required that the \a writeToBody function writes all the bytes it is asked to; this
	method does not support partial writes and throws an exception when it fails.

	\exception BNetworkRequestError In case there is an error parsing the buffer, or there is too
		little data.

	\returns The number of bytes parsed from the \a buffer.
*/
BodyParseResult
HttpBodyDecompression::ParseBody(HttpBuffer& buffer, HttpTransferFunction writeToBody, bool readEnd)
{
	// Get the underlying raw or chunked parser to write data to our decompressionstream
	auto parseResults = fBodyParser->ParseBody(
		buffer,
		// [this](const std::byte* buffer, size_t bufferSize) { // C++17
		[this](const unsigned char* buffer, size_t bufferSize) {
			auto status = fDecompressingStream->WriteExactly(buffer, bufferSize);
			if (status != B_OK) {
				throw BNetworkRequestError(
					"BDataIO::WriteExactly()", BNetworkRequestError::SystemError, status);
			}
			return bufferSize;
		},
		readEnd);
	fTransferredBodySize += parseResults.bytesParsed;

	if (readEnd || parseResults.complete) {
		// No more bytes expected so flush out the final bytes
		if (auto status = fDecompressingStream->Flush(); status != B_OK) {
			throw BNetworkRequestError(
				"BZlibDecompressionStream::Flush()", BNetworkRequestError::SystemError, status);
		}
	}

	size_t bytesWritten = 0;
	// if (auto bodySize = fDecompressorStorage->Position(); bodySize > 0) { // C++17 if-initializer
	auto bodySize = fDecompressorStorage->Position();
	if (bodySize > 0) {
		bytesWritten
			// = writeToBody(static_cast<const std::byte*>(fDecompressorStorage->Buffer()), bodySize); // C++17
			= writeToBody(static_cast<const unsigned char*>(fDecompressorStorage->Buffer()), bodySize);
		if (static_cast<off_t>(bytesWritten) != bodySize) {
			throw BNetworkRequestError(
				__PRETTY_FUNCTION__, BNetworkRequestError::SystemError, B_PARTIAL_WRITE);
		}
		fDecompressorStorage->Seek(0, SEEK_SET);
	}
	return {parseResults.bytesParsed, bytesWritten, parseResults.complete};
}


/*!
	\brief Return the TotalBodySize() from the underlying chunked or raw parser.
*/
off_t
HttpBodyDecompression::TotalBodySize(bool& hasTotal) const noexcept
{
	// The underlying parser (fBodyParser) might know the total size
	// (e.g. if it's a HttpRawBodyParser for a non-chunked, non-compressed stream
	// with Content-Length). Decompression itself (like gzip) usually doesn't
	// know the uncompressed size beforehand unless it's in a trailer, which
	// our current chunked parser doesn't fully process for size.
	if (fBodyParser)
		return fBodyParser->TotalBodySize(hasTotal);

	hasTotal = false;
	return -1;
}
