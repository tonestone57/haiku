/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Niels Sascha Reedijk, niels.reedijk@gmail.com
 */

#include <HttpRequest.h>

#include <algorithm>
#include <ctype.h>
#include <sstream>
#include <utility>

#include <DataIO.h>
#include <HttpFields.h>
#include <MimeType.h>
#include <NetServicesDefs.h>
#include <Url.h>

#include "HttpBuffer.h"
#include "HttpPrivate.h"

// using namespace std::literals; // C++17, for ""sv suffix, no longer needed
using namespace BPrivate::Network;


// #pragma mark -- BHttpMethod::InvalidMethod


BHttpMethod::InvalidMethod::InvalidMethod(const char* origin, BString input)
	:
	BError(origin),
	input(std::move(input))
{
}


const char*
BHttpMethod::InvalidMethod::Message() const noexcept
{
	if (input.IsEmpty())
		return "The HTTP method cannot be empty";
	else
		return "Unsupported characters in the HTTP method";
}


BString
BHttpMethod::InvalidMethod::DebugMessage() const
{
	BString output = BError::DebugMessage();
	if (!input.IsEmpty())
		output << ":\t " << input << "\n";
	return output;
}


// #pragma mark -- BHttpMethod


BHttpMethod::BHttpMethod(Verb verb) noexcept
	:
	fVerbValue(verb),
	fIsCustomMethod(false)
{
	// Cache the string form
	MethodString();
}

// BHttpMethod::BHttpMethod(const std::string_view& verb)
// C++17 version, replaced by BString and const char* versions in header
// and corresponding .cpp implementations if any (likely covered by BString one)

BHttpMethod::BHttpMethod(const BString& method)
	:
	fVerbValue(Get), // Default verb
	fCustomMethodString(method),
	fIsCustomMethod(true)
{
	if (method.IsEmpty() || !validate_http_token_string(method.String()))
		throw BHttpMethod::InvalidMethod(__PRETTY_FUNCTION__, method);
	fMethodStringInternal = method;
}


BHttpMethod::BHttpMethod(const char* method)
	:
	fVerbValue(Get), // Default verb
	fCustomMethodString(method),
	fIsCustomMethod(true)
{
	if (method == nullptr || method[0] == '\0' || !validate_http_token_string(method))
		throw BHttpMethod::InvalidMethod(__PRETTY_FUNCTION__, method);
	fMethodStringInternal = method;
}


BHttpMethod::BHttpMethod(const BHttpMethod& other)
	:
	fVerbValue(other.fVerbValue),
	fCustomMethodString(other.fCustomMethodString),
	fIsCustomMethod(other.fIsCustomMethod),
	fMethodStringInternal(other.fMethodStringInternal)
{
}


BHttpMethod::BHttpMethod(BHttpMethod&& other) noexcept
	:
	fVerbValue(other.fVerbValue),
	fCustomMethodString(std::move(other.fCustomMethodString)),
	fIsCustomMethod(other.fIsCustomMethod),
	fMethodStringInternal(std::move(other.fMethodStringInternal))
{
	other.fVerbValue = Get;
	other.fIsCustomMethod = false;
	// other.fMethodStringInternal should be updated if it was representing a verb
	// For simplicity, let it be cleared or set by its new state.
	other.fMethodStringInternal.Truncate(0);
}


BHttpMethod::~BHttpMethod()
{
}


BHttpMethod& BHttpMethod::operator=(const BHttpMethod& other)
{
	if (this != &other) {
		fVerbValue = other.fVerbValue;
		fCustomMethodString = other.fCustomMethodString;
		fIsCustomMethod = other.fIsCustomMethod;
		fMethodStringInternal = other.fMethodStringInternal;
	}
	return *this;
}


BHttpMethod&
BHttpMethod::operator=(BHttpMethod&& other) noexcept
{
	if (this != &other) {
		fVerbValue = other.fVerbValue;
		fCustomMethodString = std::move(other.fCustomMethodString);
		fIsCustomMethod = other.fIsCustomMethod;
		fMethodStringInternal = std::move(other.fMethodStringInternal);

		other.fVerbValue = Get;
		other.fIsCustomMethod = false;
		other.fMethodStringInternal.Truncate(0);
	}
	return *this;
}


bool
BHttpMethod::operator==(const BHttpMethod::Verb& other) const noexcept
{
	if (fIsCustomMethod) {
		// Compare custom string to string representation of Verb other
		BHttpMethod otherAsMethod(other); // Create a temporary from Verb
		return fCustomMethodString.Compare(otherAsMethod.MethodString()) == 0;
	}
	return fVerbValue == other;
}


