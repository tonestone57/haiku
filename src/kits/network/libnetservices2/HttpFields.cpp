/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Niels Sascha Reedijk, niels.reedijk@gmail.com
 */

#include <HttpFields.h>

#include <algorithm>
#include <ctype.h>
#include <utility>

#include "HttpPrivate.h"

using namespace BPrivate::Network;


// #pragma mark -- utilities


/*!
	\brief Validate whether the string is a valid HTTP header value

	RFC 7230 section 3.2.6 determines that valid tokens for the header are:
	HTAB ('\t'), SP (32), all visible ASCII characters (33-126), and all characters that
	not control characters (in the case of a char, any value < 0)

	\note When printing out the HTTP header, sometimes the string needs to be quoted and some
		characters need to be escaped. This function is not checking for whether the string can
		be transmitted as is.

	\returns \c true if the string is valid, or \c false if it is not.
*/
static inline bool
validate_value_string(const BString& string)
{
	for (int32 i = 0; i < string.Length(); ++i) {
		char c = string[i];
		if ((c >= 0 && c < 32) || c == 127 || c == '\t')
			return false;
	}
	return true;
}


/*!
	\brief Case insensitively compare two string_views.

	Inspired by https://stackoverflow.com/a/4119881
*/
static inline bool
iequals(const BString& a, const BString& b)
{
	if (a.Length() != b.Length())
		return false;
	for (int32 i = 0; i < a.Length(); ++i) {
		if (tolower(a[i]) != tolower(b[i]))
			return false;
	}
	return true;
}


/*!
	\brief Trim whitespace from the beginning and end of a string_view

	Inspired by:
		https://terrislinenbach.medium.com/trimming-whitespace-from-a-string-view-6795e18b108f
*/
static inline BString
trim(const BString& in)
{
	int32_t start = 0;
	while (start < in.Length() && isspace(in[start])) {
		start++;
	}

	int32_t end = in.Length() - 1;
	while (end >= start && isspace(in[end])) {
		end--;
	}

	if (start > end) // String is all whitespace or empty
		return BString();

	return BString(in.String() + start, end - start + 1);
}


// #pragma mark -- BHttpFields::InvalidHeader


BHttpFields::InvalidInput::InvalidInput(const char* origin, BString input)
	:
	BError(origin),
	input(std::move(input))
{
}


const char*
BHttpFields::InvalidInput::Message() const noexcept
{
	return "Invalid format or unsupported characters in input";
}


BString
BHttpFields::InvalidInput::DebugMessage() const
{
	BString output = BError::DebugMessage();
	output << "\t " << input << "\n";
	return output;
}


// #pragma mark -- BHttpFields::Name


BHttpFields::FieldName::FieldName() noexcept
	// fNameString is default constructed
{
}

// This constructor was for std::string_view, which is removed from the header.
// BHttpFields::FieldName::FieldName(const std::string_view& name) noexcept
// 	:
// 	fName(name) // This fName would be BString fNameString now
// {
// }


BHttpFields::FieldName::FieldName(const BString& name) noexcept
	:
	fNameString(name)
{
}


BHttpFields::FieldName::FieldName(const char* name) noexcept
	:
	fNameString(name)
{
}


/*!
	\brief Copy constructor;
*/
BHttpFields::FieldName::FieldName(const FieldName& other) noexcept
	: fNameString(other.fNameString) // Default would work too if BString copy is fine
{
}


/*!
	\brief Move constructor

	Moving leaves the other object in the empty state. It is implemented to satisfy the internal
	requirements of BHttpFields and std::list<Field>. Once an object is moved from it must no
	longer be used as an entry in a BHttpFields object.
*/
BHttpFields::FieldName::FieldName(FieldName&& other) noexcept
	:
	fNameString(std::move(other.fNameString))
{
	// other.fNameString is now in a valid but unspecified state (moved from)
}


/*!
	\brief Copy assignment;
*/
BHttpFields::FieldName& BHttpFields::FieldName::operator=(
	const BHttpFields::FieldName& other) noexcept
{
	if (this != &other) {
		fNameString = other.fNameString;
	}
	return *this;
}


/*!
	\brief Move assignment

	Moving leaves the other object in the empty state. It is implemented to satisfy the internal
	requirements of BHttpFields and std::list<Field>. Once an object is moved from it must no
	longer be used as an entry in a BHttpFields object.
*/
BHttpFields::FieldName&
BHttpFields::FieldName::operator=(BHttpFields::FieldName&& other) noexcept
{
	if (this != &other) {
		fNameString = std::move(other.fNameString);
		// other.fNameString is now in a valid but unspecified state
	}
	return *this;
}


bool
BHttpFields::FieldName::operator==(const BString& other) const noexcept
{
	return iequals(fNameString, other);
}

// operator==(const std::string_view& other) removed from header

