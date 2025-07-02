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
// Re-evaluating HttpParser.cpp for subtle syntax issues.


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
}


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

bool HttpParser::ParseFields(HttpBuffer& buffer, BHttpFields& fields)
{
    if (fStreamState != HttpInputStreamState::Fields) {
        debugger("The parser is not expecting header fields at this point");
    }

    // 1. Read all header lines
    bool hasLine = false;
    BString line = buffer.GetNextLine(hasLine);
    while (hasLine && !line.IsEmpty()) {
        fields.AddField(line);
        line = buffer.GetNextLine(hasLine);
    }

    // If buffer ended mid-header, wait for more data
    if (!hasLine || !line.IsEmpty()) {
        return false;
    }

    // 2. Prepare for body detection
    off_t       totalBytes      = -1;
    bool        haveByteCount   = false;

    // 3. Rule [1] & [2]: No-content responses
    if (fBodyType == HttpBodyType::NoContent
        || fStatus.StatusCode() == BHttpStatusCode::NoContent
        || fStatus.StatusCode() == BHttpStatusCode::NotModified)
    {
        fBodyType    = HttpBodyType::NoContent;
        fStreamState = HttpInputStreamState::Done;
    }
    else {
        // 4. Rule [3]: Transfer-Encoding: chunked
        const BString transferKey("Transfer-Encoding");
        auto headerIt = fields.FindField(transferKey);
        if (headerIt != fields.end()
            && headerIt->Value() == "chunked")
        {
            fBodyType    = HttpBodyType::Chunked;
            fStreamState = HttpInputStreamState::Body;
        }
        // 5. Rule [4] & [5]: Content-Length
        else if (fields.CountFields(BString("Content-Length")) > 0)
        {
            BString combinedValue;
            const BString      contentKey("Content-Length");

            for (auto& fld : fields) {
                if (fld.Name() == contentKey) {
                    if (combinedValue.IsEmpty()) {
                        combinedValue = fld.Value();
                    } else if (combinedValue != fld.Value()) {
                        throw BNetworkRequestError(
                            __func__,
                            BNetworkRequestError::ProtocolError,
                            "Conflicting Content-Length values"
                        );
                    }
                }
            }

            if (!combinedValue.IsEmpty()) {
                // Convert to std::string for stol
                std::string tmp(combinedValue.String());
                try {
                    totalBytes    = std::stol(tmp);
                    haveByteCount = true;
                }
                catch (const std::logic_error&) {
                    throw BNetworkRequestError(
                        __func__,
                        BNetworkRequestError::ProtocolError,
                        "Invalid Content-Length value"
                    );
                }
            }

            // Decide specific body type
            if (haveByteCount && totalBytes == 0) {
                fBodyType    = HttpBodyType::NoContent;
                fStreamState = HttpInputStreamState::Done;
            }
            else if (haveByteCount) {
                fBodyType    = HttpBodyType::FixedSize;
                fStreamState = HttpInputStreamState::Body;
            }
            else {
                // Content-Length present but not parseable
                throw BNetworkRequestError(
                    __func__,
                    BNetworkRequestError::ProtocolError,
                    "Cannot parse Content-Length"
                );
            }
        }
        // 6. Rule [7]: Read until close
        else {
            fBodyType    = HttpBodyType::VariableSize;
            fStreamState = HttpInputStreamState::Body;
        }
    }

    // 4. Instantiate the correct body parser
    switch (fBodyType) {
        case HttpBodyType::VariableSize:
            fBodyParser = std::make_unique<HttpRawBodyParser>();
            break;

        case HttpBodyType::FixedSize:
            fBodyParser = std::make_unique<HttpRawBodyParser>(totalBytes, true);
            break;

        case HttpBodyType::Chunked:
            fBodyParser = std::make_unique<HttpChunkedBodyParser>();
            break;

        case HttpBodyType::NoContent:
        default:
            return true;
    }

    // 5. Wrap with decompression if needed
    const BString encodingKey("Content-Encoding");
    auto compIt = fields.FindField(encodingKey);
    if (compIt != fields.end()) {
        auto val = compIt->Value();
        if ((val == "gzip" || val == "deflate") && fBodyParser) {
            fBodyParser =
                std::make_unique<HttpBodyDecompression>(std::move(fBodyParser));
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
HttpRawBodyParser::HttpRawBodyParser(off_t bodyBytesTotalValue, bool hasTotalValue)
	:
	fBodyBytesTotalValue(bodyBytesTotalValue),
	fHasBodyBytesTotal(hasTotalValue)
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