bool
BHttpMethod::operator!=(const BHttpMethod::Verb& other) const noexcept
{
	return !operator==(other);
}


const BString&
BHttpMethod::MethodString() const noexcept
{
	if (fIsCustomMethod) {
		return fCustomMethodString;
	}
	// For non-custom methods, ensure fMethodStringInternal is populated
	// This const_cast is unfortunate but necessary if we want to cache lazily in a const method.
	// Alternatively, populate in all constructors.
	BHttpMethod* nonConstThis = const_cast<BHttpMethod*>(this);
	if (nonConstThis->fMethodStringInternal.IsEmpty() && !fIsCustomMethod) {
		switch (fVerbValue) {
			case Get:
				nonConstThis->fMethodStringInternal = "GET";
				break;
			case Head:
				nonConstThis->fMethodStringInternal = "HEAD";
				break;
			case Post:
				nonConstThis->fMethodStringInternal = "POST";
				break;
			case Put:
				nonConstThis->fMethodStringInternal = "PUT";
				break;
			case Delete:
				nonConstThis->fMethodStringInternal = "DELETE";
				break;
			case Connect:
				nonConstThis->fMethodStringInternal = "CONNECT";
				break;
			case Options:
				nonConstThis->fMethodStringInternal = "OPTIONS";
				break;
			case Trace:
				nonConstThis->fMethodStringInternal = "TRACE";
				break;
			default:
				// Should not be reached if fIsCustomMethod is false
				// and fVerbValue is a valid Verb.
				// For safety, assign a default or assert.
				nonConstThis->fMethodStringInternal = "GET"; // Fallback
				break;
		}
	}
	return fMethodStringInternal;
}


bool
BHttpMethod::IsCustom() const noexcept
{
	return fIsCustomMethod;
}


BHttpMethod::Verb
BHttpMethod::GetVerb() const
{
	if (fIsCustomMethod) {
		// Or throw an exception, or return a special error Verb value
		// Depending on desired contract. Let's throw for now.
		throw BRuntimeError(__PRETTY_FUNCTION__, "GetVerb() called on a custom method.");
	}
	return fVerbValue;
}


// #pragma mark -- BHttpRequest::Data
static const BUrl kDefaultUrl = BUrl();
static const BHttpMethod kDefaultMethod = BHttpMethod::Get;
static const BHttpFields kDefaultOptionalFields = BHttpFields();

struct BHttpRequest::Data {
	BUrl url = kDefaultUrl;
	BHttpMethod method = kDefaultMethod;
	uint8 maxRedirections = 8;
	BHttpFields optionalFields;
	// std::optional<BHttpAuthentication> authentication;
	BHttpAuthentication authenticationValue;
	bool hasAuthentication;
	bool stopOnError = false;
	bigtime_t timeout = B_INFINITE_TIMEOUT;
	// std::optional<Body> requestBody;
	Body requestBodyValue;
	bool hasRequestBody;

	Data() : hasAuthentication(false), hasRequestBody(false) {}
};


// #pragma mark -- BHttpRequest helper functions


/*!
	\brief Build basic authentication header
*/
static inline BString
build_basic_http_header(const BString& username, const BString& password)
{
	BString basicEncode, result;
	basicEncode << username << ":" << password;
	result << "Basic " << encode_to_base64(basicEncode);
	return result;
}


// #pragma mark -- BHttpRequest


BHttpRequest::BHttpRequest()
	:
	fData(std::make_unique<Data>())
{
}


BHttpRequest::BHttpRequest(const BUrl& url)
	:
	fData(std::make_unique<Data>())
{
	SetUrl(url);
}


BHttpRequest::BHttpRequest(BHttpRequest&& other) noexcept = default;


BHttpRequest::~BHttpRequest() = default;


BHttpRequest& BHttpRequest::operator=(BHttpRequest&&) noexcept = default;


bool
BHttpRequest::IsEmpty() const noexcept
{
	return (!fData || !fData->url.IsValid());
}


const BHttpAuthentication*
BHttpRequest::Authentication() const noexcept
{
	if (fData && fData->hasAuthentication)
		return std::addressof(fData->authenticationValue);
	return nullptr;
}


const BHttpFields&
BHttpRequest::Fields() const noexcept
{
	if (!fData)
		return kDefaultOptionalFields;
	return fData->optionalFields;
}


uint8
BHttpRequest::MaxRedirections() const noexcept
{
	if (!fData)
		return 8;
	return fData->maxRedirections;
}


