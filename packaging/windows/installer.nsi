; The launcher's Windows installer. It installs where Tauri's did, under the same registry keys,
; so it upgrades an installed Rust launcher in place, and it takes the same options, so the
; launcher's updater runs it as tauri-plugin-updater ran Tauri's:
;
;   /S        silent: no window
;   /P        passive: a progress window only, no questions
;   /R        (with /S or /P) start the launcher when done, with the arguments after /ARGS
;   /UPDATE   an update: don't install WebView2
;
; Launchers up to 0.2.6 also came as an .msi, installed for all users in Program Files. This
; installer can't replace that copy (it installs per user), so it finds it and offers to uninstall
; it: a button on its own page, or a question at the end of a passive update. msiexec asks for
; administrator rights then, after the player chose to remove it.
;
; Built by packaging/nsis.sh, which defines VERSION, EXE, TOOLS, ICON, WEBVIEW2 and OUTFILE.

Unicode true
SetCompressor /SOLID lzma
ManifestDPIAware true

!include MUI2.nsh
!include FileFunc.nsh
!include LogicLib.nsh
!include nsDialogs.nsh

!define PRODUCTNAME "STAR FORGE Launcher"
!define MAINBINARY "star-forge.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCTNAME}"
!define MANUPRODUCTKEY "Software\star-forge\${PRODUCTNAME}"

Name "${PRODUCTNAME}"
OutFile "${OUTFILE}"
; Per user, like Tauri's default: no administrator needed.
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\${PRODUCTNAME}"
InstallDirRegKey HKCU "${MANUPRODUCTKEY}" ""

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCTNAME}"
VIAddVersionKey "FileDescription" "${PRODUCTNAME}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" ""

Var PassiveMode
Var UpdateMode
Var MsiCode     ; product code of an .msi install of the launcher, or ""
Var MsiButton
Var MsiStatus

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING

Page custom MsiPage
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipIfPassive
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION RunLauncher
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Create a desktop shortcut"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION DesktopShortcut
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipIfPassive
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${GetOptions} $CMDLINE "/P" $0
  ${IfNot} ${Errors}
    StrCpy $PassiveMode 1
  ${EndIf}
  ${GetOptions} $CMDLINE "/UPDATE" $0
  ${IfNot} ${Errors}
    StrCpy $UpdateMode 1
  ${EndIf}
  Call FindMsiCopy
FunctionEnd

; --- an .msi install from 0.2 -------------------------------------------------------------------

!macro FindMsiIn ROOT
  ${If} $MsiCode == ""
    StrCpy $R0 0
    ${Do}
      EnumRegKey $R1 ${ROOT} "Software\Microsoft\Windows\CurrentVersion\Uninstall" $R0
      ${If} $R1 == ""
        ${ExitDo}
      ${EndIf}
      ReadRegStr $R2 ${ROOT} "Software\Microsoft\Windows\CurrentVersion\Uninstall\$R1" "DisplayName"
      ReadRegDWORD $R3 ${ROOT} "Software\Microsoft\Windows\CurrentVersion\Uninstall\$R1" "WindowsInstaller"
      ${If} $R2 == "${PRODUCTNAME}"
      ${AndIf} $R3 = 1
        StrCpy $MsiCode $R1
        ${ExitDo}
      ${EndIf}
      IntOp $R0 $R0 + 1
    ${Loop}
  ${EndIf}
!macroend

; Sets $MsiCode to the .msi install's product code ({GUID}), if there is one.
Function FindMsiCopy
  StrCpy $MsiCode ""
  SetRegView 64
  !insertmacro FindMsiIn HKLM
  SetRegView 32
  !insertmacro FindMsiIn HKLM
  SetRegView default
  !insertmacro FindMsiIn HKCU
FunctionEnd

; Uninstalls it (msiexec asks for administrator rights). $R9 = msiexec's exit code.
Function UninstallMsiCopy
  ExecWait '"$SYSDIR\msiexec.exe" /x $MsiCode /passive /norestart' $R9
  Call FindMsiCopy
FunctionEnd

