#include "navtex/ale_pdu.hpp"

#include <cstdarg>
#include <cstdio>

#include "navtex/ale_codes.hpp"

namespace navtex::ale {

namespace {

void put(Bits& b, unsigned value, int n) {
  for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>((value >> i) & 1));
}

unsigned get(const Bits& b, int pos, int n) {
  unsigned v = 0;
  for (int i = 0; i < n; ++i) v = (v << 1) | (b[static_cast<std::size_t>(pos + i)] & 1);
  return v;
}

constexpr std::uint32_t kCrc4 = 0xB;    // x^4 + x^3 + x + 1
constexpr std::uint32_t kCrc8 = 0x9B;   // x^8 + x^7 + x^4 + x^3 + x + 1
constexpr std::uint32_t kCrc12 = 0xBCF; // x^12 + x^11 + x^9 + x^8 + x^7 + x^6 + x^3 + x^2 + x + 1

Bits withCrc(Bits b, int crcBits) {
  std::uint32_t poly = crcBits == 4 ? kCrc4 : crcBits == 8 ? kCrc8 : kCrc12;
  put(b, crcAppendixC(b.data(), static_cast<int>(b.size()), poly, crcBits), crcBits);
  return b;
}

bool crcMatches(const Bits& b, int crcBits) {
  int n = static_cast<int>(b.size()) - crcBits;
  std::uint32_t poly = crcBits == 4 ? kCrc4 : crcBits == 8 ? kCrc8 : kCrc12;
  return crcAppendixC(b.data(), n, poly, crcBits) == get(b, n, crcBits);
}

std::string fmt(const char* f, ...) {
  char buf[320];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

const char* callType(unsigned t) {
  static const char* const k[8] = {"3G ARQ packet data", "HF modem circuit", "analog voice circuit",
                                   "high-quality circuit", "unicast ARQ packet", "unicast circuit",
                                   "multicast circuit", "control"};
  return k[t & 7];
}

const char* leReason(unsigned r) {
  switch (r) {
    case 0: return "NO_RESPONSE";
    case 1: return "REJECTED";
    case 2: return "NO_TRAF_CHAN";
    case 3: return "LOW_QUALITY";
    default: return nullptr;
  }
}

std::string addr3g(unsigned a) { return fmt("%u/%u", (a >> 5) & 63, a & 31); }

const char* tmTraffic(unsigned t) {
  static const char* const k[17] = {"HDL_24", "HDL_12", "HDL_6", "HDL_3", "LDL_512", "LDL_256",
                                    "LDL_128", "LDL_64", "ANDVT", "DGTL_VOICE", "ANLG_VOICE", "SER_110",
                                    "HQ_9600", "HQ_6400", "HQ_4800", "HQ_3200", "SER_4285"};
  if (t < 17) return k[t];
  return t == 61 ? "PKT" : t == 62 ? "CKT" : t == 63 ? "NO_TR" : "reserved";
}

const char* tmReason(unsigned r) {
  static const char* const k[7] = {"ABORT", "RELINK", "SIGN_OFF", "UNLINK", "UNL_ACK", "UNL_NAK", "SUSPEND"};
  return r < 7 ? k[r] : "reserved";
}

const char* waleTraffic(unsigned t) {
  switch (t) {
    case 0: return "no traffic";
    case 1: return "analog voice";
    case 2: return "low-latency 600 bps";
    case 3: return "low-latency 1200 bps";
    case 4: return "low-latency 2400 bps";
    case 5: return "best throughput";
    case 6: return "low error rate";
    case 16: return "STANAG 4415";
    case 17: return "STANAG 4539";
    case 18: return "STANAG 5069 wideband";
    case 24: return "adaptive >= 19.2 kbps";
    case 25: return "adaptive >= 38.4 kbps";
    case 26: return "adaptive >= 50 kbps";
    case 27: return "adaptive >= 64 kbps";
    case 61: return "sync check";
    case 62: return "LQA exchange";
    case 63: return "time of day";
    default: return t >= 51 && t <= 60 ? "vendor" : "reserved";
  }
}

const char* waleEc(unsigned ec) {
  static const char* const k[4] = {"3 kHz", "up to 12 kHz", "up to 24 kHz", "up to 48 kHz"};
  return k[ec & 3];
}

const char* waleReason(unsigned r) {
  switch (r) {
    case 0: return "NO_MORE_TRAFFIC";
    case 1: return "NO_RESPONSE";
    case 2: return "NOT_EQUIPPED";
    case 3: return "RELINK";
    case 4: return "PREEMPTED";
    default: return r >= 32 ? "vendor" : "reserved";
  }
}

const char* waleStatus(unsigned s) {
  switch (s) {
    case 0: return "off";
    case 1: return "receive only";
    case 3: return "normal";
    default: return s >= 32 ? "vendor" : "reserved";
  }
}

std::uint8_t oddParity(char c) {
  std::uint8_t v = static_cast<std::uint8_t>(c & 0x7f);
  return parity(v) ? v : static_cast<std::uint8_t>(v | 0x80);
}

}  // namespace

// ------------------------------------------------------------- 3G-ALE

Bits leCall(int calledMember, int type, int callerMember, int callerGroup) {
  Bits b;
  put(b, 2, 2);
  put(b, static_cast<unsigned>(calledMember), 6);
  put(b, static_cast<unsigned>(type), 3);
  put(b, static_cast<unsigned>(callerMember), 6);
  put(b, static_cast<unsigned>(callerGroup), 5);
  return withCrc(b, 4);
}

Bits leHandshake(int linkId, int command, int argument) {
  Bits b;
  put(b, 0, 2);
  put(b, static_cast<unsigned>(linkId), 6);
  put(b, static_cast<unsigned>(command), 3);
  put(b, static_cast<unsigned>(argument), 7);
  return withCrc(b, 8);
}

Bits leNotification(int status, int callerMember, int callerGroup) {
  Bits b;
  put(b, 2, 2);
  put(b, 63, 6);
  put(b, static_cast<unsigned>(status), 3);
  put(b, static_cast<unsigned>(callerMember), 6);
  put(b, static_cast<unsigned>(callerGroup), 5);
  return withCrc(b, 4);
}

Bits leTimeOffset(int quality, bool early, int offset) {
  Bits b;
  put(b, 1, 2);
  put(b, 4, 3);
  put(b, static_cast<unsigned>(quality), 3);
  put(b, early ? 1u : 0u, 1);
  put(b, static_cast<unsigned>(offset), 9);
  return withCrc(b, 8);
}

Bits leGroupTimeBroadcast(int group, int dwell, int slot) {
  Bits b;
  put(b, 1, 2);
  put(b, 5, 3);
  put(b, 0, 1);
  put(b, static_cast<unsigned>(group), 5);
  put(b, static_cast<unsigned>(dwell), 4);
  put(b, static_cast<unsigned>(slot), 3);
  return withCrc(b, 8);
}

Bits leBroadcast(int countdown, int type, int channel) {
  Bits b;
  put(b, 1, 2);
  put(b, 6, 3);
  put(b, static_cast<unsigned>(countdown), 3);
  put(b, static_cast<unsigned>(type), 3);
  put(b, static_cast<unsigned>(channel), 7);
  return withCrc(b, 8);
}

Bits leScanningCall(int address) {
  Bits b;
  put(b, 1, 2);
  put(b, 7, 3);
  put(b, 3, 2);
  put(b, static_cast<unsigned>(address), 11);
  return withCrc(b, 8);
}

int leLinkId(int caller, int responder) {
  std::uint32_t t1 = static_cast<std::uint32_t>(caller) * 0x13C6EFu;
  std::uint32_t t2 = static_cast<std::uint32_t>(responder) * 0x13C6EFu;
  return static_cast<int>(((t1 >> 4) + (t2 >> 15)) & 0x3f);
}

bool leCrcOk(const Bits& b) {
  if (b.size() != 26) return false;
  switch (get(b, 0, 2)) {
    case 2: return crcMatches(b, 4);
    case 0:
    case 1: return crcMatches(b, 8);
    default: return false;
  }
}

std::string describeLe(const Bits& b) {
  if (b.size() != 26) return "?";
  const unsigned kind = get(b, 0, 2);
  if (kind == 2) {
    unsigned called = get(b, 2, 6), f3 = get(b, 8, 3), member = get(b, 11, 6), group = get(b, 17, 5);
    unsigned caller = (member << 5) | group;
    if (called == 63) {
      static const char* const kStatus[8] = {"nominal", "time server", "reserved", "reserved",
                                             "reserved", "reserved", "commencing EMCON", "leaving network"};
      return fmt("LE_Notification  from %s  status: %s", addr3g(caller).c_str(), kStatus[f3]);
    }
    return fmt("LE_Call  to member %u  from %s  %s%s", called, addr3g(caller).c_str(), callType(f3),
               called >= 60 ? "  (net entry member number)" : "");
  }
  if (kind == 0) {
    unsigned link = get(b, 2, 6), cmd = get(b, 8, 3), arg = get(b, 11, 7);
    switch (cmd) {
      case 0:
      case 7: {
        const char* r = leReason(arg);
        return fmt("LE_Handshake  link %02X  %s  reason: %s", link, cmd == 0 ? "continue handshake" : "abort handshake",
                   r ? r : "reserved");
      }
      case 1: return fmt("LE_Handshake  link %02X  commence traffic setup  channel %u", link, arg);
      case 2: return fmt("LE_Handshake  link %02X  voice traffic  channel %u", link, arg);
      case 3: return fmt("LE_Handshake  link %02X  link release  channel %u", link, arg);
      case 4:
        return fmt("LE_Handshake  link %02X  sync check  time quality %u, slot %u", link, (arg >> 3) & 7, arg & 7);
      case 6: return fmt("LE_Handshake  link %02X  data %02X", link, arg);
      default: return fmt("LE_Handshake  link %02X  command %u (reserved)  argument %u", link, cmd, arg);
    }
  }
  if (kind == 1) {
    unsigned sub = get(b, 2, 3);
    switch (sub) {
      case 4: {
        unsigned q = get(b, 5, 3), sign = get(b, 8, 1), code = get(b, 9, 9);
        std::string ms = code <= 400 ? fmt("%u ms", 2 * code) : code > 420 ? fmt("%u ms", 40 * (code - 400)) : "reserved";
        return fmt("LE_Time_Offset  time quality %u  %s %s", q, ms.c_str(), sign ? "early" : "late");
      }
      case 5:
        return fmt("LE_Group_Time_Broadcast  server group %u  dwell %u  slot %u", get(b, 6, 5), get(b, 11, 4),
                   get(b, 15, 3));
      case 6:
        return fmt("LE_Broadcast  countdown %u  %s  channel %u", get(b, 5, 3), callType(get(b, 8, 3)), get(b, 11, 7));
      case 7: return fmt("LE_Scanning_Call  to %s", addr3g(get(b, 7, 11)).c_str());
      default: return fmt("3G-ALE PDU 01%u%u%u (reserved)", (sub >> 2) & 1, (sub >> 1) & 1, sub & 1);
    }
  }
  return "3G-ALE PDU 11 (reserved)";
}

// ------------------------------------------------------------- 3G TM

Bits tmPdu(int priority, int dest, int source, int type, int argument) {
  Bits b;
  put(b, 1, 3);
  put(b, static_cast<unsigned>(priority), 2);
  put(b, static_cast<unsigned>(dest), 11);
  put(b, static_cast<unsigned>(source), 11);
  put(b, static_cast<unsigned>(type), 3);
  put(b, static_cast<unsigned>(argument), 6);
  return withCrc(b, 12);
}

bool tmCrcOk(const Bits& b) { return b.size() == 48 && get(b, 0, 3) == 1 && crcMatches(b, 12); }

std::string describeTm(const Bits& b) {
  if (b.size() != 48) return "?";
  static const char* const kPriority[4] = {"highest", "high", "routine", "low"};
  unsigned prio = get(b, 3, 2), dest = get(b, 5, 11), src = get(b, 16, 11), type = get(b, 27, 3), arg = get(b, 30, 6);
  std::string addrs = fmt("from %s to %s", addr3g(src).c_str(), dest == 0x7ff ? "all" : addr3g(dest).c_str());
  switch (type) {
    case 0: return fmt("TM_REQUEST  %s  traffic %s  priority %s", addrs.c_str(), tmTraffic(arg), kPriority[prio]);
    case 1: return fmt("TM_CONFIRM  %s  traffic %s", addrs.c_str(), tmTraffic(arg));
    case 2: return fmt("TM_TERM  %s  reason %s", addrs.c_str(), tmReason(arg));
    default: return fmt("TM PDU type %u (reserved)  %s", type, addrs.c_str());
  }
}

// ------------------------------------------------------------- 4G WALE

WalePdu waleLinkSetup(int type, bool override, bool more, int field, int ec, int caller, int called, int v1, int v2) {
  WalePdu p{};
  p[0] = static_cast<std::uint8_t>((2 << 5) | ((type & 7) << 2) | (override ? 2 : 0) | (more ? 1 : 0));
  p[1] = static_cast<std::uint8_t>(((field & 63) << 2) | (ec & 3));
  p[2] = static_cast<std::uint8_t>(caller >> 8);
  p[3] = static_cast<std::uint8_t>(caller);
  p[4] = static_cast<std::uint8_t>(called >> 8);
  p[5] = static_cast<std::uint8_t>(called);
  p[6] = static_cast<std::uint8_t>(v1 >> 8);
  p[7] = static_cast<std::uint8_t>(v1);
  p[8] = static_cast<std::uint8_t>(v2 >> 8);
  p[9] = static_cast<std::uint8_t>(v2);
  waleSetCrc(p);
  return p;
}

WalePdu waleText(const std::string& text, int countdown, bool som, bool eom) {
  WalePdu p{};
  int used = static_cast<int>(text.size() < 8 ? text.size() : 8);
  int padding = eom ? 8 - used : 0;
  p[0] = static_cast<std::uint8_t>(((padding & 7) << 2) | (som ? 2 : 0) | (eom ? 1 : 0));
  p[1] = static_cast<std::uint8_t>(countdown);
  for (int i = 0; i < 8; ++i) p[static_cast<std::size_t>(2 + i)] = oddParity(i < used ? text[static_cast<std::size_t>(i)] : '\0');
  waleSetCrc(p);
  return p;
}

std::vector<WalePdu> waleTextMessage(const std::string& text) {
  std::vector<WalePdu> out;
  const std::size_t n = text.empty() ? 1 : (text.size() + 7) / 8;
  for (std::size_t i = 0; i < n; ++i)
    out.push_back(waleText(i * 8 < text.size() ? text.substr(i * 8, 8) : std::string(), static_cast<int>(n - 1 - i),
                           i == 0, i + 1 == n));
  return out;
}

void waleSetCrc(WalePdu& p) {
  std::uint16_t crc = crc16Wale(p.data(), 10);
  p[10] = static_cast<std::uint8_t>(crc & 0xff);
  p[11] = static_cast<std::uint8_t>(crc >> 8);
}

bool waleCrcOk(const WalePdu& p) {
  std::uint16_t crc = crc16Wale(p.data(), 10);
  return p[10] == (crc & 0xff) && p[11] == (crc >> 8);
}

bool waleIsText(const WalePdu& p) { return (p[0] >> 5) == 0; }

bool waleMoreFollow(const WalePdu& p) {
  unsigned protocol = p[0] >> 5;
  if (protocol == 0 || protocol == 1) return !(p[0] & 1);  // message: not yet EOM
  return p[0] & 1;                                          // M bit
}

std::string waleTextOf(const WalePdu& p, bool& som, bool& eom) {
  som = p[0] & 2;
  eom = p[0] & 1;
  int padding = eom ? (p[0] >> 2) & 7 : 0;
  std::string s;
  for (int i = 0; i < 8 - padding; ++i) {
    std::uint8_t o = p[static_cast<std::size_t>(2 + i)];
    char c = static_cast<char>(o & 0x7f);
    if (!parity(o)) c = '~';                 // even parity: error
    else if (c == '\0') continue;            // NUL padding
    else if (c < 0x20 || c == 0x7f) c = c == '\n' || c == '\r' ? ' ' : '.';
    s += c;
  }
  return s;
}

std::string describeWale(const WalePdu& p) {
  const unsigned protocol = p[0] >> 5, type = (p[0] >> 2) & 7;
  const bool v = p[0] & 2, m = p[0] & 1;
  const unsigned field = p[1] >> 2, ec = p[1] & 3;
  const unsigned a1 = (p[2] << 8) | p[3], a2 = (p[4] << 8) | p[5];
  const unsigned v1 = (p[6] << 8) | p[7], v2 = (p[8] << 8) | p[9];
  std::string flags = std::string(v ? "  override" : "") + (m ? "  (more follows)" : "");
  if (protocol == 2) {
    switch (type) {
      case 0:
        return fmt("LSU_Req  %04X -> %04X  traffic: %s  EC %s  assigned %04X  occupied %04X%s", a1, a2,
                   waleTraffic(field), waleEc(ec), v1, v2, flags.c_str());
      case 1:
        return fmt("LSU_Conf  %04X -> %04X  SNR %d dB  EC %s  Tx %04X  Rx %04X%s", a1, a2, static_cast<int>(field) - 10,
                   waleEc(ec), v1, v2, flags.c_str());
      case 2: return fmt("LSU_Term  %04X -> %04X  reason: %s%s", a1, a2, waleReason(field), flags.c_str());
      case 4:
        return fmt("LSU_Req1 (one-way)  %04X -> %04X  traffic: %s  EC %s  Tx %04X%s", a1, a2, waleTraffic(field),
                   waleEc(ec), v1, flags.c_str());
      case 7:
        return fmt("LSU_Status  from %04X  status: %s  count %u  EC %s  assigned %04X%s", a1, waleStatus(field), p[4],
                   waleEc(ec), v1, flags.c_str());
      default: return fmt("Link setup PDU type %u (reserved)  %04X -> %04X%s", type, a1, a2, flags.c_str());
    }
  }
  if (protocol == 0) {
    bool som, eom;
    std::string t = waleTextOf(p, som, eom);
    return fmt("Text message PDU %u  \"%s\"", p[1], t.c_str());
  }
  if (protocol == 1)
    return fmt("Binary message PDU %u  %02X %02X %02X %02X %02X %02X %02X %02X", p[1], p[2], p[3], p[4], p[5], p[6],
               p[7], p[8], p[9]);
  if (protocol == 3) {
    switch (type) {
      case 0: return fmt("Message header  %04X -> %04X  purpose %02X%02X", a1, a2, p[6], p[7]);
      case 5: return fmt("TOD_Resp  %04X -> %04X  time %02u:%02u (min:s)%s", a1, a2, p[8] & 63, p[9] & 63, flags.c_str());
      case 6: return fmt("Fill_Req  from %04X  assigned %04X  occupied %04X%s", a1, v1, v2, flags.c_str());
      case 7: return fmt("Fill_Resp  %04X -> %04X  SNR %d dB  Tx %04X  Rx %04X%s", a1, a2, static_cast<int>(field) - 10,
                         v1, v2, flags.c_str());
      default: return fmt("Utility PDU type %u  %04X -> %04X%s", type, a1, a2, flags.c_str());
    }
  }
  return fmt("PDU protocol %u (reserved)", protocol);
}

}  // namespace navtex::ale