const BHttpMethod&
BHttpRequest::Method() const noexcept
{
	if (!fData)
		return kDefaultMethod;
	return fData->method;
}


const BHttpRequest::Body*
BHttpRequest::RequestBody() const noexcept
{
	if (fData && fData->hasRequestBody)
		return std::addressof(fData->requestBodyValue);
	return nullptr;
}


bool
BHttpRequest::StopOnError() const noexcept
{
	if (!fData)
		return false;
	return fData->stopOnError;
}


bigtime_t
BHttpRequest::Timeout() const noexcept
{
	if (!fData)
		return B_INFINITE_TIMEOUT;
	return fData->timeout;
}


const BUrl&
BHttpRequest::Url() const noexcept
{
	if (!fData)
		return kDefaultUrl;
	return fData->url;
}


void
BHttpRequest::SetAuthentication(const BHttpAuthentication& authentication)
{
	if (!fData)
		fData = std::make_unique<Data>();

	fData->authenticationValue = authentication;
	fData->hasAuthentication = true;
}


// static constexpr std::array<std::string_view, 6> fReservedOptionalFieldNames // C++17
// 	= {"Host"sv, "Accept-Encoding"sv, "Connection"sv, "Content-Type"sv, "Content-Length"sv};
static const std::array<const char*, 5> fReservedOptionalFieldNames // Last one seems unused or typo in original size
	= {"Host", "Accept-Encoding", "Connection", "Content-Type", "Content-Length"};


void
BHttpRequest::SetFields(const BHttpFields& fields)
{
	if (!fData)
		fData = std::make_unique<Data>();

	for (auto& field: fields) {
		// field.Name() returns BString, so we need to compare with const char*
		bool isReserved = false;
		for (const char* reservedName : fReservedOptionalFieldNames) {
			if (field.Name() == reservedName) {
				isReserved = true;
				break;
			}
		}
		if (isReserved) {
			throw BHttpFields::InvalidInput(
				__PRETTY_FUNCTION__, field.Name());
		}
	}
	fData->optionalFields = fields;
}


void
BHttpRequest::SetMaxRedirections(uint8 maxRedirections)
{
	if (!fData)
		fData = std::make_unique<Data>();
	fData->maxRedirections = maxRedirections;
}


void
BHttpRequest::SetMethod(const BHttpMethod& method)
{
	if (!fData)
		fData = std::make_unique<Data>();
	fData->method = method;
}


void
BHttpRequest::SetRequestBody(
	std::unique_ptr<BDataIO> input, BString mimeType, off_t sizeParam, bool hasSizeParam)
{
	if (input == nullptr)
		throw std::invalid_argument("input cannot be null");

	// TODO: support optional mimetype arguments like type/subtype;parameter=value
	if (!BMimeType::IsValid(mimeType.String()))
		throw std::invalid_argument("mimeType must be a valid mimetype");

	// TODO: review if there should be complex validation between the method and whether or not
	// there is a request body. The current implementation does the validation at the request
	// generation stage, where GET, HEAD, OPTIONS, CONNECT and TRACE will not submit a body.

	if (!fData)
		fData = std::make_unique<Data>();

	fData->requestBodyValue.input = std::move(input);
	fData->requestBodyValue.mimeType = std::move(mimeType);
	fData->requestBodyValue.sizeValue = hasSizeParam ? sizeParam : 0;
	fData->requestBodyValue.hasSize = hasSizeParam;
	fData->requestBodyValue.hasStartPosition = false; // Initialize
	fData->hasRequestBody = true;


	// Check if the input is a BPositionIO, and if so, store the current position, so that it can
	// be rewinded in case of a redirect.
	auto inputPositionIO = dynamic_cast<BPositionIO*>(fData->requestBodyValue.input.get());
	if (inputPositionIO != nullptr) {
		fData->requestBodyValue.startPositionValue = inputPositionIO->Position();
		fData->requestBodyValue.hasStartPosition = true;
	}
}


void
BHttpRequest::SetStopOnError(bool stopOnError)
{
	if (!fData)
		fData = std::make_unique<Data>();
	fData->stopOnError = stopOnError;
}


void
BHttpRequest::SetTimeout(bigtime_t timeout)
{
	if (!fData)
		fData = std::make_unique<Data>();
	fData->timeout = timeout;
}


void
BHttpRequest::SetUrl(const BUrl& url)
{
	if (!fData)
		fData = std::make_unique<Data>();

	if (!url.IsValid())
		throw BInvalidUrl(__PRETTY_FUNCTION__, BUrl(url));
	if (url.Protocol() != "http" && url.Protocol() != "https") {
		// TODO: optimize BStringList with modern language features
		BStringList list;
		list.Add("http");
		list.Add("https");
		throw BUnsupportedProtocol(__PRETTY_FUNCTION__, BUrl(url), list);
	}
	fData->url = url;
}


