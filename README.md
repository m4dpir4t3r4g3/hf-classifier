# SDRconnect HF classifier

An HF emission classifier and decoder for
[SDRconnect](https://www.sdrplay.com/sdrconnect/) with an SDRplay RSPduo. It
watches the IQ that SDRconnect streams, finds every emission in it, decodes
the modes that existing decoders handle (NAVTEX, SITOR-B, RTTY, ALE, Morse,
PSK, DSC, HFDL, radiofax, SSTV, time signals, FT8/FT4, WSPR) and classifies the rest (Russian/CIS, NATO and other military and
government HF modems), with measured parameters and a confidence for every
label. Passive reception only; no decryption.

**Version 0.6.0, a preview** ([CHANGELOG.md](CHANGELOG.md)). It has run
with a real SDRconnect (the desktop program) and RSPduo on one PC,
receiving without loss, but live it has not yet decoded CW, OOK or FT8/FT4
there (being looked into with that PC's recordings). Frequency windows
let you choose the decoder, or none, for a part of the band.
SDRconnect_headless has not yet worked with it (it ends with an access
violation of its own soon after opening the radio).
If it does not work for you, *Settings → Diagnostic report…* makes one
file to send.

**Try it first without a receiver:** install, then start *HF Classifier
demo* from the Start menu (or `hf_classifier --demo --open`). It plays a
synthetic band in a loop: amateur digital modes at 14.080 MHz for 45 s,
then a utility band at 8.400 MHz for 60 s with DSC, SITOR-B, HFDL, a
radiofax chart and signals
built like military systems (a STANAG 4285-like framed modem, Akula-like
bursts, a 4:3-code FSK, a 16-tone modem, an MFSK). Click an emission in
the lane above the waterfall to see what was measured, what it is
consistent with, and the model's opinion. The demo logs to its own
`demo.db` and uses port 8074, so it runs next to the real thing.

The program, `hf_classifier`, connects to SDRconnect, finds every emission
in the IQ, and decodes these modes:

- NAVTEX/SITOR-B
- GMDSS DSC (digital selective calling on MF/HF)
- RTTY: 45.45 Bd/170 Hz and 50 Bd/450 Hz always; other rates and shifts
  (45.45 to 100 Bd, 85 to 1000 Hz) once the signal's shift and rate are measured
- CW
- PSK31, PSK63, PSK125, PSK250 and PSK500, BPSK and QPSK (fldigi's modes)
- ALE 2G, 3G and 4G
- HFDL (the aircraft data link): squitters of the ground stations and the
  PDUs from and to aircraft, on every channel a frequency list names
  (EiBi, built in, names 124)
- FT8, FT4 and WSPR, in their sub-bands

Recognised by structure, not decoded: **DRM** broadcasting (by its OFDM
guard interval, with the robustness mode), and narrower OFDM with DRM's
timing (WinDRM, EasyPal).
- Radiofax (weather charts, press photos): the picture itself, saved as a
  PNG and shown with the emission
- SSTV (amateur colour pictures): Martin, Scottie, Robot 36/72 and PD
  modes, the picture saved and shown the same way
- Time signal stations, by their second pulses; the DCF77 and MSF time
  codes are decoded (date, time, summer time)

Recognised by structure, without reading any traffic: **Akula** bursts (by
their published sync and end groups), and the **7-bit constant-weight code**
that CIS-36-50 / CIS-50-50 (T-600), SITOR and several ARQ systems use (it
names the code; rate and shift narrow down the system).

It logs everything to SQLite and shows it in a browser: a waterfall, the
emissions, the decoded text, search and CSV/JSON export, and click-to-listen
in SDRconnect.

The classification of modes that are not decoded (the military and
government modems) has its first layer: every emission's modulation is
measured ("≈ 2-FSK, shift 850 Hz, 75 Bd", "≈ MFSK, 8 tones 250 Hz apart,
125 Bd", "≈ PSK, 31.2 Bd", "≈ multi-carrier, 16 tones 112 Hz apart"), and
compared with the 65 catalogued modes whose parameters are known
([catalogue/](catalogue/README.md), from the sources in
[docs/RESEARCH.md](docs/RESEARCH.md)): the UI lists the modes the emission
is *consistent with*, with where their parameters come from. That is not an
identification yet: several modes often share every measured parameter. The
period at which a signal repeats itself is measured too ("≈ PSK, 2400 Bd,
frame 106.7 ms"), which tells apart serial modems that share their rate:
STANAG 4285, 4529 and 4539, MIL-STD-188-110 and CIS BPSK 1200. Telling the
rest apart needs their sync patterns (milestone 5).

A **trained model** adds its opinion of the family (FSK, MFSK, PSK,
multi-carrier, OOK, sweep/pulse, analogue) with a confidence, shown apart
from the measurements as "Model's opinion: looks like PSK (66 %)", or "not
sure" under 50 %. On signals of systems it was not trained on it is right
about 60 % of the time, and about 87 % when it is sure
([tools/ml/](tools/ml/README.md)). It names families, never systems.

**Frequency lists** add what WebSDR and KiwiSDR sites show as markers: put
any list of known frequencies (see [Frequency lists](#frequency-lists)) in
the data folder's `frequencies` folder, and every emission on a listed
frequency shows the listing, the listed mode goes first among the modes it
is consistent with, and the decoder the list names is tried. A listing is
a hint, never an identification.

**Tested on synthetic signals, on the Signal Identification Wiki's audio
samples, and against a stand-in for SDRconnect.** Nothing has run against a
real SDRconnect yet. On synthetic signals in white noise, every mode decodes
down to about 0 dB SNR in 2.5 kHz (FT8 to -22 dB), and signals of other
modes carrying random data (FSK, MFSK, BPSK, keyed tones) produce no text:
decoders without their own error detection only publish text that reads
like radio traffic (details and the price of that in
[core/README.md](core/README.md)). On the wiki's samples (below, *The other
tools*), 55 of the 82 samples of the decoded or recognised modes decode or
are recognised. Of the 992 samples of 269 other entries, 5 get a label:
four are that mode really in the recording (a Morse ID; ALE calls beside a
modem's traffic, which pass ALE's error check), one is undecided. See [docs/PLAN.md](docs/PLAN.md) for the plan and
[docs/RESEARCH.md](docs/RESEARCH.md) for the research behind it.

| Document | Contents |
|---|---|
| This README | Installing, setting up SDRconnect, using the program and tools, building (with a quick start in Bulgarian) |
| [docs/PLAN.md](docs/PLAN.md) | Scope, how it fits into SDRconnect, milestones, what is still needed |
| [docs/ROADMAP.md](docs/ROADMAP.md) | What is still needed before 1.0, the first version that is not a preview |
| [docs/RESEARCH.md](docs/RESEARCH.md) | SDRconnect and RSPduo facts, the modem catalogue with sources, measurements on recordings |
| [app/README.md](app/README.md) | The program: threads, web interface and its API, click to listen |
| [core/README.md](core/README.md) | The SDRconnect client, channelizer, detector, decoder host: how they work, parameters, limitations |
| [catalogue/README.md](catalogue/README.md) | The mode catalogue and the provenance of its parameters |
| [decoders/README.md](decoders/README.md), [third_party/README.md](third_party/README.md) | Code copied from the author's SDRuno plugin repositories and from others, with versions and licences |

## Installing on Windows

1. Install [SDRconnect](https://www.sdrplay.com/sdrconnect/) (1.0.9 or
   later) and check that it shows your RSPduo.
2. Run `HFClassifier_Setup_<version>.exe`. It comes from every CI run
   (artifact `sdrconnect-classifier-windows-x64`), or build it yourself
   (below). It installs into `C:\Program Files\HF Classifier`, adds
   *HF Classifier* to the Start menu, and optionally a desktop shortcut and
   *Start with Windows*. It tells you if SDRconnect is missing.
   Silent install: `HFClassifier_Setup_<version>.exe /S [/DESKTOP=0|1] [/AUTOSTART=0|1]`.
3. Start *HF Classifier*. It runs in the notification area (next to the
   clock) and opens its window, an app window of Microsoft Edge or Chrome.
   Its icon's menu opens the window again, opens the data folder, or exits.

What it does about SDRconnect, by itself:

- It connects to SDRconnect's WebSocket server (127.0.0.1:5454 unless set
  otherwise) and reconnects when that goes away.
- If nothing answers and the SDRconnect window is not open, it starts
  `SDRconnect_headless.exe` (SDRconnect without a window) from SDRconnect's
  folder, and stops it again when it exits. Settings → *Start SDRconnect
  without a window* turns this off.
- If the SDRconnect window is open but does not answer, its WebSocket
  server is off; the window says how to switch it on.

The window always says what the connection is doing and what to do next
(not installed, server off, starting, connected). Its Settings dialog holds
the language (Bulgarian or English), SDRconnect's address and port, the
tuner, dual tuner mode, the headless start, opening the window at start,
FT8/FT4, and the window's port. They are saved in
`%LOCALAPPDATA%\HF Classifier\settings.json`, next to the log
(`hf_classifier.db`) and the program's own messages (`hf_classifier.log`).
Uninstalling keeps that folder.

Started a second time (the shortcut clicked again), it opens the running
one's window.

**Unverified, until tried with a real SDRconnect:** the window's process
name (`SDRconnect.exe`), where its installer puts it (`Program Files\SDRplay\SDRconnect`,
or found through Windows' list of installed programs), and the headless
program's `--websocket_port` option (from third-party guides). If the
automatic start does not work, start SDRconnect yourself; the rest does not
depend on it.

## Бърз старт (на български)

1. Инсталирайте [SDRconnect](https://www.sdrplay.com/sdrconnect/) (1.0.9
   или по-нов) и проверете, че вижда вашия RSPduo.
2. Стартирайте `HFClassifier_Setup_<версия>.exe` и изберете „Български“.
   Програмата се инсталира в `C:\Program Files\HF Classifier`, с пряк път
   „HF класификатор“ в менюто „Старт“; по желание и на работния плот и със
   стартиране заедно с Windows.
3. Стартирайте „HF класификатор“. Той работи в областта за уведомяване (до
   часовника) и отваря прозореца си в Microsoft Edge или Chrome.

**Първо без приемник:** „HF класификатор - демонстрация“ в менюто „Старт“
пуска синтетичен обхват в цикъл (любителски цифрови видове на 14,080 MHz,
после DSC, SITOR-B, HFDL, радиофакс и сигнали, построени като военни
системи, на 8,400 MHz).
Щракнете излъчване, за да видите измереното, с какво е съвместимо и
мнението на модела. Демонстрацията пише в отделен дневник (`demo.db`) на
порт 8074. Версия 0.6.0 е предварителна: работила е с истински SDRconnect
(с прозорец) и RSPduo на един компютър без загуба на сигнал, но там още не
е декодирала CW, OOK или FT8/FT4 на живо (търси се причината в записите от
този компютър). Честотните прозорци (провлачване по водопада) позволяват
да изберете декодер или никаква обработка за част от обхвата. SDRconnect
без прозорец още не е проработил с нея (спира с грешка при достъп до
паметта скоро след като отвори приемника). Ако нещо не работи, „Настройки“ →
„Диагностичен отчет…“ прави един файл за изпращане.

Програмата сама се свързва със SDRconnect. Ако SDRconnect не работи, тя го
стартира без прозорец (`SDRconnect_headless.exe`) и го спира при изход; това
се изключва от „Настройки“. Ако прозорецът на SDRconnect е отворен, но не
отговаря, значи WebSocket сървърът му е изключен: включете го в настройките
на SDRconnect (Preferences, раздел WebSocket Server), порт 5454. Лентата в
горната част на прозореца винаги казва какво става с връзката и какво да
направите.

В прозореца: водопад с лента на излъчванията над него (зелено — декодирано,
оранжево — неразпознато), таблица на излъчванията с търсене и филтри,
подробности за избраното излъчване с бутон „Слушай в SDRconnect“, който
настройва демодулацията, филтъра и честотата на SDRconnect, и декодирания
текст. „Търсене в дневника“ и „Експорт“ (CSV за Excel или JSON) работят
върху целия дневник. Дневникът, настройките и съобщенията на програмата се
пазят в `%LOCALAPPDATA%\HF Classifier`.

**Честоти и маркери.** Както в KiwiSDR, записаните на известни честоти
станции и системи се показват като маркери над водопада; щракване върху
маркер настройва SDRconnect на него. Бутонът „Честоти“ показва заредените
бази, включва и изключва всяка от тях и търси във всички. Вградени са
честотите на GMDSS DSC и NAVTEX и разписанието на EiBi (радиостанции с
часовете им, HFDL, морзови, телетайпни и станции за точно време). Други
бази (`dx.json` или `dx_community.json` от KiwiSDR, отметки от OpenWebRX,
собствен списък с по една честота на ред, например
`4583 DDK2 Hamburg RTTY 50/450`) се слагат в папката, която бутонът
„Отвори папката с базите“ отваря, и се зареждат с „Прочети базите отново“.
Записът в списък е указание, не разпознаване.

**Обработка.** В горната лента до връзката „Натоварване N % · M се
декодират“ казва колко от реалното време обработката е заета (при 100 %
изостава): зелено, докато смогва, оранжево от 80 %, червено, докато се
губи сигнал. Щракване показва подробностите: колко време отива за всяка
дейност и колко сигнала се декодират, пробват или само измерват. В
таблицата до вида на сигнала пише какво става с него („пробва се“,
„оставен“, „спрян“...), а в подробностите му има „Спри обработката“.
Филтърът „В обработка“ показва сигналите, с които програмата работи.
Настройките (най-много сигнали наведнъж, след колко секунди без резултат
да се остави сигнал, под колко dB да не се обработва, дали да се
измерват сигнали без декодер и кои декодери да са изключени) са в
„Настройки“ → „Обработка“ и влизат в сила веднага след запазване.

**Изтегляне и мои маркери.** В „Честоти“ бутонът „Изтегли списъците на
KiwiSDR и EiBi“ кара вашия компютър да изтегли общия списък с етикети на
KiwiSDR (там са военните и служебните маркери от сайтовете с KiwiSDR) и
текущото разписание на EiBi в папката с базите и да ги прочете. „Запази
като мой маркер“ в подробностите на сигнал или точка от водопада записва
честота, етикет, бележка и вид в `my_markers.json`; маркерите ви са
златисти, списък с тях има в „Честоти“, където се променят и изтриват.

**Задачи за запис.** Бутонът „Задачи за запис“ изброява от кои системи са
нужни записи, за да се научи програмата да ги разпознава, колко записа
вече има и на кои честоти вашите бази посочват тези системи. Щракнете
честота, изчакайте сигнала и натиснете „Запис“. Записите (SigMF) са в
папка `recordings` до дневника; изберете малка честота на дискретизация в
SDRconnect (62,5 до 250 kHz), за да са малки файловете.

**Когато нещо не работи: диагностичен отчет.** „Настройки“ →
„Диагностичен отчет…“ (предлага се и в предупреждението, когато връзката
не става), „Направи диагностичен отчет“ в менюто на иконата или „HF
класификатор - диагностичен отчет“ в менюто „Старт“ (работи и когато
прозорецът не се отваря) прави един ZIP файл в папка `reports` до
дневника: версиите на програмата, Windows и SDRconnect, настройките,
състоянието на връзката, дневниците и измерване на това как SDRconnect
подава сигнала. Изпратете този файл. Не съдържа записи, картини или
дневника на сигналите; пътищата в него може да съдържат потребителското ви
име в Windows.

Програмата само приема: не предава, не дешифрира и не декодира криптирани
съобщения.

## Setting up SDRconnect

You need SDRconnect 1.0.9 or later (1.0.10 is current) on Windows 10 or 11, 64-bit.

1. Start SDRconnect, select the RSPduo and start it (play button).
2. Switch its **WebSocket server** on. It is off by default and listens on
   port 5454. SDRplay's documents do not say where the switch is; two
   third-party guides give "Other Actions → Preferences → WebSocket Server"
   and "⋮ → Settings → WebSocket" (versions may differ).
3. The WebSocket server accepts connections from your whole network, not
   only from this PC (third-party measurement). Unless you want that, keep
   Windows Firewall blocking port 5454 for other machines. SDRconnect's
   firewall rule allows it on "Private" networks.

Or leave SDRconnect closed and let HF Classifier start it without a window
(above). By hand that is
`"C:\Program Files\SDRplay\SDRconnect\SDRconnect_headless.exe" --websocket_port=5454`
(the option comes from third-party guides, not from SDRplay; port 5454 is
said to be the default anyway; `sdrc_probe` tells you whether it works).

**If it does not work with SDRconnect headless:** the program now writes
what happens into two files in the data folder (the icon's menu opens it):
`hf_classifier.log` (how it started SDRconnect_headless, what SDRconnect
reported on connecting: API version, control, active radio, rate,
frequency; whether IQ arrives) and `sdrconnect_headless.log`
(SDRconnect_headless's own output, and its exit code if it quits). If it
connects but no IQ arrives within 5 s and no radio is open, it asks
SDRconnect to open the first radio; if that does not help either, the
window says "connected, but no signal arrives".

### When something does not work: the diagnostic report

One click makes one ZIP file with everything needed to see why: Settings,
*Diagnostic report…* (also offered in the window's warning when the
connection fails), or *Make a diagnostic report* in the icon's menu, or
*HF Classifier diagnostic report* in the Start menu (that one works even
when the window does not open). It is written to `reports` in the data
folder, and the window offers it for download. Please send that file.

It contains `report.json` (the program's version; Windows, processor and
memory; the settings; the connection state and the receivers' processing
load; where SDRconnect is installed, the version of `SDRconnect.exe` and
`SDRconnect_headless.exe`, whether they run and whether the WebSocket port
answers; and the measurements `sdrc_probe` makes: SDRconnect's properties
and how IQ arrives at the current rate, or why they were not made),
`probe.txt` (the same measurements as text), `settings.json`, and the last
2 MB of `hf_classifier.log`, `hf_classifier.previous.log` (the run before;
the program keeps it now) and `sdrconnect_headless.log`. No recordings,
pictures or log of signals; the paths in it can contain the Windows user
name (`README.txt` in the file says so).

While the program runs, the measuring is done on its own connection:
processing pauses for a few seconds, and the connection is set up again
afterwards. *Also measure each sample rate* tries every rate as `sdrc_probe`
does (about a minute; SDRconnect gets its settings back at the end). From
the command line: `hf_classifier --report` (or `--report-rates`); if the
program is running, it is asked to make the report.

Tested against the stand-in SDRconnect (`testing/`), on Linux, and in CI on
Windows (the installed program, with the stand-in answering, and asked
while it runs); not yet with a real SDRconnect.

## hf_classifier

`HFClassifier.exe` is the program for everyday use (no console, in the
notification area). `hf_classifier.exe` is the same program in a console,
printing what it does; both take the options below, which override the
settings for that run (they are not saved).

```
hf_classifier                              # as set in Settings (SDRconnect on this PC, tuner 1)
hf_classifier --tuner both --open          # RSPduo in dual tuner mode: both tuners
hf_classifier --no-headless                # never start SDRconnect_headless
hf_classifier --play rec.sigmf-meta --open # a recording instead of SDRconnect
```

The window is http://127.0.0.1:8073/. It follows SDRconnect: tune there as
usual, and the classifier follows the new centre frequency and sample rate.
The emission lane above the waterfall shows what is on the air now: green
for decoded, orange for not (yet) identified. Clicking an emission and
pressing *Listen in SDRconnect* sets SDRconnect's demodulator, filter and
VFO to it. The log (`hf_classifier.db`, SQLite) keeps every emission and
decode across runs. *Search the log* and *Export* query it with the filters
shown.

How it works: one FFT per block gives the waterfall and the detector its
spectrum, and cuts a 12 kHz channel out for every emission. The emission's
audio goes to the decoders that fit its bandwidth, and text is published only
when it passes the decoder's quality gate. The FT8 and FT4 sub-bands inside
the receiver's band are decoded per 15 s and 7.5 s UTC slot. Details:
[core/README.md](core/README.md).

| Option | |
|---|---|
| `--host`, `--port` | where SDRconnect is (default 127.0.0.1:5454) |
| `--tuner 1\|2\|both` | which RSPduo tuner(s) to process |
| `--dual SERIAL` | switch the RSPduo to dual tuner mode first |
| `--rate HZ`, `--centre HZ` | set these first |
| `--headless PATH`, `--no-headless` | which `SDRconnect_headless` to start when nothing answers (default: found in SDRconnect's folder), or never start it |
| `--demo` | play the built-in synthetic band in a loop instead of connecting (logs to `demo.db`, window on port 8074 unless `--db`/`--http` are given) |
| `--play FILE`, `--speed X`, `--exit` | playback of a SigMF recording (ci16_le, cf32_le, ci8, cu8); speed 0 = as fast as possible |
| `--http PORT`, `--db PATH`, `--settings PATH` | window port (8073; 0 = any free one), log file, settings file |
| `--frequencies DIR` | the folder of frequency lists (default: `frequencies` in the data folder) |
| `--pictures DIR` | where received radiofax and SSTV pictures go (default: `pictures` in the data folder) |
| `--recordings DIR` | where recordings made from the window go (default: `recordings` in the data folder) |
| `--open`, `--no-open`, `--language bg\|en` | open the window at start or not, its language |
| `--max-sessions N`, `--no-ft8` | emissions decoded at once (48); skip FT8/FT4 |

With options, `hf_classifier` opens the window only with `--open`; without,
as set in Settings. `--settings FILE` reads that file instead of the one in
the data folder (and the Settings dialog saves to it).

### Frequency lists

As on WebSDR and KiwiSDR sites, stations and systems listed on known
frequencies show as **markers above the waterfall**. Click a marker and
SDRconnect is set to it: demodulator, filter and frequency, as the list
says or as the listed mode implies. *Frequencies* (top right) shows the
databases that are loaded, lets you switch each on and off, shows only
what is on the air now for lists with a schedule, and searches all of
them ("hfdl", "volmet", "stanag"): clicking a result moves the receiver
there.

Built in: the nine GMDSS DSC and NAVTEX frequencies (ITU), and **EiBi's
schedule** (winter 2026/27, 8971 entries: broadcasters with their hours,
and utility stations: 124 HFDL, 124 Morse, 75 teletype, 71 time signal;
EiBi's terms allow passing the lists on, see `catalogue/eibi/`). To add
more, press *Open the databases folder*, put the files there (the
`frequencies` folder inside the data folder) and press *Read the
databases again*. Any number of files:

**Download** (in *Frequencies*): *Download the KiwiSDR and EiBi lists*
makes your PC fetch KiwiSDR's community label list
(`kiwisdr.com/dx/dx_community.json`, where the military and utility
markers of KiwiSDR sites come from) with its category file, and EiBi's
schedule for the current season (`eibispace.de`; the previous season if
the current one is not out yet), into the databases folder, and read the
databases again. Each list is kept only if it reads as a list (a server's
error page is not kept); the window shows what each gave. Nothing is
shipped with the program for this: you download the lists, for your own
use. Only the newest EiBi schedule in the folder is read.

**My markers**: *Save as my marker* (in an emission's or a clicked
point's details) saves its frequency, a label, a note and the mode into
`my_markers.json` in the databases folder; they are markers at once (in
gold), are listed in *Frequencies*, and can be edited or deleted there or
by clicking them. A label that names a mode ("STANAG 4285", "RTTY 50/450")
guides the decoders and "Consistent with" as other lists' entries do.

**Categories**: KiwiSDR labels carry a category (Milcom, Aero, HFDL,
Marine, Time, Utility, ...). Once the category file is downloaded the
markers take KiwiSDR's colours and names, and *Frequencies* has a tick
box per category to hide or show them. With the real community list
(907 labels) 905 are read, among them 241 Milcom, 157 Aero and 129 HFDL.

**Shared frequencies**: clicking a marker lists every entry within
500 Hz from all lists (on-air by schedule first, with hours), so when
several stations share a channel you see them all.

- **KiwiSDR labels**: a KiwiSDR's `dx.json` as it is (the stock file of
  the KiwiSDR software reads completely: 872 entries), or the KiwiSDR
  community list (`dx_community.json`, from kiwisdr.com/dx/), which is
  where most military and utility markers on KiwiSDR sites come from. It
  is not shipped: I found no statement that it may be passed on.
- **A newer EiBi schedule** (`sked-*.csv` from eibispace.de): it replaces
  the built-in one.
- **Your own**: a text file, one frequency per line with what is there,
  for example `4583 DDK2 Hamburg RTTY 50/450` or
  `8453.0;STANAG 4285 net;France`. Commas, semicolons, tabs or spaces.
- **OpenWebRX bookmarks** and other JSON with a `frequency` and a `name`.
- **WebSDR station lists** and spreadsheet exports (CSV): the first number
  on a line is the frequency, the rest the label.

One unit per file, told from its largest number: Hz, kHz or MHz. Labels
are searched for the modes of the catalogue ("STANAG 4285", "HFDL",
"NAVTEX", "ALE", "Link-11", "T-600" ...) and for a rate and shift
("50 Bd 450 Hz", "75/850"). The console version prints how many entries
each file gave.

What a listing does, and does not: an emission on a listed frequency (or
up to 2.5 kHz above one, as lists give dial frequencies) shows
"Listed on this frequency" in its details; a listed mode that also fits
the measurements goes first under "Consistent with", marked; and the
decoder a listing names (NAVTEX, DSC, CW, PSK, RTTY with its rate and
shift) is tried at once. Nothing is labelled from a list alone. An entry
with hours (EiBi) counts only while it is on the air.

**Without internet on the SDR PC:** the program needs none. To get the
lists, press the Download button on any PC with internet where HF
Classifier is installed, then copy the files from
`%LOCALAPPDATA%\HF Classifier\frequencies` on that PC to the same folder
on the SDR PC, and press *Read the databases again* (or restart).

### Frequency windows and a marker's decoder

**Drag across the waterfall** to mark a frequency window, then choose what
happens to the signals whose centre is in it:

- **one decoder only** (CW, PSK31 to QPSK500, RTTY at any rate and shift,
  SITOR-B, DSC, ALE, HFDL, radiofax, SSTV, time signals, DRM, Akula): it
  is used without the program's guesses from the bandwidth and without the
  check that its text reads like radio traffic (you have said what it is);
- **no processing**: no decoding and no classification there, and no
  FT8/FT4/WSPR or HFDL channel inside it (saves processor time, keeps out
  what you know is noise).

Windows show over the waterfall; click a window's label to change or
delete it. They are listed in Settings → Processing and kept in
`decode_windows.json` in the databases folder (copied along with the lists
to a PC without internet). Where windows overlap, the narrowest decides;
the FT8/FT4/WSPR sub-bands keep their own decoding unless a window says
no processing.

**A marker's decoder:** in a marker's dialog, *Decode only with* gives the
signals within 1.5 kHz of the marker that decoder only. Saving a marker
from a signal that has been decoded (green) fills in the decoder that
read it.

### Processing: load, signals, settings

The top bar shows the load next to the connection: **Load N % · M
decoding**, N being how much of real time the processing is busy (at
100 % it cannot keep up). Green while it keeps up, orange from 80 %, red
while signal is being dropped. Clicking it opens the details: the time
each activity takes (the spectrum and the detector, each decoder,
measuring signals, the FT8/FT4, WSPR and HFDL sub-bands), all threads
together in % of one processor core, and how many signals are being
decoded, tried or only measured, and have been left.

What happens to each signal shows in the emissions table (*being tried*,
*measuring*, *given up*, *stopped*, *measured only* next to its kind) and
in its details (*Processing*: the decoders, for how long, the processor
time, and a **Stop processing it** button that leaves it for good). The
filter **Being processed** lists the signals the program is working on.

**Settings → Processing** (saved with the dialog, in effect at once, no
restart):

- **At most this many signals at once** (48). When it is full, a signal a
  decoder can read takes the place of one that is only being measured, or
  of one tried for 20 s or more without a result.
- **Leave a signal with no result after** (120 s; 0 = never): a signal
  that several decoders have tried, without any of them reading it, is
  left once it has been measured. Signals being decoded are never left.
- **Do not process signals weaker than** (0 dB).
- **Measure and classify signals no decoder fits** (on). Off: the
  processor goes only to signals that can be decoded. On, a signal no
  decoder fits is measured once (8 s) and then left, instead of being
  followed for as long as it lasts, as before.
- **Decoders**: untick a family (PSK, RTTY, ALE, FT8, WSPR, HFDL...) to
  switch it off.

Also in `settings.json`: `max_sessions`, `give_up_seconds`, `min_snr_db`,
`measure_unknown`, `disabled_decoders` (comma-separated).

### Recording tasks

The program learns from recordings of real signals and is tested on them,
and so far it has none from your receiver. *Recording tasks* (top right)
lists what would help, in groups: decoded modes (to confirm them on the
air), systems recognised by structure (Akula, T-600), systems that are
only classified (the STANAGs, MIL-STD-188-110, Link-11, the CIS modems
...), systems without a public specification, and modes a decoder is
planned for (HFDL, weather fax, SSTV, WSPR, DRM, time signals). Each task
shows how many recordings there are of how many wanted, and **the
frequencies your databases list for that system**, those on the air now
first. Click one and SDRconnect goes there; when the signal is on the air,
press *Record*. The details of any emission have a *Record* button too,
which files the recording under the task the emission seems to belong to.

A recording is SigMF (`recordings` in the data folder, or `--recordings
DIR`): everything SDRconnect is sending, so choose a small sample rate in
SDRconnect (62.5 to 250 kHz; 2 minutes at 62.5 kHz is 30 MB). It stops by
itself after the task's time, when you press *Stop*, or if the sample rate
changes; at most 2 GB. The frequencies come only from the databases you
have loaded: with the built-in ones, the military systems have none, until
you add a list that names them (the KiwiSDR community list does). The
tasks are in `catalogue/recording_tasks.json`. Recording is off in the
demo.

No receiver at hand? `make_test_scene scene` writes a 48 s synthetic
recording with one signal of every decoded mode. Play it with
`hf_classifier --play scene.sigmf-meta --open`.

## The other tools

### sdrc_probe: measure your setup first

```
sdrc_probe
sdrc_probe --antennas --dual 1806030000      # with your RSPduo's serial number
```

It connects to SDRconnect and measures what the API document does not say:
which sample rates SDRconnect accepts and whether the IQ really arrives at
that rate on your PC, message sizes, levels, the antenna names and, with
`--dual`, what RSPduo dual-tuner mode allows. It changes the sample rate (and
the antenna and device mode, if asked) while it runs and restores them at
the end; it says so if something could not be restored. Close other programs
that use the WebSocket API first. It writes `sdrc_probe_report.json`
(the tool and the time, then the measurements under `"probe"`): please send
that file back. The program's diagnostic report (above) makes the same
measurements on the program's own connection.

| Option | |
|---|---|
| `--seconds S` | measuring time per setting (default 5) |
| `--rates LIST` | sample rates to try (default 62500 to 10000000, plus 3000000) |
| `--no-rates` | only the current rate |
| `--antennas` | select each antenna in turn and measure its noise level |
| `--dual SERIAL` | test dual-tuner mode (selects `SERIAL:Dual`, as SDRplay's own example does) |
| `--host`, `--port` | where SDRconnect is (default 127.0.0.1:5454) |

### sdrc_record: IQ recordings for testing

```
sdrc_record -o 2026-10-20_akula_9201khz --seconds 300 --description "Akula? 9201 kHz USB, longwire"
```

It writes `BASE.sigmf-data` (the samples exactly as SDRconnect sends them,
16-bit IQ) and `BASE.sigmf-meta` ([SigMF](https://github.com/sigmf/SigMF)):
sample rate, receiver, and for every stretch of contiguous samples its
centre frequency and UTC time. It starts a new stretch after every retune
and every gap. Gaps (IQ that SDRconnect dropped) are found by timing and
marked as annotations where they happened. A change of sample rate starts a
new file pair. Options: `--device primary|secondary`, `--center HZ`,
`--rate HZ`, `--dual SERIAL`, `--host`, `--port`.

Recordings of the modes listed in [docs/RESEARCH.md](docs/RESEARCH.md)
section 3.5 are what the classifier will be trained and tested on.

### sigidwiki_bench: the Signal Identification Wiki's samples

```
tools/sigidwiki_bench.py --classifier build/hf_classifier     # Python 3, numpy, ffmpeg; network the first time
```

It fetches the [Signal Identification Wiki](https://www.sigidwiki.com)'s
signal pages and audio samples (the wiki is the source of Artemis's
database; the samples stay in `sigidwiki_cache/`, as the wiki grants no
right to redistribute them), makes each sample into IQ, plays it through
`hf_classifier`, and writes a report: per entry what was decoded, labels on
entries of other modes (false, or that mode inside the recording), what the
feature meter measured, and whether the catalogue listed the right mode.

The samples are USB audio, mostly MP3, made into IQ with white noise added
across the band; how loud that noise should be cannot be told from the
audio, so each sample is played twice (noise at the passband's typical
level, and at its quietest part) and what either run finds counts. They are
short, mostly strong and clean, and went through someone's receiver and
sound card: close to IQ from an RSPduo, not the same.

Results on 2026-11-10, 287 entries (the wiki's VLF to HF signals), 1077
samples (about half an hour, four at a time):

| Wiki entry of a decoded or recognised mode | Samples | Decoded or recognised |
|---|---|---|
| Radio Teletype (RTTY) | 19 | 17 |
| Phase Shift Keying (PSK) | 16 | 11 |
| Morse Code (CW), Coherent CW | 7 | 4 |
| SITOR-B | 2 | 2 |
| GMDSS Digital Selective Calling | 5 | 3 |
| ALE 2G / 3G / 4G | 1 / 5 / 2 | 1 / 0 / 2 |
| FT8 / FT4 | 2 / 3 | 1 / 3 |
| 4:3 code: CIS-36-50 / CIS-50-50 | 4 / 1 | 3 / 1 |
| 4:3 code: POL-ARQ / SWED-ARQ / ARQ-E3 / ARQ-N | 4 / 6 / 4 / 1 | 4 / 1 / 1 / 1 |
| **In all** | **82** | **55** |

The decoded modes alone: 44 of 62 (on 2026-11-07, 33 of 57: PSK went from
4 to 11 with PSK250/500 and QPSK, the second 4G burst is found now that a
session's decoders also hear the 0.75 s before it started, and DSC is
new). Of the two DSC misses one is VHF DSC (1200 Bd, not decoded) and one a
recording so clean that the detector takes the whole 16 kHz for one
emission; the decoder alone reads both of its calls. The 4:3 code rows are
recognition of a code, not decoding: 11 of 20 samples of six systems that
use 7-bit constant-weight words; SWED-ARQ and ARQ-E3 mostly at rates the
detector does not try or in short bursts.

The misses: the 5 PSK samples are variants the decoder does not have (PSK10
and four FEC variants); the 3G ALE samples are data waveforms (BW2-BW4),
36 s of STANAG 4539 traffic, and one BW0 burst (7 dB) that overlaps a
longer signal in the same recording and is absorbed into its emission
(`core/README.md`, Limitations); the Morse misses are a beacon ID too short
for the text rule, Morse sent by FSK and a marker that is not text;
ft8_lib itself decodes nothing in the second FT8 clip; the RTTY misses are
several overlapping signals and 100 Bd at 85 Hz. Both 4G bursts decode up
to their CRC, which fails (linking protection, as `core/README.md`
explains under Limitations), and are reported as received intact.

**Other entries**: of 992 samples of 269 entries, 5 got a label. Four are
that mode really in the recording (a Russian VLF station's Morse ID; ALE
calls in three modem recordings). The fifth is "4:3 code" on RDL, a Russian
VLF FSK station, at exactly the detector's threshold (45 of 60 words): it
may be right, I cannot tell. Two earlier runs found false labels that are
fixed: a POL-ARQ recording read as SITOR-B (2026-11-07; believed text may
no longer be more than 60 % one character), and on 2026-11-10 "Akula" on a
KG-STV sample from Akula's end group alone (now only with its sync group)
and "T-600" on eight samples of ARQ systems that share its kind of code
(now named as the code).

**Catalogue**: for the 223 samples of 39 entries of catalogued modes, a
right mode is among the modes the strongest emissions are consistent with
for 115, and first for 92 (on 2026-11-07: 59 and 40 of 216; on 2026-11-09:
108 and 86). The last step is the feature meter reading real FSK better
(`core/README.md`): RTTY went from 9 to 14 of 19, PACTOR I from 1 to 3 of 4. The serial
modems are told apart by their frame period now: a right mode comes first,
with its frame agreeing, for all 7 STANAG 4285 samples, all 5 STANAG 4529,
18 of 20 MIL-STD-188-110 serial, both STANAG 4415 (the same modem), all 7
110A-MOD, 1 of 5 STANAG 4539 (the others repeat at a multiple of its frame,
or at a period not explained) and 1 of 3 CIS BPSK 1200; on 2026-11-07, for
none of them. The catalogue's frame periods were checked on these same
samples (`docs/RESEARCH.md` section 3.7), so this is not an independent
test. It also works for PSK31 to PSK500 (12 of 16), RTTY (14 of 19), Olivia
(6 of 9), Morse (4 of 6) and CIS-12 (3 of 15). Not for 3G/4G ALE: no frame
is catalogued for them, and a 4G burst repeats almost all of itself every
40 ms, so its symbol rate cannot be read (it is decoded instead); nor for
HFDL and Link-11 (listed, not first), the parallel-tone modems (16 and 39
tones), MFSK16, SITOR-A or Serdolik. The full report
(`sigidwiki_report.md`) lists every sample with what was decoded and
measured.

### make_busy_scene: a busy band, to see whether it keeps up

```
make_busy_scene busy                 # 2.048 MS/s, 30 s around 7.074 MHz
hf_classifier --play busy.sigmf-meta --speed 0 --exit --db busy.db --http 0
```

A synthetic busy band: 40 CW stations, FT8 and FT4, RTTY, PSK31, 20 AM
broadcasters and 15 wideband modem-like signals, starting at different
times. Playback faster than its length means the processing keeps up at
that rate on this PC. Not a real recording.

### fake_sdrconnect: no receiver needed

```
fake_sdrconnect --port 5454 --tone 30000:-20 --fsk -50000:-25:1000:500
```

A stand-in for SDRconnect's WebSocket server with synthetic IQ: carriers and
2-FSK signals, a refused 3 MS/s rate, delayed `property_changed`, dual-tuner
mode, and optional lost IQ (`--drop-after S --drop S`). Where the real
SDRconnect's behaviour is unknown, it uses placeholders (see
`testing/fake_sdrconnect.hpp`). It is for development and tests, and is not
evidence of how SDRconnect behaves.

## Building from source

CMake 3.16+, a C++17 and C11 compiler (MSVC 2022, GCC, Clang). No other
dependencies: the third-party code (ft8_lib, KISS FFT, SQLite) and the
decoders from the author's SDRuno plugin repositories are in the source tree.

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target installer   # Windows, with NSIS 3 installed
```

The installer (`installer/setup.nsi`, English and Bulgarian) lands in
`build/HFClassifier_Setup_<version>.exe`.

The test suite (`hfc_tests [name-filter]`) covers:

- the codecs and WebSocket framing against their RFC test vectors, and
  fragments, pings, masking and oversized messages from a hand-made server;
- JSON, the IQ clock (lost samples, reader stalls of 0.5 to 2.4 s, loss
  during a stall), SigMF writing and reading (four sample formats), strict
  ISO time parsing;
- the client against the fake SDRconnect;
- the channelizer's exactness at the fake's rates and 2.048 MS/s, on odd
  bins and between bins;
- the detector on noise, on a mixed band, merging and grouping FSK tones,
  and a sudden rise of the noise;
- end-to-end decoding of every mode through the whole chain; weak ALE and
  strong CW; no decodes in noise or from random-data signals of other
  modes; FT8 slots with more than 50 signals;
- retunes, the SQLite log (two receivers, sessions, upgrading an old log),
  and the application's HTTP/WebSocket interface, including following and
  controlling the fake SDRconnect with one tuner and with both;
- the diagnostic report: the ZIP format, the probe against the fake (and
  the rate it found restored), a report asked of the running program over
  its UI's command (measured on its own connection, which is set up again
  afterwards), `--report` with and without the program running, and with
  nothing answering;
- the settings file and its checks, the command line, the Settings
  dialog's commands (saved without the run's command-line overrides), the
  connection states, and starting a "headless SDRconnect" (the fake) when
  nothing answers and stopping it at exit.

CI (GitHub Actions) builds and tests on Linux on every push (pushes that
change only documentation run nothing). Started by hand with *full* or
*release*, and for every release, it also runs the suite under
ThreadSanitizer and AddressSanitizer, and on Windows with MSVC: these take
most of the Actions minutes, so they do not run on every push. It runs the probe and the recorder against the fake, and
plays the synthetic scene through `hf_classifier`, checking that every mode
was identified. On Windows it builds the installer, installs it silently,
plays the scene through both installed programs, and uninstalls, checking
that the uninstaller closes a running HF Classifier and keeps the user's
data; it also makes a diagnostic report with the installed program (the
fake SDRconnect answering) and asks the running one for another, checks
both, and checks that the previous run's log is kept. The web UI was checked by hand in Chromium (both languages, phone
width); there is no automated UI test.

## Layout

```
app/        hf_classifier and HFClassifier: per-receiver processing, log, web UI (app/ui), settings, SDRconnect start-up, tray
installer/  the Windows installer (NSIS)
core/       SDRconnect client, channelizer, detector, decoder host, pipeline, store, web server (library hfc_core)
decoders/   the decoders copied from the author's SDRuno plugin repositories
third_party/ ft8_lib (with KISS FFT), SQLite
catalogue/  the mode catalogue
testing/    the fake SDRconnect and the synthetic scene builder (library hfc_testing)
tools/      sdrc_probe, sdrc_record, fake_sdrconnect, make_test_scene
tests/      the test suite
docs/       plan and research
```
