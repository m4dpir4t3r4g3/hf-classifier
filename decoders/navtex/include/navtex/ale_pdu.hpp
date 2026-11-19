// The PDUs of 3G ALE (MIL-STD-188-141B appendix C: the 26-bit 3G-ALE PDUs of
// figure C-17 carried by BW0, the 48-bit traffic management PDUs of table
// C-XXXVI carried by BW1) and 4G ALE (MIL-STD-188-141D appendix G: the 96-bit
// WALE PDUs of figures G-13 to G-34): building them (for the test signals),
// checking their CRCs and describing them in one line.
//
// 3G PDUs are bit vectors, first bit sent first (fields MSB first, C.4.10).
// WALE PDUs are 12 octets, octet 0 first, each sent bit 0 first (G.5.2).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace navtex::ale {

using Bits = std::vector<std::uint8_t>;
using WalePdu = std::array<std::uint8_t, 12>;

// --- 3G-ALE (BW0)
Bits leCall(int calledMember, int callType, int callerMember, int callerGroup);
Bits leHandshake(int linkId, int command, int argument);
Bits leNotification(int callerStatus, int callerMember, int callerGroup);
Bits leTimeOffset(int timeQuality, bool early, int offsetCode);
Bits leGroupTimeBroadcast(int serverGroup, int dwell, int slot);
Bits leBroadcast(int countdown, int callType, int channel);
Bits leScanningCall(int calledAddress);
// The link ID of C.5.2.2.2 from the 11-bit caller and responder addresses.
int leLinkId(int callerAddress, int responderAddress);
// Whether the CRC of a 26-bit PDU is right (unknown PDU types: false).
bool leCrcOk(const Bits& pdu);
std::string describeLe(const Bits& pdu);

// --- 3G traffic management (BW1)
Bits tmPdu(int priority, int destAddress, int sourceAddress, int type, int argument);
bool tmCrcOk(const Bits& pdu);
std::string describeTm(const Bits& pdu);

// --- 4G WALE
// Link setup PDUs (protocol 010): type 0 LSU_Req, 1 LSU_Conf, 2 LSU_Term,
// 4 LSU_Req1, 7 LSU_Status. `field` is the 6-bit Traffic Type, SNR, Reason or
// Status field; vector1/2 the two sub-channel vectors (or the Status PDU's count).
WalePdu waleLinkSetup(int type, bool override, bool more, int field, int ec, int caller, int called, int vector1,
                      int vector2);
// A Text Message PDU (protocol 000) carrying up to 8 characters.
WalePdu waleText(const std::string& text8, int countdown, bool som, bool eom);
// Splits a text into Text Message PDUs.
std::vector<WalePdu> waleTextMessage(const std::string& text);
// Fills in octets 10 and 11.
void waleSetCrc(WalePdu& pdu);
bool waleCrcOk(const WalePdu& pdu);
std::string describeWale(const WalePdu& pdu);
// Whether more PDUs follow this one in the transmission (M bit, or a message
// not yet at its end).
bool waleMoreFollow(const WalePdu& pdu);
// Text Message PDUs: the characters (parity errors shown as '~', padding removed).
bool waleIsText(const WalePdu& pdu);
std::string waleTextOf(const WalePdu& pdu, bool& som, bool& eom);

}  // namespace navtex::ale
