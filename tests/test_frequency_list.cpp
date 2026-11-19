// Frequency lists: the formats read, units, the modes labels name, lookup.
#include <cmath>
#include <string>

#include "hfc/frequency_list.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {
const ListedFrequency* at(const std::vector<ListedFrequency>& v, double hz) {
  for (const auto& e : v)
    if (std::fabs(e.hz - hz) < 0.5) return &e;
  return nullptr;
}
}  // namespace

TEST(frequency_list_reads_text_and_json) {
  FrequencyList l;
  // Lines: spaces, commas, semicolons, tabs, quotes. One unit per list, by
  // its largest number: kHz here, then Hz, then MHz.
  CHECK(l.addText("# a comment\nkHz;station;mode\n"
                  "4583 DDK2 Hamburg RTTY 50 Bd 450 Hz\n"
                  "10100.8,\"DDK9\",\"RTTY 50/450\"\n"
                  "\n// another comment\nno number here\n",
                  "a.txt") == 2);
  CHECK(l.addText("14074000;FT8\n", "hz.csv") == 1);
  // A comma in the text after the frequency: still read.
  FrequencyList free;
  CHECK(free.addText("8390.0 STANAG 4285 net (demo entry, synthetic)\n", "x") == 1);
  CHECK(free.lookup(8389900, 8390100).size() == 1 && free.lookup(8389900, 8390100)[0].mode == "STANAG 4285");
  CHECK(l.addText("7.646\tDDH7 RTTY\n", "mhz.tsv") == 1);
  // In a kHz list, 17.2 is 17.2 kHz.
  FrequencyList vlf;
  CHECK(vlf.addText("[[17.2, \"CWN\", \"SAQ SWE\"], [518, \"CW\", \"NAVTEX\"]]", "dx.json") == 2);
  CHECK(at(vlf.lookup(0, 1e6, 10), 17200) && at(vlf.lookup(0, 1e6, 10), 518000));
  CHECK(at(vlf.lookup(0, 1e6, 10), 17200)->mode.empty());  // "CWN" there is the receiver's mode, not Morse
  auto v = l.lookup(0, 30e6, 100);
  CHECK(v.size() == 4);
  const ListedFrequency* e = at(v, 4583000);
  CHECK(e && e->label == "DDK2 Hamburg RTTY 50 Bd 450 Hz" && e->mode == "RTTY (other rates and shifts)" && e->baud == 50 &&
        e->shiftHz == 450 && e->source == "a.txt");
  e = at(v, 10100800);
  CHECK(e && e->label == "DDK9 RTTY 50/450" && e->baud == 50 && e->shiftHz == 450);
  CHECK(at(v, 14074000) && at(v, 14074000)->mode == "FT8");
  CHECK(at(v, 7646000));
  // JSON: objects with a frequency (as OpenWebRX's bookmarks), and arrays
  // starting with one (as KiwiSDR's labels), at any depth.
  FrequencyList j;
  CHECK(j.addText("[{\"name\": \"STANAG 4285 net\", \"frequency\": 8453000, \"modulation\": \"usb\"},"
                  " {\"name\": \"no frequency\"}]",
                  "bookmarks.json") == 1);
  CHECK(j.addText("{\"dx\": [[4209.5, \"CW\", \"NAVTEX\", \"HF NAVTEX\", {\"T1\": 1}], [6312, \"USB\", \"DSC\"]]}", "dx.json") == 2);
  v = j.lookup(0, 30e6, 100);
  CHECK(at(v, 8453000) && at(v, 8453000)->mode == "STANAG 4285" && at(v, 8453000)->label == "STANAG 4285 net");
  CHECK(at(v, 4209500) && at(v, 4209500)->mode == "NAVTEX / SITOR-B" && at(v, 4209500)->label == "NAVTEX HF NAVTEX");
  CHECK(at(v, 6312000) && at(v, 6312000)->mode == "DSC-HF");
  // Nothing usable.
  FrequencyList none;
  CHECK(none.addText("", "x") == 0 && none.addText("{\"a\": [", "x") == 0 && none.addText("hello\nworld", "x") == 0);
  CHECK(none.addFile("/no/such/file") == 0);
}

