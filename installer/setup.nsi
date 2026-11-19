; Installer for HF Classifier, in English and Bulgarian.
;
; Installs the program into Program Files\HF Classifier, with a shortcut in
; the Start menu, optionally one on the desktop and one that starts it with
; Windows (in the notification area, without opening its window), and an
; uninstaller registered in Windows' list of installed programs. It closes a
; running HF Classifier first. It tells whether SDRconnect is installed and
; offers its download page if not. The log and settings, in each user's
; %LOCALAPPDATA%\HF Classifier, are kept when uninstalling.
;
;   makensis -DVERSION=0.6.0 -DBINDIR=<folder> -DREADME=<README.md> [-DOUTFILE=...] setup.nsi
;
; Give BINDIR and README as absolute paths: makensis works in this
; script's folder. BINDIR holds HFClassifier.exe, hf_classifier.exe, sdrc_probe.exe and
; sdrc_record.exe, built for 64-bit Windows. CMake's installer target
; (cmake --build build --config Release --target installer) passes them.
;
; Silent install: HFClassifier_Setup.exe /S [/DESKTOP=0|1] [/AUTOSTART=0|1] [/D=C:\path] (/D last).
; Silent uninstall: "C:\Program Files\HF Classifier\Uninstall.exe" /S

; This file is UTF-8 with a byte order mark: keep the mark. Without it
; makensis on Windows reads the file in the system's ANSI code page (cp1252
; on an English Windows) and every Bulgarian text comes out as garbage.
Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !error "Pass -DVERSION=x.y.z"
!endif
!ifndef BINDIR
  !error "Pass -DBINDIR=<folder> with HFClassifier.exe, hf_classifier.exe, sdrc_probe.exe and sdrc_record.exe"
!endif
!ifndef README
  !error "Pass -DREADME=path\to\README.md"
!endif
!ifndef OUTFILE
  !define OUTFILE "HFClassifier_Setup_${VERSION}.exe"
!endif
; File needs backslashes on Windows; CMake passes forward slashes.
!ifdef NSIS_WIN32_MAKENSIS
  !searchreplace BIN "${BINDIR}" "/" "\"
  !searchreplace READMEFILE "${README}" "/" "\"
!else
  !define BIN "${BINDIR}"
  !define READMEFILE "${README}"
!endif

!define PRODUCT "HF Classifier"
!define APPDIR "HF Classifier"
!define EXE "HFClassifier.exe"
!define REGKEY "Software\${APPDIR}"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\HFClassifier"
!define URL "https://github.com/m4dpir4t3r4g3/SDRconnect_classifier"
!define SDRCONNECT_URL "https://www.sdrplay.com/sdrconnect/"
; The shortcuts' names in both languages, so that either is removed.
!define LINK_EN "HF Classifier"
!define LINK_BG "HF класификатор"
!define DEMO_EN "HF Classifier demo"
!define DEMO_BG "HF класификатор - демонстрация"
!define REPORT_EN "HF Classifier diagnostic report"
!define REPORT_BG "HF класификатор - диагностичен отчет"

Name "$(ProductName)"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\${APPDIR}"
BrandingText "${PRODUCT} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=0 "ProductName" "${PRODUCT}"
VIAddVersionKey /LANG=0 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "FileDescription" "${PRODUCT} setup"
VIAddVersionKey /LANG=0 "LegalCopyright" "${URL}"

!define MUI_ICON "../app/resources/hf_classifier.ico"
!define MUI_UNICON "../app/resources/hf_classifier.ico"

; The language chosen at install is kept for the uninstaller.
!define MUI_LANGDLL_REGISTRY_ROOT HKLM
!define MUI_LANGDLL_REGISTRY_KEY "${REGKEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "Language"

Var SdrconnectFound  ; "1" if SDRconnect is installed
Var FinishMsg

!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_WELCOMEPAGE_TITLE_3LINES
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE_3LINES
!define MUI_PAGE_CUSTOMFUNCTION_PRE FinishPre
!define MUI_FINISHPAGE_TEXT "$FinishMsg"
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "$(RunNow)"
!define MUI_FINISHPAGE_RUN_FUNCTION RunAsUser
!define MUI_FINISHPAGE_LINK "$(SdrconnectLink)"
!define MUI_FINISHPAGE_LINK_LOCATION "${SDRCONNECT_URL}"
!insertmacro MUI_PAGE_FINISH

!define MUI_UNCONFIRMPAGE_TEXT_TOP "$(UninstallText)"
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Bulgarian"
!insertmacro MUI_RESERVEFILE_LANGDLL

