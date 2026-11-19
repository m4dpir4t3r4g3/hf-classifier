# Turns a file into a C++ source defining `const std::string& <FUNCTION>()`
# that returns its bytes. A byte array, not a string literal: MSVC limits
# string literals to 64 KB.
#   cmake -DINPUT=file -DOUTPUT=out.cpp -DFUNCTION=hfc::uiPage -P embed.cmake
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR bytes "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" body "${body}")
string(REGEX MATCH "^(.*)::([^:]+)$" _ "${FUNCTION}")
set(ns "${CMAKE_MATCH_1}")
set(fn "${CMAKE_MATCH_2}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT} by cmake/embed.cmake. Do not edit.
#include <string>
namespace ${ns} {
namespace {
const unsigned char kData[] = {
${body}
0};
}  // namespace
const std::string& ${fn}() {
  static const std::string s(reinterpret_cast<const char*>(kData), ${bytes});
  return s;
}
}  // namespace ${ns}
")