TEST(frequency_list_names_catalogued_modes) {
  double baud = 0, shift = 0;
  CHECK(FrequencyList::modeOf("NATO STANAG-4285 600L") == "STANAG 4285");
  CHECK(FrequencyList::modeOf("MIL-STD-188-110A serial") == "MIL-STD-188-110 serial / STANAG 4415");
  CHECK(FrequencyList::modeOf("USAF HFGCS ALE") == "ALE 2G");
  CHECK(FrequencyList::modeOf("4G WALE") == "ALE 3G / 4G (WALE)");
  CHECK(FrequencyList::modeOf("female voice, numbers") == "");  // "ale" inside a word
  CHECK(FrequencyList::modeOf("Shannon VOLMET") == "");
  CHECK(FrequencyList::modeOf("HFDL Shannon") == "HFDL");
  CHECK(FrequencyList::modeOf("CIS Navy T-600 50 Bd/200", &baud, &shift) == "CIS-36-50 (T-600)" && baud == 50 && shift == 200);
  CHECK(FrequencyList::modeOf("RTTY 45.45/170", &baud, &shift) == "RTTY (other rates and shifts)" && baud == 45.45 && shift == 170);
  CHECK(FrequencyList::modeOf("Beacon CW") == "CW");
  CHECK(FrequencyList::modeOf("GMDSS/DSC") == "DSC-HF");
  CHECK(FrequencyList::modeOf("Akula burst FSK 500Bd 1000Hz", &baud, &shift) == "Akula" && baud == 500 && shift == 1000);
}

TEST(frequency_list_looks_up_an_emissions_band) {
  auto l = FrequencyList::builtin();
  CHECK(l->size() == 9);
  // Inside the band (150 Hz of slack).
  auto v = l->lookup(8414400, 8414600);
  CHECK(v.size() == 1 && v[0].hz == 8414500 && v[0].mode == "DSC-HF" && v[0].source == "ITU");
  CHECK(l->lookup(8414300, 8414400).size() == 1 && l->lookup(8414000, 8414300).empty());
  // A dial frequency up to 2.5 kHz below the band; not above it.
  CHECK(l->lookup(8415500, 8416500).size() == 1 && l->lookup(8417100, 8418000).empty());
  CHECK(l->lookup(8412000, 8414000).empty());
  // The nearest first.
  CHECK(l->addText("8415.0 Something else\n8414.6 A third", "b") == 2);
  v = l->lookup(8414900, 8415100, 2);
  CHECK(v.size() == 2 && v[0].label == "Something else" && v[1].label == "A third");
}

TEST(frequency_list_reads_eibi_schedules) {
  FrequencyList l;
  // EiBi's CSV: hours on the air, the country, and for utility stations
  // what they send, in the language column.
  const std::size_t n = l.addText("kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\n"
                                  "147.3;0000-2400;;D;DDH47 Pinneberg;-TY;Eu;;1;;\n"
                                  "5450;0000-2400;;G;RAF Volmet;E;Eu;;1;;\n"
                                  "6532;0000-2400;;USA;Riverhead NY;-HF;NAm;;1;;\n"
                                  "8424;0000-2400;;GRC;SVO Olimpia Radio;-CW;Eu;;1;;\n"
                                  "9420;2200-0300;;GRC;Voice of Greece;GR;Eu;a;1;;\n"
                                  "9420;0600-0800;Mo-Fr;GRC;Voice of Greece;GR;Eu;a;1;;\n"
                                  "10000;0000-2400;;USA;WWV Fort Collins;-TS;NAm;;1;;\n",
                                  "sked-test.csv");
  CHECK(n == static_cast<std::size_t>(7));
  const auto& e = l.entries();
  const auto* cw = at(e, 8424000);
  CHECK(cw && cw->mode == "CW" && cw->demod == "CW" && cw->label == "SVO Olimpia Radio (GRC) Morse" && cw->startMin < 0);
  const auto* hf = at(e, 6532000);
  CHECK(hf && hf->mode == "HFDL" && hf->demod == "USB");
  const auto* ty = at(e, 147300);
  CHECK(ty && ty->demod == "USB" && ty->label.find("teletype") != std::string::npos);
  const auto* ts = at(e, 10000000);
  CHECK(ts && ts->mode == "Time signal" && ts->demod == "AM" && ts->label.find("time signal") != std::string::npos);
  const auto* bc = at(e, 5450000);
  CHECK(bc && bc->mode == "AM broadcast" && bc->demod == "AM");
  // Hours: 22:00 to 03:00 runs over midnight.
  const auto* vg = at(e, 9420000);
  CHECK(vg && vg->startMin == 22 * 60 && vg->stopMin == 3 * 60);
  const double day = 1700006400;  // 2023-11-15 00:00 UTC
  CHECK(vg->onAir(day + 23 * 3600) && vg->onAir(day + 2 * 3600) && !vg->onAir(day + 4 * 3600) && vg->onAir(-1));
  // lookup with a time leaves out who is off the air then.
  CHECK(l.lookup(9419000, 9421000, 5, day + 12 * 3600).size() == static_cast<std::size_t>(0));
  CHECK(l.lookup(9419000, 9421000, 5, day + 7 * 3600).size() == static_cast<std::size_t>(1));
  CHECK(l.lookup(9419000, 9421000, 5).size() == static_cast<std::size_t>(2));
  CHECK(l.sources().size() == static_cast<std::size_t>(1));
  CHECK(l.sources()[0].first == "sked-test.csv" && l.sources()[0].second == 7);
}