Function MsiPage
  ${If} $PassiveMode = 1
  ${OrIf} $MsiCode == ""
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Older version found" "An older ${PRODUCTNAME} is installed for all users."
  nsDialogs::Create 1018
  Pop $0
  ${NSD_CreateLabel} 0 0 100% 64u "${PRODUCTNAME} 0.2, installed for all users from its .msi, is still on this computer. This version installs for your user only, so the old copy would stay in Program Files with its own Start menu entry.$\r$\n$\r$\nYou can remove it now, or later under Settings > Apps. Your profiles and settings are kept either way. Removing it needs administrator rights: Windows will ask."
  Pop $0
  ${NSD_CreateButton} 0 72u 60% 16u "Uninstall the old version"
  Pop $MsiButton
  ${NSD_OnClick} $MsiButton OnUninstallMsi
  ${NSD_CreateLabel} 0 96u 100% 32u ""
  Pop $MsiStatus
  nsDialogs::Show
FunctionEnd

Function OnUninstallMsi
  Pop $0
  EnableWindow $MsiButton 0
  ${NSD_SetText} $MsiStatus "Uninstalling the old version..."
  Call UninstallMsiCopy
  ${If} $MsiCode == ""
    ${NSD_SetText} $MsiStatus "The old version is uninstalled. Press Next to continue."
  ${Else}
    ${NSD_SetText} $MsiStatus "The old version is still installed (msiexec exit code $R9). Try again, or remove it later under Settings > Apps."
    EnableWindow $MsiButton 1
  ${EndIf}
FunctionEnd

; A passive update from an .msi install has no pages: ask once it's done.
Function AskAboutMsiCopy
  ${If} $MsiCode == ""
  ${OrIf} ${Silent}
    Return
  ${EndIf}
  MessageBox MB_YESNO|MB_ICONQUESTION "${PRODUCTNAME} ${VERSION} is installed for your user. The older version, installed for all users from its .msi, is still in Program Files.$\r$\n$\r$\nUninstall the old version now? Windows will ask for administrator rights. Your profiles and settings are kept." /SD IDNO IDNO done
  Call UninstallMsiCopy
  ${If} $MsiCode != ""
    MessageBox MB_OK|MB_ICONEXCLAMATION "The old version is still installed (msiexec exit code $R9). You can remove it under Settings > Apps." /SD IDOK
  ${EndIf}
  done:
FunctionEnd

Function SkipIfPassive
  ${If} $PassiveMode = 1
    Abort
  ${EndIf}
FunctionEnd

Function RunLauncher
  Exec '"$INSTDIR\${MAINBINARY}"'
FunctionEnd

Function DesktopShortcut
  CreateShortcut "$DESKTOP\${PRODUCTNAME}.lnk" "$INSTDIR\${MAINBINARY}"
FunctionEnd

; Waits for a running launcher to let go of its exe (an update starts while it is exiting);
; asks the player to close it if it doesn't.
Function WaitForLauncher
  IfFileExists "$INSTDIR\${MAINBINARY}" 0 done
  StrCpy $1 0
  retry:
    ClearErrors
    FileOpen $0 "$INSTDIR\${MAINBINARY}" a
    ${IfNot} ${Errors}
      FileClose $0
      Goto done
    ${EndIf}
    IntOp $1 $1 + 1
    ${If} $1 < 20
      Sleep 500
      Goto retry
    ${EndIf}
    ${If} ${Silent}
      Abort "${PRODUCTNAME} is running."
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${PRODUCTNAME} is running. Close it, then retry." /SD IDCANCEL IDRETRY retry
    Abort "${PRODUCTNAME} is running."
  done:
FunctionEnd

; WebView2 shows the launcher's window. Windows 11 has it; older systems may not, so the
; installer runs Microsoft's bootstrapper (it downloads the runtime).
Function WebView2
  ${If} $UpdateMode = 1
    Return
  ${EndIf}
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  ${If} $0 == ""
    ReadRegStr $0 HKCU "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  ${EndIf}
  ${If} $0 != ""
  ${AndIf} $0 != "0.0.0.0"
    Return
  ${EndIf}
  DetailPrint "Installing WebView2..."
  File "/oname=$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" "${WEBVIEW2}"
  ExecWait '"$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" /silent /install' $1
  ${If} $1 <> 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "WebView2 could not be installed (error $1). The launcher needs it to show its window." /SD IDOK
  ${EndIf}
