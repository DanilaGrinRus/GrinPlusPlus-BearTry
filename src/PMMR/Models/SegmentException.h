#pragma once

#include <cstdint>
#include <exception>
#include <string>

// Error conditions for PIBD segment creation and validation.
// Mirrors grin v5.5.2 SegmentError so that a peer that sends a bad segment can
// be distinguished from a malformed message.
enum class ESegmentError
{
	MissingLeaf,
	MissingHash,
	NonExistent,
	Mismatch,
	TooLarge
};

class SegmentException : public std::exception
{
public:
	SegmentException(const ESegmentError error, std::string message)
		: m_error(error), m_message(std::move(message))
	{
	}

	const char* what() const noexcept final { return m_message.c_str(); }

	ESegmentError GetError() const noexcept { return m_error; }

private:
	ESegmentError m_error;
	std::string m_message;
};