bool
BHttpFields::FieldName::operator==(const BHttpFields::FieldName& other) const noexcept
{
	return iequals(fNameString, other.fNameString);
}

// operator std::string_view() removed from header
BHttpFields::FieldName::operator const BString&() const
{
	return fNameString;
}


const BString&
BHttpFields::FieldName::GetString() const
{
	return fNameString;
}


// #pragma mark -- BHttpFields::Field


BHttpFields::Field::Field() noexcept
	: // fName (FieldName) and fValueString (BString) are default constructed
	fHasRawField(false)
{
}

// Constructor for std::string_view name, std::string_view value removed from header,
// so definition removed. BString and const char* overloads will be used.

BHttpFields::Field::Field(const BString& name, const BString& value)
	: fHasRawField(true), fName(name), fValueString(value)
{
	if (name.IsEmpty() || !validate_http_token_string(name)) // validate_http_token_string needs to be adapted for BString
		throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, name);
	if (value.IsEmpty() || !validate_value_string(value)) // validate_value_string already takes BString
		throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, value);

	fRawFieldString.SetToFormat("%s: %s", name.String(), value.String());
}

BHttpFields::Field::Field(const char* name, const char* value)
	: fHasRawField(true), fName(name), fValueString(value)
{
	BString bname(name);
	BString bvalue(value);
	if (bname.IsEmpty() || !validate_http_token_string(bname))
		throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, bname);
	if (bvalue.IsEmpty() || !validate_value_string(bvalue))
		throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, bvalue);

	fRawFieldString.SetToFormat("%s: %s", name, value);
}


BHttpFields::Field::Field(BString& field) // Takes non-const BString& to potentially move from it
	: fHasRawField(true)
{
	// Check if the input contains a key, a separator and a value.
	auto separatorIndex = field.FindFirst(':');
	if (separatorIndex <= 0) // Must have name, ':', and some value part (even if empty after trim)
		throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, field);

	BString namePart;
	field.CopyInto(namePart, 0, separatorIndex);

	// The value part starts after ':' and any leading spaces.
	// The original trim function returned a new string_view. Now it returns BString.
	BString valuePartFull;
	if (separatorIndex < field.Length() -1)
		field.CopyInto(valuePartFull, separatorIndex + 1, field.Length() - (separatorIndex + 1));

	BString trimmedValuePart = trim(valuePartFull);


	if (namePart.IsEmpty() || !validate_http_token_string(namePart)) // validate_http_token_string needs BString
		throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, namePart);
	// Value can be empty after trim, but original validate_value_string checked for length > 0.
	// Let's assume an empty value is valid for a field (e.g. "X-Empty-Header:").
	// The original validate_value_string checked `value.length() == 0` then threw.
	// We should keep that behavior if intended. For now, let's assume empty value is okay,
	// but `validate_value_string` itself will check for invalid chars if not empty.
	if (!trimmedValuePart.IsEmpty() && !validate_value_string(trimmedValuePart))
		throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, trimmedValuePart);

	// If the original `field` BString is being stored in `fRawFieldString`,
	// then `fName` and `fValueString` should reference parts of it, or be copies.
	// The header implies fName is FieldName (which holds a BString) and fValueString is BString.
	// The original string_view fName and fValue pointed into fRawField.
	// To maintain that, we'd need to store pointers/offsets.
	// For simplicity now, fName and fValueString will be copies.

	fRawFieldString = field; // Store the original passed-in string
	fName = FieldName(namePart);
	fValueString = trimmedValuePart;
}

BHttpFields::Field::Field(BString&& rawField)
    : // Similar to the BString& constructor, but takes rvalue
    fHasRawField(true)
{
    auto separatorIndex = rawField.FindFirst(':');
    if (separatorIndex <= 0)
        throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, rawField);

    BString namePart;
    rawField.CopyInto(namePart, 0, separatorIndex);

    BString valuePartFull;
	if (separatorIndex < rawField.Length() -1)
	    rawField.CopyInto(valuePartFull, separatorIndex + 1, rawField.Length() - (separatorIndex + 1));
    BString trimmedValuePart = trim(valuePartFull);

    if (namePart.IsEmpty() || !validate_http_token_string(namePart))
        throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, namePart);
    if (!trimmedValuePart.IsEmpty() && !validate_value_string(trimmedValuePart))
        throw BHttpFields::InvalidInput(__PRETTY_FUNCTION__, trimmedValuePart);

    fRawFieldString = std::move(rawField);
    fName = FieldName(namePart);
    fValueString = trimmedValuePart;
}


BHttpFields::Field::Field(const BHttpFields::Field& other)
	:
	fRawFieldString(other.fRawFieldString),
	fHasRawField(other.fHasRawField),
	fName(other.fName), // FieldName has proper copy constructor
	fValueString(other.fValueString)
{
}