; --- English
LangString ProductName ${LANG_ENGLISH} "HF Classifier"
LangString LinkName ${LANG_ENGLISH} "${LINK_EN}"
LangString DemoLinkName ${LANG_ENGLISH} "${DEMO_EN}"
LangString ReportLinkName ${LANG_ENGLISH} "${REPORT_EN}"
LangString WelcomeText ${LANG_ENGLISH} "This will install HF Classifier ${VERSION}.$\r$\n$\r$\n\
  It watches what SDRconnect receives from your SDRplay receiver (RSPduo), finds every emission, decodes FT8, FT4, NAVTEX, RTTY, CW, PSK and ALE, and measures and logs the rest.$\r$\n$\r$\n\
  It connects to SDRconnect by itself, and can start SDRconnect without a window when it is not running. It only receives: it never transmits or decrypts.$\r$\n$\r$\n\
  If HF Classifier is running, setup closes it."
LangString FinishText ${LANG_ENGLISH} "HF Classifier is installed. Start it from the Start menu: it runs in the notification area (next to the clock) and opens its window in Microsoft Edge or Chrome.$\r$\n$\r$\n\
  In SDRconnect, switch its WebSocket server on (port 5454), or let HF Classifier start SDRconnect without a window.$\r$\n$\r$\n\
  To see the program at work without a receiver, start $\"${DEMO_EN}$\" from the Start menu: a synthetic band, with its own log."
LangString FinishNoSdrconnect ${LANG_ENGLISH} "HF Classifier is installed.$\r$\n$\r$\n\
  SDRconnect was not found on this computer. HF Classifier needs it: download it from SDRplay's site (link below), install it, connect the receiver, and start HF Classifier from the Start menu.$\r$\n$\r$\n\
  Meanwhile, $\"${DEMO_EN}$\" in the Start menu shows the program at work on a synthetic band."
LangString RunNow ${LANG_ENGLISH} "Start HF Classifier now"
LangString SdrconnectLink ${LANG_ENGLISH} "SDRconnect on SDRplay's site"
LangString UninstallText ${LANG_ENGLISH} "HF Classifier will be removed. Your log and settings (in %LOCALAPPDATA%\HF Classifier) are kept; delete that folder too if you no longer need them."
LangString Needs64 ${LANG_ENGLISH} "HF Classifier needs 64-bit Windows 10 or 11."
LangString SdrconnectLog ${LANG_ENGLISH} "SDRconnect: $0"
LangString SecMainName ${LANG_ENGLISH} "HF Classifier"
LangString SecMainDesc ${LANG_ENGLISH} "The program, its command-line tools (hf_classifier, sdrc_probe, sdrc_record), and Start-menu shortcuts to it, its demo and its diagnostic report."
LangString SecDesktopName ${LANG_ENGLISH} "Desktop shortcut"
LangString SecDesktopDesc ${LANG_ENGLISH} "A shortcut on the desktop."
LangString SecAutostartName ${LANG_ENGLISH} "Start with Windows"
LangString SecAutostartDesc ${LANG_ENGLISH} "Start HF Classifier when you sign in to Windows, in the notification area, without opening its window."

; --- Bulgarian
LangString ProductName ${LANG_BULGARIAN} "HF класификатор"
LangString LinkName ${LANG_BULGARIAN} "${LINK_BG}"
LangString DemoLinkName ${LANG_BULGARIAN} "${DEMO_BG}"
LangString ReportLinkName ${LANG_BULGARIAN} "${REPORT_BG}"
LangString WelcomeText ${LANG_BULGARIAN} "Ще бъде инсталиран HF класификатор ${VERSION}.$\r$\n$\r$\n\
  Програмата следи какво приема SDRconnect от вашия приемник SDRplay (RSPduo), открива всяко излъчване, декодира FT8, FT4, NAVTEX, RTTY, CW, PSK и ALE, а останалите излъчвания измерва и записва в дневник.$\r$\n$\r$\n\
  Тя се свързва със SDRconnect сама и може да стартира SDRconnect без прозорец, когато той не работи. Програмата само приема: никога не предава и не дешифрира.$\r$\n$\r$\n\
  Ако HF класификаторът работи, инсталацията ще го затвори."
LangString FinishText ${LANG_BULGARIAN} "HF класификаторът е инсталиран. Стартирайте го от менюто „Старт“: работи в областта за уведомяване (до часовника) и отваря прозореца си в Microsoft Edge или Chrome.$\r$\n$\r$\n\
  В SDRconnect включете WebSocket сървъра (порт 5454) или оставете HF класификатора да стартира SDRconnect без прозорец.$\r$\n$\r$\n\
  За да видите програмата в действие без приемник, стартирайте $\"${DEMO_BG}$\" от менюто „Старт“: синтетична лента, със собствен дневник."
LangString FinishNoSdrconnect ${LANG_BULGARIAN} "HF класификаторът е инсталиран.$\r$\n$\r$\n\
  SDRconnect не е намерен на този компютър, а програмата има нужда от него: изтеглете го от сайта на SDRplay (връзката по-долу), инсталирайте го, свържете приемника и стартирайте HF класификатора от менюто „Старт“.$\r$\n$\r$\n\
  Междувременно $\"${DEMO_BG}$\" в менюто „Старт“ показва програмата в действие върху синтетична лента."
