#include "hfc/json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hfc {

namespace {

class Reader {
 public:
  explicit Reader(const std::string& s) : s_(s) {}

  void skipSpace() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }
  bool eat(char c) {
    skipSpace();
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  bool atEnd() {
    skipSpace();
    return i_ >= s_.size();
  }
  char peek() {
    skipSpace();
    return i_ < s_.size() ? s_[i_] : '\0';
  }

  bool string(std::string& out) {
    out.clear();
    if (!eat('"')) return fail("expected a string");
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) break;
      const char e = s_[i_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return fail("bad \\u escape");
          if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
            uint32_t lo;
            if (i_ + 1 >= s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return fail("lone surrogate");
            i_ += 2;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default:
          return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }

  // Any value: strings are unescaped, everything else is kept as JSON text.
  bool value(std::string& out, bool& isString) {
    const char c = peek();
    if (c == '"') {
      isString = true;
      return string(out);
    }
    isString = false;
    const std::size_t start = i_;
    if (!skipValue()) return false;
    out = s_.substr(start, i_ - start);
    return true;
  }

  const std::string& error() const { return err_; }
  bool fail(const char* what) {
    if (err_.empty()) err_ = std::string(what) + " at offset " + std::to_string(i_);
    return false;
  }

 private:
  bool hex4(uint32_t& v) {
    if (i_ + 4 > s_.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      const char h = s_[i_++];
      v <<= 4;
      if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
      else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
      else return false;
    }
    return true;
  }

  static void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool skipValue(int depth = 0) {
    if (depth > 64) return fail("nesting too deep");
    const char c = peek();
    if (c == '"') {
      std::string tmp;
      return string(tmp);
    }
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      ++i_;
      if (eat(close)) return true;
      for (;;) {
        if (c == '{') {
          std::string k;
          if (!string(k) || !eat(':')) return fail("bad object member");
        }
        if (!skipValue(depth + 1)) return false;
        if (eat(close)) return true;
        if (!eat(',')) return fail("expected ',' or the end of a container");
      }
    }
    for (const char* lit : {"true", "false", "null"}) {
      const std::string l(lit);
      if (s_.compare(i_, l.size(), l) == 0) {
        i_ += l.size();
        return true;
      }
    }
    // Number: -?digits[.digits][e[+-]digits]
    const std::size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    std::size_t digits = 0;
    while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_, ++digits;
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    if (digits == 0) {
      i_ = start;
      return fail("expected a value");
    }
    return true;
  }

  const std::string& s_;
  std::size_t i_ = 0;
  std::string err_;
};

}  // namespace

bool parseJsonObject(const std::string& text, std::vector<JsonField>& fields, std::string* error) {
  fields.clear();
  Reader r(text);
  bool ok = r.eat('{');
  if (!ok) r.fail("expected '{'");
  if (ok && !r.eat('}')) {
    for (;;) {
      JsonField f;
      if (!r.string(f.key) || !r.eat(':') || !r.value(f.value, f.isString)) {
        ok = r.fail("bad member");
        break;
      }
      fields.push_back(std::move(f));
      if (r.eat('}')) break;
      if (!r.eat(',')) {
        ok = r.fail("expected ',' or '}'");
        break;
      }
    }
  }
  if (ok && !r.atEnd()) ok = r.fail("text after the object");
  if (!ok && error) *error = r.error();
  return ok;
}

bool parseJsonArray(const std::string& text, std::vector<JsonField>& elements, std::string* error) {
  elements.clear();
  Reader r(text);
  bool ok = r.eat('[');
  if (!ok) r.fail("expected '['");
  if (ok && !r.eat(']')) {
    for (;;) {
      JsonField f;
      if (!r.value(f.value, f.isString)) {
        ok = false;
        break;
      }
      elements.push_back(std::move(f));
      if (r.eat(']')) break;
      if (!r.eat(',')) {
        ok = r.fail("expected ',' or ']'");
        break;
      }
    }
  }
  if (ok && !r.atEnd()) ok = r.fail("text after the array");
  if (!ok && error) *error = r.error();
  return ok;
}

std::string jsonField(const std::vector<JsonField>& fields, const std::string& key) {
  for (const auto& f : fields)
    if (f.key == key) return f.value;
  return std::string();
}

std::string jsonQuote(const std::string& text) {
  std::string out = "\"";
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out + "\"";
}

void JsonWriter::newline() {
  out_ += '\n';
  out_.append(first_.size() * 2, ' ');
}

void JsonWriter::beforeValue() {
  if (afterKey_) {
    afterKey_ = false;
    return;
  }
  if (!first_.empty()) {
    if (!first_.back()) out_ += ',';
    first_.back() = false;
    newline();
  }
}

JsonWriter& JsonWriter::beginObject() {
  beforeValue();
  out_ += '{';
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::endObject() {
  const bool empty = first_.back();
  first_.pop_back();
  if (!empty) newline();
  out_ += '}';
  return *this;
}

JsonWriter& JsonWriter::beginArray() {
  beforeValue();
  out_ += '[';
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::endArray() {
  const bool empty = first_.back();
  first_.pop_back();
  if (!empty) newline();
  out_ += ']';
  return *this;
}

JsonWriter& JsonWriter::key(const std::string& k) {
  beforeValue();
  out_ += jsonQuote(k) + ": ";
  afterKey_ = true;
  return *this;
}

JsonWriter& JsonWriter::value(const std::string& s) {
  beforeValue();
  out_ += jsonQuote(s);
  return *this;
}

JsonWriter& JsonWriter::raw(const std::string& json) {
  beforeValue();
  out_ += json;
  return *this;
}

JsonWriter& JsonWriter::value(double v) {
  beforeValue();
  if (!std::isfinite(v)) {
    out_ += "null";
    return *this;
  }
  char buf[32];
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    out_ += buf;
    return *this;
  }
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  // %.17g can print 0.10000000000000001; use the shortest form that reads back.
  for (int prec = 1; prec <= 17; ++prec) {
    char b2[32];
    std::snprintf(b2, sizeof(b2), "%.*g", prec, v);
    if (std::strtod(b2, nullptr) == v) {
      out_ += b2;
      return *this;
    }
  }
  out_ += buf;
  return *this;
}

JsonWriter& JsonWriter::value(int64_t v) {
  beforeValue();
  out_ += std::to_string(v);
  return *this;
}

JsonWriter& JsonWriter::value(uint64_t v) {
  beforeValue();
  out_ += std::to_string(v);
  return *this;
}

JsonWriter& JsonWriter::value(bool v) {
  beforeValue();
  out_ += v ? "true" : "false";
  return *this;
}

}  // namespace hfc
