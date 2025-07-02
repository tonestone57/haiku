/*
 * Copyright 2022 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _B_HTTP_FIELDS_H_
#define _B_HTTP_FIELDS_H_

#include <list>
// #include <optional> // C++17, removed
// #include <string_view> // C++17, removed
#include <variant> // Will be handled in a later step
#include <vector>

#include <ErrorsExt.h>
#include <String.h>


namespace BPrivate {

namespace Network {


class BHttpFields
{
public:
		// Exceptions
	class InvalidInput;

	// Wrapper Types
	class FieldName;
	class Field;

	// Type Aliases
	using ConstIterator = std::list<Field>::const_iterator;

	// Constructors & Destructor
								BHttpFields();
								BHttpFields(std::initializer_list<Field> fields);
								BHttpFields(const BHttpFields& other);
								BHttpFields(BHttpFields&& other);
								~BHttpFields() noexcept;

	// Assignment operators
			BHttpFields&		operator=(const BHttpFields&);
			BHttpFields&		operator=(BHttpFields&&) noexcept;

	// Access list
			const Field&		operator[](size_t index) const;

	// Modifiers
			// void				AddField(const std::string_view& name, // C++17
			// 						const std::string_view& value); // C++17
			void				AddField(const BString& name, const BString& value);
			void				AddField(const char* name, const char* value); // For literals
			void				AddField(BString& field);
			void				AddFields(std::initializer_list<Field> fields);
			// void				RemoveField(const std::string_view& name) noexcept; // C++17
			void				RemoveField(const BString& name) noexcept;
			void				RemoveField(ConstIterator it) noexcept;
			void				MakeEmpty() noexcept;

	// Querying
			// ConstIterator		FindField(const std::string_view& name) const noexcept; // C++17
			ConstIterator		FindField(const BString& name) const noexcept;
			size_t				CountFields() const noexcept;
			// size_t				CountFields(const std::string_view& name) const noexcept; // C++17
			size_t				CountFields(const BString& name) const noexcept;

	// Range-based iteration
			ConstIterator		begin() const noexcept;
			ConstIterator		end() const noexcept;

private:
			std::list<Field>	fFields;
};


class BHttpFields::InvalidInput : public BError
{
public:
								InvalidInput(const char* origin, BString input);

	virtual	const char*			Message() const noexcept override;
	virtual	BString				DebugMessage() const override;

			BString				input;
};


class BHttpFields::FieldName
{
public:
	// Comparison
			bool				operator==(const BString& other) const noexcept;
			// bool				operator==(const std::string_view& other) const noexcept; // C++17
			bool				operator==(const FieldName& other) const noexcept;

	// Conversion
	// operator					std::string_view() const; // C++17
	operator					const BString&() const; // Or provide a BString Name() const;
	const BString&				GetString() const; // Explicit getter

private:
	friend class BHttpFields;

								FieldName() noexcept;
								// FieldName(const std::string_view& name) noexcept; // C++17
								FieldName(const BString& name) noexcept;
								FieldName(const char* name) noexcept; // For literals
								FieldName(const FieldName& other) noexcept;
								FieldName(FieldName&&) noexcept;
			FieldName&			operator=(const FieldName& other) noexcept;
			FieldName&			operator=(FieldName&&) noexcept;

			// std::string_view	fName; // C++17
			BString				fNameString;
};


class BHttpFields::Field
{
public:
	// Constructors
								Field() noexcept;
								// Field(const std::string_view& name, const std::string_view& value); // C++17
								Field(const BString& name, const BString& value);
								Field(const char* name, const char* value); // For literals
								Field(BString& field); // Assumes "Name: Value"
								Field(const Field& other);
								Field(Field&&) noexcept;

	// Assignment
			Field&				operator=(const Field& other);
			Field&				operator=(Field&& other) noexcept;

	// Access Operators
			const FieldName&	Name() const noexcept;
			// std::string_view	Value() const noexcept; // C++17
			const BString&		Value() const noexcept;
			// std::string_view	RawField() const noexcept; // C++17
			const BString&		RawField() const noexcept; // Returns fRawFieldString
			bool				IsEmpty() const noexcept; // Checks fHasRawField

private:
	friend class BHttpFields;

								Field(BString&& rawField); // Assumes "Name: Value" or just "NameValue" if no ':'

			// std::optional<BString> fRawField; // C++17
			BString fRawFieldString; // If not set, IsEmpty() will be true. Stores "Name: Value".
			bool fHasRawField = false;

			FieldName			fName;
			// std::string_view	fValue; // This will be handled later // C++17
			BString				fValueString; // Derived from fRawFieldString
};


} // namespace Network

} // namespace BPrivate

#endif // _B_HTTP_FIELDS_H_