FunctionEnd

Section Install
  InitPluginsDir
  Call WaitForLauncher
  Call WebView2

  SetOutPath $INSTDIR
  File "${EXE}"
  SetOutPath "$INSTDIR\tools"
  File "${TOOLS}\sc-inject.exe"
  File "${TOOLS}\sc_agent.dll"
  File "${TOOLS}\sc-server.exe"
  SetOutPath $INSTDIR
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; starforge:// links.
  WriteRegStr HKCU "Software\Classes\starforge" "" "URL:star-forge protocol"
  WriteRegStr HKCU "Software\Classes\starforge" "URL Protocol" ""
  WriteRegStr HKCU "Software\Classes\starforge\DefaultIcon" "" '"$INSTDIR\${MAINBINARY}",0'
  WriteRegStr HKCU "Software\Classes\starforge\shell\open\command" "" '"$INSTDIR\${MAINBINARY}" "%1"'

  ; Where it is, for the next install; and Apps & features.
  WriteRegStr HKCU "${MANUPRODUCTKEY}" "" $INSTDIR
  WriteRegStr HKCU "${UNINSTKEY}" "MainBinaryName" "${MAINBINARY}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${PRODUCTNAME}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" '"$INSTDIR\${MAINBINARY}"'
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "star-forge"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" '"$INSTDIR"'
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" $0

  CreateShortcut "$SMPROGRAMS\${PRODUCTNAME}.lnk" "$INSTDIR\${MAINBINARY}"
  ; The finish page asks about the desktop shortcut; without it, make one (as Tauri's does),
  ; except on updates, where the player may have deleted it.
  ${If} $PassiveMode = 1
  ${OrIf} ${Silent}
    ${If} $UpdateMode <> 1
      Call DesktopShortcut
    ${EndIf}
    SetAutoClose true
  ${EndIf}
SectionEnd

; Everything after " /ARGS " on the command line, as it is (GetOptions would stop at any "/",
; as in a starforge:// link).
Function ArgsAfterArgs
  StrLen $2 $CMDLINE
  StrCpy $1 0
  loop:
    IntCmp $1 $2 none 0 none
    StrCpy $3 $CMDLINE 6 $1
    ${If} $3 == " /ARGS"
      IntOp $1 $1 + 6
      StrCpy $3 $CMDLINE "" $1
      ; Drop the space after it.
      StrCpy $4 $3 1
      ${If} $4 == " "
        StrCpy $3 $3 "" 1
      ${EndIf}
      Push $3
      Return
    ${EndIf}
    IntOp $1 $1 + 1
    Goto loop
  none:
  Push ""
FunctionEnd

Function .onInstSuccess
  ${If} $PassiveMode = 1
    Call AskAboutMsiCopy
  ${EndIf}
  ${If} $PassiveMode = 1
  ${OrIf} ${Silent}
    ${GetOptions} $CMDLINE "/R" $0
    ${IfNot} ${Errors}
      Call ArgsAfterArgs
      Pop $0
      Exec '"$INSTDIR\${MAINBINARY}" $0'
    ${EndIf}
  ${EndIf}
FunctionEnd

Section Uninstall
  Delete "$INSTDIR\${MAINBINARY}"
  Delete "$INSTDIR\tools\sc-inject.exe"
  Delete "$INSTDIR\tools\sc_agent.dll"
  Delete "$INSTDIR\tools\sc-server.exe"
  RMDir "$INSTDIR\tools"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${PRODUCTNAME}.lnk"
  Delete "$DESKTOP\${PRODUCTNAME}.lnk"

  ; The link handler, if it is still this launcher's.
  ReadRegStr $0 HKCU "Software\Classes\starforge\shell\open\command" ""
  ${If} $0 == '"$INSTDIR\${MAINBINARY}" "%1"'
    DeleteRegKey HKCU "Software\Classes\starforge"
  ${EndIf}
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "${MANUPRODUCTKEY}"
  DeleteRegKey /ifempty HKCU "Software\star-forge"
  ; Profiles, settings and downloaded packs stay: they are the player's.
SectionEnd
