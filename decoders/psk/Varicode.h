#pragma once

#include <cstdint>
#include <string>

namespace psk {

// PSK31 Varicode (G3PLX), extended to all 256 byte values as fldigi does so
// UTF-8 (e.g. Cyrillic) and 8-bit code pages can be sent. Characters are
// separated by two or more zero bits; every code starts and ends with a one
// and never contains "00".
class Varicode
{
public:
	// Bit string (e.g. "1011") for a byte.
	static const char* Encode(unsigned char c);

	// Look up a code given as an integer whose binary representation is the
	// code (MSB first). Returns the byte or -1 if the code is unknown.
	static int Decode(uint32_t code);
};

// Streaming Varicode bit decoder.
class VaricodeDecoder
{
public:
	// Feeds one received bit. Returns the decoded byte (0..255) when the bit
	// completes a character, or -1 otherwise.
	int PushBit(int bit);
	void Reset();
	// Drops any character in progress and ignores bits until the next
	// character gap, so the tail of a half-received character is not
	// decoded as a different one.
	void Resync();

private:
	uint32_t m_code = 0;
	int m_bits = 0;
	int m_zeros = 2;
	bool m_overflow = false;
};

} // namespace psk