TEST(frequency_list_keeps_how_to_listen) {
  FrequencyList l;
  // KiwiSDR labels carry the demodulator; elsewhere the mode implies it.
  l.addText("[[4625, \"AM\", \"The Buzzer\"], [8992, \"USB\", \"HFGCS\"], [3756, \"LSB\", \"net\"], [518, \"CWN\", \"NAVTEX\"]]", "dx.json");
  l.addText("14230 SSTV calling\n7039 beacon cluster morse\n", "mine.txt");
  const auto& e = l.entries();
  CHECK(at(e, 4625000) && at(e, 4625000)->demod == "AM" && at(e, 4625000)->label == "The Buzzer");
  CHECK(at(e, 8992000) && at(e, 8992000)->demod == "USB");
  CHECK(at(e, 3756000) && at(e, 3756000)->demod == "LSB");
  CHECK(at(e, 518000) && at(e, 518000)->demod == "CW" && at(e, 518000)->mode == "NAVTEX / SITOR-B");
  CHECK(at(e, 7039000) && at(e, 7039000)->demod == "CW");
  CHECK(at(e, 14230000) && at(e, 14230000)->demod.empty());
  CHECK(l.sources().size() == static_cast<std::size_t>(2));
}

TEST(frequency_list_bundled_eibi) {
  FrequencyList l;
  const std::size_t n = l.addText(FrequencyList::bundledEibi(), FrequencyList::bundledEibiName());
  CHECK(n > 8000);
  std::size_t hfdl = 0, cw = 0, timed = 0;
  bool ascending = true;
  double last = 0;
  for (const auto& e : l.entries()) {
    hfdl += e.mode == "HFDL";
    cw += e.mode == "CW";
    timed += e.startMin >= 0;
    ascending = ascending && e.hz >= last;
    last = e.hz;
    // Labels are valid UTF-8 (the file is converted from Latin-1) and short.
    CHECK(e.label.size() <= 120 && !e.label.empty());
  }
  CHECK(ascending && hfdl > 100 && cw > 100 && timed > 5000);
}

TEST(frequency_list_reads_kiwisdr_community_labels) {
  // KiwiSDR's community list: kHz, the demodulator, ident, notes (with
  // "\n"), and the label type {"T8":1}; a VHF entry must not make the
  // whole list Hz.
  FrequencyList l;
  const std::size_t n = l.addText("{\"dx\":[\n"
                                  "[16.30, \"CW\", \"VTX1 IND\", \"MSK\", {\"T8\":1}],\n"
                                  "[4338.5, \"USB\", \"STANAG FUB\", \"French Navy\\nParis\", {\"T8\":1, \"lo\":-1500, \"hi\":1500}],\n"
                                  "[8416.0, \"USB\", \"DSC\", \"\", {\"T13\":1}],\n"
                                  "[10087.0, \"USB\", \"HFDL CA San Francisco\", \"\", {\"T7\":1}],\n"
                                  "[121500.0, \"AM\", \"Guard\", \"\", {\"T12\":1}],\n"
                                  "[6998.0, \"USB\", \"no type\", \"\"]\n"
                                  "]}",
                                  "dx_community.json");
  CHECK(n == 6);
  const auto& e = l.entries();
  const auto* vtx = at(e, 16300);
  CHECK(vtx && vtx->category == 8 && vtx->demod == "CW" && vtx->label == "VTX1 IND MSK");
  const auto* fub = at(e, 4338500);
  CHECK(fub && fub->category == 8 && fub->label == "STANAG FUB French Navy Paris");
  CHECK(at(e, 8416000) && at(e, 8416000)->category == 13 && at(e, 8416000)->mode == "DSC-HF");
  CHECK(at(e, 10087000) && at(e, 10087000)->mode == "HFDL" && at(e, 10087000)->category == 7);
  CHECK(at(e, 121500000) && at(e, 121500000)->category == 12);
  CHECK(at(e, 6998000) && at(e, 6998000)->category == -1);
}