LangString RunNow ${LANG_BULGARIAN} "Стартирай HF класификатора сега"
LangString SdrconnectLink ${LANG_BULGARIAN} "SDRconnect на сайта на SDRplay"
LangString UninstallText ${LANG_BULGARIAN} "HF класификаторът ще бъде премахнат. Дневникът и настройките (в %LOCALAPPDATA%\HF Classifier) се запазват; изтрийте и тази папка, ако вече не ви трябват."
LangString Needs64 ${LANG_BULGARIAN} "HF класификаторът изисква 64-битов Windows 10 или 11."
LangString SdrconnectLog ${LANG_BULGARIAN} "SDRconnect: $0"
LangString SecMainName ${LANG_BULGARIAN} "HF класификатор"
LangString SecMainDesc ${LANG_BULGARIAN} "Програмата, инструментите ѝ за команден ред (hf_classifier, sdrc_probe, sdrc_record) и преки пътища в менюто „Старт“ към нея, към демонстрацията ѝ и към диагностичния ѝ отчет."
LangString SecDesktopName ${LANG_BULGARIAN} "Пряк път на работния плот"
LangString SecDesktopDesc ${LANG_BULGARIAN} "Пряк път на работния плот."
LangString SecAutostartName ${LANG_BULGARIAN} "Стартиране с Windows"
LangString SecAutostartDesc ${LANG_BULGARIAN} "HF класификаторът се стартира при влизане в Windows, в областта за уведомяване, без да отваря прозореца си."

; Closes a running HF Classifier as its Exit command does (it stops a
; headless SDRconnect it started and saves the log): WM_CLOSE to its window,
; then up to 10 s to stop, then it is ended. hf_classifier.exe in a console
; is not closed: if its file is in use, copying it asks to retry.
!macro CloseRunning UN
Function ${UN}CloseRunning
  FindWindow $0 "HFClassifierTray"
  ${If} $0 = 0
    Return
  ${EndIf}
  SendMessage $0 ${WM_CLOSE} 0 0
  StrCpy $1 0
  ${Do}
    Sleep 500
    FindWindow $0 "HFClassifierTray"
    ${If} $0 = 0
      Sleep 500  ; the process ends just after its window
      Return
    ${EndIf}
    IntOp $1 $1 + 1
  ${LoopUntil} $1 >= 20
  nsExec::Exec 'taskkill /F /IM ${EXE}'
  Pop $0
  Sleep 500
FunctionEnd
!macroend
!insertmacro CloseRunning ""
!insertmacro CloseRunning "un."

; $SdrconnectFound: SDRconnect's folder (Program Files\SDRplay\SDRconnect)
; exists, or Windows' list of installed programs has an entry whose name
; begins with "SDRconnect". $0: what was found, for the log.
Function FindSdrconnect
  StrCpy $SdrconnectFound "0"
  StrCpy $0 "not found"
  ${If} ${FileExists} "$PROGRAMFILES64\SDRplay\SDRconnect\*.*"
    StrCpy $SdrconnectFound "1"
    StrCpy $0 "$PROGRAMFILES64\SDRplay\SDRconnect"
    Return
  ${EndIf}
  StrCpy $1 0
  ${Do}
    EnumRegKey $2 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall" $1
    ${If} $2 == ""
      ${Break}
    ${EndIf}
    ReadRegStr $3 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\$2" "DisplayName"
    StrCpy $4 $3 10
    ${If} $4 == "SDRconnect"
      StrCpy $SdrconnectFound "1"
      StrCpy $0 "$3 (installed programs)"
      Return
    ${EndIf}
    IntOp $1 $1 + 1
  ${Loop}
FunctionEnd


Section "$(SecMainName)" SecMain
  SectionIn RO
  SetShellVarContext all
  Call CloseRunning
  SetOutPath "$INSTDIR"
  File "${BIN}\${EXE}"
  File "${BIN}\hf_classifier.exe"
  File "${BIN}\sdrc_probe.exe"
  File "${BIN}\sdrc_record.exe"
  File "/oname=README.txt" "${READMEFILE}"
  File "THIRD_PARTY_NOTICES.txt"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Start menu (for all users), named in the installer's language.
  Delete "$SMPROGRAMS\${LINK_EN}.lnk"
  Delete "$SMPROGRAMS\${LINK_BG}.lnk"
  CreateShortcut "$SMPROGRAMS\$(LinkName).lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
  Delete "$SMPROGRAMS\${DEMO_EN}.lnk"
  Delete "$SMPROGRAMS\${DEMO_BG}.lnk"
  CreateShortcut "$SMPROGRAMS\$(DemoLinkName).lnk" "$INSTDIR\${EXE}" "--demo --open" "$INSTDIR\${EXE}" 0
  Delete "$SMPROGRAMS\${REPORT_EN}.lnk"
  Delete "$SMPROGRAMS\${REPORT_BG}.lnk"
  CreateShortcut "$SMPROGRAMS\$(ReportLinkName).lnk" "$INSTDIR\${EXE}" "--report" "$INSTDIR\${EXE}" 0

  WriteRegStr HKLM "${REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "Version" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "$(ProductName)"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "${PRODUCT}"
  WriteRegStr HKLM "${UNINSTKEY}" "URLInfoAbout" "${URL}"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" $0

  Call FindSdrconnect
  DetailPrint "$(SdrconnectLog)"
