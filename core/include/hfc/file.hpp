// Files by UTF-8 path, on every platform. On Windows, fopen() takes the
// path in the ANSI code page, so a folder such as C:\Users\Иван\... (a
// Cyrillic user name) cannot be opened with it; these use the wide API there.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace hfc {

std::FILE* openFile(const std::string& utf8Path, const char* mode);
bool makeDirectory(const std::string& utf8Path);  // true if it exists afterwards
bool fileExists(const std::string& utf8Path);
// The program's arguments in UTF-8 (on Windows, argv is in the ANSI code
// page: read again from the wide command line). Without argv[0].
std::vector<std::string> utf8Arguments(int argc, char** argv);

#ifdef _WIN32
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
#endif

}  // namespace hfc