void
BHttpRequest::ClearAuthentication() noexcept
{
	if (fData)
		fData->hasAuthentication = false;
		// fData->authenticationValue can be left as is or cleared
}


std::unique_ptr<BDataIO>
BHttpRequest::ClearRequestBody() noexcept
{
	if (fData && fData->hasRequestBody) {
		auto body = std::move(fData->requestBodyValue.input);
		fData->hasRequestBody = false;
		// fData->requestBodyValue members can be left or cleared
		return body;
	}
	return nullptr;
}


BString
BHttpRequest::HeaderToString() const
{
	HttpBuffer buffer;
	SerializeHeaderTo(buffer);

	size_t length;
	const unsigned char* data = buffer.Data(length);
	// buffer.RemainingBytes() should be equivalent to length here after Data() call
	return BString(static_cast<const char*>(static_cast<const void*>(data)), length);
}


/*!
	\brief Private method used by BHttpSession::Request to rewind the content in case of redirect

	\retval true Content was rewinded successfully. Also the case if there is no content
	\retval false Cannot/could not rewind content.
*/
bool
BHttpRequest::RewindBody() noexcept
{
	if (fData && fData->hasRequestBody && fData->requestBodyValue.hasStartPosition) {
		auto inputData = dynamic_cast<BPositionIO*>(fData->requestBodyValue.input.get());
		if (inputData == nullptr) // Should not happen if startPosition is set, but good practice
			return false;
		return fData->requestBodyValue.startPositionValue
			== inputData->Seek(fData->requestBodyValue.startPositionValue, SEEK_SET);
	}
	return true;
}


/*!
	\brief Private method used by HttpSerializer::SetTo() to serialize the header data into a
		buffer.
*/
void
BHttpRequest::SerializeHeaderTo(HttpBuffer& buffer) const
{
	// Method & URL
	//	TODO: proxy
	buffer << fData->method.MethodString() << " "; // Removed "sv"
	if (fData->url.HasPath() && fData->url.Path().Length() > 0)
		buffer << fData->url.Path(); // BString can be directly streamed
	else
		buffer << "/"; // Removed "sv"

	if (fData->url.HasRequest())
		buffer << "?" << Url().Request().String(); // Removed "sv"

	// TODO: switch between HTTP 1.0 and 1.1 based on configuration
	buffer << " HTTP/1.1\r\n"; // Removed "sv"

	BHttpFields outputFields;
	if (true /* http == 1.1 */) {
		BString host = fData->url.Host();
		int defaultPort = fData->url.Protocol() == "http" ? 80 : 443;
		if (fData->url.HasPort() && fData->url.Port() != defaultPort)
			host << ':' << fData->url.Port();

		// AddFields takes an initializer_list of BHttpFields::Field.
		// BHttpFields::Field can be constructed from {const char*, const char*}.
		outputFields.AddFields({
			{"Host", host.String()}, {"Accept-Encoding", "gzip"},
			// Allows the server to compress data using the "gzip" format.
			// "deflate" is not supported, because there are two interpretations
			// of what it means (the RFC and Microsoft products), and we don't
			// want to handle this. Very few websites support only deflate,
			// and most of them will send gzip, or at worst, uncompressed data.
			{"Connection", "close"}
			// Let the remote server close the connection after response since
			// we don't handle multiple request on a single connection
		});
	}

	if (fData->hasAuthentication) {
		// This request will add a Basic authorization header
		BString authorization = build_basic_http_header(
			fData->authenticationValue.username, fData->authenticationValue.password);
		outputFields.AddField("Authorization", authorization.String()); // Removed "sv" and string_view
	}

	if (fData->hasRequestBody) {
		outputFields.AddField(
			"Content-Type", fData->requestBodyValue.mimeType.String()); // Removed "sv" and string_view
		if (fData->requestBodyValue.hasSize)
			outputFields.AddField("Content-Length", std::to_string(fData->requestBodyValue.sizeValue)); // Removed "sv"
		else
			throw BRuntimeError(__PRETTY_FUNCTION__,
				"Transfer body with unknown content length; chunked transfer not supported");
	}

	for (const auto& field: outputFields)
		buffer << field.RawField() << "\r\n"; // Removed "sv"

	for (const auto& field: fData->optionalFields)
		buffer << field.RawField() << "\r\n"; // Removed "sv"

	buffer << "\r\n"; // Removed "sv"
}
