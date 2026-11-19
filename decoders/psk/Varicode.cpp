#include "Varicode.h"

#include <array>
#include <set>
#include <string>

namespace psk {

namespace {

const char* const kTable[128] = {
	"1010101011", "1011011011", "1011101101", "1101110111", // NUL SOH STX ETX
	"1011101011", "1101011111", "1011101111", "1011111101", // EOT ENQ ACK BEL
	"1011111111", "11101111",   "11101",      "1101101111", // BS  HT  LF  VT
	"1011011101", "11111",      "1101110101", "1110101011", // FF  CR  SO  SI
	"1011110111", "1011110101", "1110101101", "1110101111", // DLE DC1 DC2 DC3
	"1101011011", "1101101011", "1101101101", "1101010111", // DC4 NAK SYN ETB
	"1101111011", "1101111101", "1110110111", "1101010101", // CAN EM  SUB ESC
	"1101011101", "1110111011", "1011111011", "1101111111", // FS  GS  RS  US
	"1",          "111111111",  "101011111",  "111110101",  // SP  !   "   #
	"111011011",  "1011010101", "1010111011", "101111111",  // $   %   &   '
	"11111011",   "11110111",   "101101111",  "111011111",  // (   )   *   +
	"1110101",    "110101",     "1010111",    "110101111",  // ,   -   .   /
	"10110111",   "10111101",   "11101101",   "11111111",   // 0   1   2   3
	"101110111",  "101011011",  "101101011",  "110101101",  // 4   5   6   7
	"110101011",  "110110111",  "11110101",   "110111101",  // 8   9   :   ;
	"111101101",  "1010101",    "111010111",  "1010101111", // <   =   >   ?
	"1010111101", "1111101",    "11101011",   "10101101",   // @   A   B   C
	"10110101",   "1110111",    "11011011",   "11111101",   // D   E   F   G
	"101010101",  "1111111",    "111111101",  "101111101",  // H   I   J   K
	"11010111",   "10111011",   "11011101",   "10101011",   // L   M   N   O
	"11010101",   "111011101",  "10101111",   "1101111",    // P   Q   R   S
	"1101101",    "101010111",  "110110101",  "101011101",  // T   U   V   W
	"101110101",  "101111011",  "1010101101", "111110111",  // X   Y   Z   [
	"111101111",  "111111011",  "1010111111", "101101101",  // \   ]   ^   _
	"1011011111", "1011",       "1011111",    "101111",     // `   a   b   c
	"101101",     "11",         "111101",     "1011011",    // d   e   f   g
	"101011",     "1101",       "111101011",  "10111111",   // h   i   j   k
	"11011",      "111011",     "1111",       "111",        // l   m   n   o
	"111111",     "110111111",  "10101",      "10111",      // p   q   r   s
	"101",        "110111",     "1111011",    "1101011",    // t   u   v   w
	"11011111",   "1011101",    "111010101",  "1010110111", // x   y   z   {
	"110111011",  "1010110101", "1011010111", "1110110101", // |   }   ~   DEL
};

// Codes for bytes 128..255 (fldigi's extension, used to send UTF-8 and
// other 8-bit text) are the remaining valid code words in order of length,
// then value: every code starts and ends with a one and contains no "00".
// The longest is 12 bits, so every code value fits below 1 << 12.
constexpr int kMaxCodeBits = 12;

struct Tables
{
	std::array<std::string, 256> codes;
	std::array<int16_t, 1 << kMaxCodeBits> reverse;
};

Tables BuildTables()
{
	Tables t;
	std::set<std::string> used;
	for (int c = 0; c < 128; ++c)
	{
		t.codes[static_cast<size_t>(c)] = kTable[c];
		used.insert(kTable[c]);
	}
	int next = 128;
	for (int bits = 1; bits <= kMaxCodeBits && next < 256; ++bits)
	{
		for (uint32_t v = 1u << (bits - 1); v < (1u << bits) && next < 256; ++v)
		{
			std::string code;
			for (int b = bits - 1; b >= 0; --b)
			{
				code.push_back((v >> b) & 1u ? '1' : '0');
			}
			if (code.back() == '1' && code.find("00") == std::string::npos && !used.count(code))
			{
				t.codes[static_cast<size_t>(next++)] = code;
			}
		}
	}

	t.reverse.fill(-1);
	for (int c = 0; c < 256; ++c)
	{
		uint32_t code = 0;
		for (char bit : t.codes[static_cast<size_t>(c)])
		{
			code = (code << 1) | (bit == '1' ? 1u : 0u);
		}
		t.reverse[code] = static_cast<int16_t>(c);
	}
	return t;
}

const Tables& GetTables()
{
	static const Tables tables = BuildTables();
	return tables;
}

} // namespace

const char* Varicode::Encode(unsigned char c)
{
	return GetTables().codes[c].c_str();
}

int Varicode::Decode(uint32_t code)
{
	const auto& reverse = GetTables().reverse;
	return code < reverse.size() ? reverse[code] : -1;
}

int VaricodeDecoder::PushBit(int bit)
{
	if (bit)
	{
		if (m_overflow)
		{
			m_zeros = 0;
			return -1;
		}
		// A single zero inside a character is part of the code.
		if (m_zeros == 1)
		{
			m_code <<= 1;
			++m_bits;
		}
		m_code = (m_code << 1) | 1u;
		++m_bits;
		m_zeros = 0;
		if (m_bits > kMaxCodeBits)
		{
			// Too long to be valid; drop it and wait for the next "00" gap.
			m_overflow = true;
			m_code = 0;
			m_bits = 0;
		}
		return -1;
	}

	if (m_zeros < 2)
	{
		++m_zeros;
	}
	if (m_zeros < 2)
	{
		return -1;
	}
	m_overflow = false;
	if (m_bits == 0)
	{
		return -1;
	}
	const int c = Varicode::Decode(m_code);
	m_code = 0;
	m_bits = 0;
	return c;
}

void VaricodeDecoder::Reset()
{
	m_code = 0;
	m_bits = 0;
	m_zeros = 2;
	m_overflow = false;
}

void VaricodeDecoder::Resync()
{
	m_code = 0;
	m_bits = 0;
	if (m_zeros < 2)
	{
		m_overflow = true;
	}
}

} // namespace psk
