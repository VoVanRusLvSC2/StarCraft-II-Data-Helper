; Inno Setup script for maintainers. Build the Release binaries first.
#define AppName "SC2 Data Helper"
#ifndef AppVersion
  #define AppVersion "3.0.0"
#endif
#define AppPublisher "VoVanRusLvSC2"
#define AppExeName "SC2DataHelper.exe"

[Setup]
AppId={{8D6A4F47-0D9B-4F76-9D2B-0B2C7E5B2C31}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\SC2 Data Helper
DefaultGroupName={#AppName}
OutputDir=..\dist
OutputBaseFilename=SC2DataHelper-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64compatible
LicenseFile=..\LICENSE

[Files]
Source: "..\dist\SC2DataHelper-{#AppVersion}-win64\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion; Excludes: "vc_redist.x64.exe,logs\*,scripts\__pycache__\*,*.pyc"
Source: "..\dist\SC2DataHelper-{#AppVersion}-win64\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\SC2 Map Optimizer (GUI)"; Filename: "{app}\{#AppExeName}"
Name: "{commondesktop}\SC2 Map Optimizer (GUI)"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut for SC2 Map Optimizer (GUI)"

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: waituntilterminated
Filename: "{app}\{#AppExeName}"; Description: "Launch SC2 Map Optimizer (GUI)"; Flags: nowait postinstall skipifsilent