SectionEnd

Section "$(SecDesktopName)" SecDesktop
  SetShellVarContext all
  Delete "$DESKTOP\${LINK_EN}.lnk"
  Delete "$DESKTOP\${LINK_BG}.lnk"
  CreateShortcut "$DESKTOP\$(LinkName).lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
SectionEnd

; In the Startup folder of all users, so that the uninstaller removes it
; whoever runs it.
Section /o "$(SecAutostartName)" SecAutostart
  SetShellVarContext all
  CreateShortcut "$SMSTARTUP\${LINK_EN}.lnk" "$INSTDIR\${EXE}" "--no-open" "$INSTDIR\${EXE}" 0
SectionEnd

; Optional parts not chosen this time are removed (installed before).
Section "-cleanup"
  SetShellVarContext all
  ${IfNot} ${SectionIsSelected} ${SecDesktop}
    Delete "$DESKTOP\${LINK_EN}.lnk"
    Delete "$DESKTOP\${LINK_BG}.lnk"
  ${EndIf}
  ${IfNot} ${SectionIsSelected} ${SecAutostart}
    Delete "$SMSTARTUP\${LINK_EN}.lnk"
  ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "$(SecMainDesc)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "$(SecDesktopDesc)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutostart} "$(SecAutostartDesc)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; The finish page says what to do next, depending on whether SDRconnect
; was found.
Function FinishPre
  ${If} $SdrconnectFound == "1"
    StrCpy $FinishMsg "$(FinishText)"
  ${Else}
    StrCpy $FinishMsg "$(FinishNoSdrconnect)"
  ${EndIf}
FunctionEnd

; Started by Explorer, so that it runs as the signed-in user, not with the
; installer's administrator rights.
Function RunAsUser
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${EXE}"'
FunctionEnd

; After the sections, whose names it uses.
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(Needs64)" /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  !insertmacro MUI_LANGDLL_DISPLAY
  ; Where a previous version went, unless /D gave a folder.
  ReadRegStr $0 HKLM "${REGKEY}" "InstallDir"
  ${If} $0 != ""
  ${AndIf} $INSTDIR == "$PROGRAMFILES64\${APPDIR}"
    StrCpy $INSTDIR $0
  ${EndIf}
  ; /DESKTOP=0|1 and /AUTOSTART=0|1 choose the optional parts (silent install).
  ${GetOptions} $CMDLINE "/DESKTOP=" $0
  ${If} $0 == "0"
    !insertmacro UnselectSection ${SecDesktop}
  ${ElseIf} $0 == "1"
    !insertmacro SelectSection ${SecDesktop}
  ${EndIf}
  ${GetOptions} $CMDLINE "/AUTOSTART=" $0
  ${If} $0 == "0"
    !insertmacro UnselectSection ${SecAutostart}
  ${ElseIf} $0 == "1"
    !insertmacro SelectSection ${SecAutostart}
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all
  Call un.CloseRunning
  Delete "$INSTDIR\${EXE}"
  Delete "$INSTDIR\hf_classifier.exe"
  Delete "$INSTDIR\sdrc_probe.exe"
  Delete "$INSTDIR\sdrc_record.exe"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${LINK_EN}.lnk"
  Delete "$SMPROGRAMS\${LINK_BG}.lnk"
  Delete "$SMPROGRAMS\${DEMO_EN}.lnk"
  Delete "$SMPROGRAMS\${DEMO_BG}.lnk"
  Delete "$SMPROGRAMS\${REPORT_EN}.lnk"
  Delete "$SMPROGRAMS\${REPORT_BG}.lnk"
  Delete "$DESKTOP\${LINK_EN}.lnk"
  Delete "$DESKTOP\${LINK_BG}.lnk"
  Delete "$SMSTARTUP\${LINK_EN}.lnk"
  DeleteRegKey HKLM "${REGKEY}"
  DeleteRegKey HKLM "${UNINSTKEY}"
SectionEnd

Function un.onInit
  SetRegView 64
  !insertmacro MUI_UNGETLANGUAGE
FunctionEnd