BHttpFields::Field::Field(BHttpFields::Field&& other) noexcept
	:
	fRawFieldString(std::move(other.fRawFieldString)),
	fHasRawField(other.fHasRawField),
	fName(std::move(other.fName)), // FieldName has proper move constructor
	fValueString(std::move(other.fValueString))
{
	other.fHasRawField = false;
	// other.fName and other.fValueString are in a valid but unspecified state
}


BHttpFields::Field&
BHttpFields::Field::operator=(const BHttpFields::Field& other)
{
	if (this != &other) {
		fRawFieldString = other.fRawFieldString;
		fHasRawField = other.fHasRawField;
		fName = other.fName; // FieldName has proper copy assignment
		fValueString = other.fValueString;
	}
	return *this;
}


BHttpFields::Field&
BHttpFields::Field::operator=(BHttpFields::Field&& other) noexcept
{
	if (this != &other) {
		fRawFieldString = std::move(other.fRawFieldString);
		fHasRawField = other.fHasRawField;
		fName = std::move(other.fName); // FieldName has proper move assignment
		fValueString = std::move(other.fValueString);

		other.fHasRawField = false;
		// other.fName and other.fValueString are in a valid but unspecified state
	}
	return *this;
}


const BHttpFields::FieldName&
BHttpFields::Field::Name() const noexcept
{
	return fName; // fName is FieldName, which has BString fNameString
}


const BString&
BHttpFields::Field::Value() const noexcept
{
	return fValueString;
}


const BString&
BHttpFields::Field::RawField() const noexcept
{
	// If !fHasRawField, fRawFieldString might be empty or hold some default.
	// The caller should check IsEmpty() first if they need to distinguish.
	return fRawFieldString;
}


bool
BHttpFields::Field::IsEmpty() const noexcept
{
	return !fHasRawField;
}


// #pragma mark -- BHttpFields


BHttpFields::BHttpFields()
{
}


BHttpFields::BHttpFields(std::initializer_list<BHttpFields::Field> fields)
{
	AddFields(fields);
}


BHttpFields::BHttpFields(const BHttpFields& other) = default;


BHttpFields::BHttpFields(BHttpFields&& other)
	:
	fFields(std::move(other.fFields))
{
	// Explicitly clear the other list, as the C++ standard does not specify that the other list
	// will be empty.
	other.fFields.clear();
}


BHttpFields::~BHttpFields() noexcept
{
}


BHttpFields& BHttpFields::operator=(const BHttpFields& other) = default;


BHttpFields&
BHttpFields::operator=(BHttpFields&& other) noexcept
{
	fFields = std::move(other.fFields);

	// Explicitly clear the other list, as the C++ standard does not specify that the other list
	// will be empty.
	other.fFields.clear();
	return *this;
}


const BHttpFields::Field&
BHttpFields::operator[](size_t index) const
{
	if (index >= fFields.size())
		throw BRuntimeError(__PRETTY_FUNCTION__, "Index out of bounds");
	auto it = fFields.cbegin();
	std::advance(it, index);
	return *it;
}


// Definition for AddField(const std::string_view&, const std::string_view&) removed,
// as it's removed from the header. BString and const char* overloads are used.

void
BHttpFields::AddField(BString& field)
{
	fFields.emplace_back(field);
}


void
BHttpFields::AddFields(std::initializer_list<Field> fields)
{
	for (auto& field: fields) {
		if (!field.IsEmpty())
			fFields.push_back(std::move(field));
	}
}


void
BHttpFields::RemoveField(const BString& name) noexcept
{
	for (auto it = FindField(name); it != end(); it = FindField(name)) {
		fFields.erase(it);
	}
}


void
BHttpFields::RemoveField(ConstIterator it) noexcept
{
	fFields.erase(it);
}


void
BHttpFields::MakeEmpty() noexcept
{
	fFields.clear();
}


BHttpFields::ConstIterator
BHttpFields::FindField(const BString& name) const noexcept
{
	for (auto it = fFields.cbegin(); it != fFields.cend(); it++) {
		if ((*it).Name() == name) // FieldName::operator==(const BString&)
			return it;
	}
	return fFields.cend();
}


size_t
BHttpFields::CountFields() const noexcept
{
	return fFields.size();
}


size_t
BHttpFields::CountFields(const BString& name) const noexcept
{
	size_t count = 0;
	for (auto it = fFields.cbegin(); it != fFields.cend(); it++) {
		if ((*it).Name() == name) // FieldName::operator==(const BString&)
			count += 1;
	}
	return count;
}


BHttpFields::ConstIterator
BHttpFields::begin() const noexcept
{
	return fFields.cbegin();
}


BHttpFields::ConstIterator
BHttpFields::end() const noexcept
{
	return fFields.cend();
}
