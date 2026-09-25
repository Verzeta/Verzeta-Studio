; SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; verzeta-studio.iss
; ------------------
; Inno Setup 6 script for Verzeta Studio Windows installer.
;
; Run deploy-windows.ps1 FIRST to populate ..\..\deploy-windows\
; (relative to this script's location at packaging\windows\), then
; compile this script with:
;
;   & "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" packaging\windows\verzeta-studio.iss
;
; or just run:
;
;   .\packaging\windows\deploy-windows.ps1 -BuildInstaller

#define AppName      "Verzeta Studio"
; AppVersion is read at compile time from /VERSION at the repo root
; (single source of truth shared with CMake, Craft.yaml, Windows.yaml,
; and Release.yaml). To bump, edit /VERSION once; everything else
; re-derives. Two levels up from packaging/windows/ → repo root.
#define VersionFile FileOpen(SourcePath + "..\..\VERSION")
#define AppVersion   Trim(FileRead(VersionFile))
#expr FileClose(VersionFile)
#define AppPublisher "Aditya Mehra"
#define AppURL       "https://verzeta.com"
#define AppExeName   "verzeta-studio.exe"
; Two levels up because this .iss now lives at packaging\windows\
; while the deploy bundle stays at the repo root's deploy-windows\.
#define DeployDir    "..\..\deploy-windows"

[Setup]
AppId={{A7F3C2D1-8E4B-4A9F-B6C3-D2E1F0A8B5C4}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes
; Single .exe output
OutputDir=Output
OutputBaseFilename=verzeta-studio-{#AppVersion}-windows-x64-setup
; Compression
Compression=lzma2/ultra64
SolidCompression=yes
LZMANumBlockThreads=4
; Require 64-bit Windows
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Require admin for Program Files install
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
; Visual
WizardStyle=modern
; Minimum Windows version: Windows 10
MinVersion=10.0.17763
; Don't leave a footprint if install is cancelled
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\{#AppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon";    Description: "{cm:CreateDesktopIcon}";    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startmenuicon";  Description: "Create a Start Menu shortcut"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
; Everything in the deploy bundle — recurse subdirs for Qt plugins, QML, etc.
Source: "{#DeployDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}";          Filename: "{app}\{#AppExeName}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";    Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
; Offer to launch immediately after install
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Clean up user-created files that Setup didn't place (logs, cache)
; User data in AppData is intentionally left intact.
Type: filesandordirs; Name: "{app}"
