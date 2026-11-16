; RadioTray Inno Setup script.
; Build with installer\build.ps1, or compile manually:
;   ISCC.exe /DSourceExe="..\build\Release\RadioTray.exe" RadioTray.iss

#ifndef SourceExe
  #define SourceExe "..\build\Release\RadioTray.exe"
#endif

#define AppName      "RadioTray"
#define AppVersion   "1.1.0"
#define AppPublisher "lad0shka"

[Setup]
AppId={{6F1C2B8E-3D4A-4E59-9B7F-2A8C1D0E5F31}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Per-user install: no administrator rights needed.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=..\source\app.ico
UninstallDisplayIcon={app}\RadioTray.exe
UninstallDisplayName={#AppName}
OutputDir=Output
OutputBaseFilename=RadioTray-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Ask to close a running copy (upgrade or uninstall) instead of failing on locked files.
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName={#AppName}
VersionInfoCompany={#AppPublisher}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart";   Description: "Start RadioTray when Windows starts (silently, without starting playback)"; Flags: checkedonce
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SourceExe}";            DestDir: "{app}"; Flags: ignoreversion
Source: "..\BASS-LICENSE.txt";     DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";                  Filename: "{app}\RadioTray.exe"
Name: "{group}\Uninstall {#AppName}";        Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";            Filename: "{app}\RadioTray.exe"; Tasks: desktopicon

[Registry]
; Same value the app writes from its "Start with Windows" menu item.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "RadioTray"; ValueData: """{app}\RadioTray.exe"""; Flags: uninsdeletevalue; Tasks: autostart
; Remove the saved station selection on uninstall only.
Root: HKCU; Subkey: "Software\RadioTray"; Flags: dontcreatekey uninsdeletekey

[UninstallDelete]
; Embedded-library cache created by the application.
Type: filesandordirs; Name: "{localappdata}\RadioTray"

[Run]
Filename: "{app}\RadioTray.exe"; Description: "Launch RadioTray"; Flags: nowait postinstall skipifsilent
