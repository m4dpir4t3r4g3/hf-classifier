#pragma once

#include <cstddef>
#include <string>

namespace psk {

// Turns decoded bytes into Unicode text. PSK programs send either UTF-8
// (fldigi and most modern software) or, in older software, an 8-bit code
// page; for Cyrillic that is almost always Windows-1251. Bytes are decoded
// as UTF-8, and any byte that cannot be part of a valid UTF-8 sequence is
// decoded as Windows-1251 instead.
class TextDecoder
{
public:
	// Feeds one byte; appends any completed characters to `out`.
	void Push(unsigned char byte, std::u32string& out);
	// Emits a pending, incomplete UTF-8 sequence as Windows-1251 (call when
	// no bytes have arrived for a while).
	void Flush(std::u32string& out);
	bool Pending() const { return m_count > 0; }
	void Reset() { m_count = 0; }

	static char32_t FromWindows1251(unsigned char byte);

private:
	unsigned char m_bytes[4] = {};
	size_t m_count = 0;
	size_t m_expected = 0;
};

} // namespace psk
