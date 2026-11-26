; Inno Setup 6 script for Reel Player.
; Build via:  .\publish.ps1 -Installer      (passes SourceDir and Arch)
; or:         iscc /DSourceDir=..\dist\ReelPlayer-win-x64 /DArch=x64 installer\ReelPlayer.iss

#ifndef SourceDir
  #define SourceDir "..\dist\ReelPlayer-win-x64"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#define AppName "Reel Player"
#define AppVersion GetVersionNumbersString(SourceDir + "\ReelPlayer.exe")
#define AppExe "ReelPlayer.exe"
#define ProgId "ReelPlayer.Media"

[Setup]
AppId={{6F1F3C2B-7C59-4C3B-9E0B-2D8A4E5B7A11}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Reel Player contributors
DefaultDirName={autopf}\Reel Player
DefaultGroupName=Reel Player
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=..\dist
OutputBaseFilename=ReelPlayer-Setup-{#Arch}
SetupIconFile=..\cpp\resources\app.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
#if Arch == "x64"
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#elif Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#endif
LicenseFile=..\THIRD-PARTY-NOTICES.txt

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associate"; Description: "Add Reel Player to ""Open with"" for common video and audio files"; GroupDescription: "File types:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Reel Player"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall Reel Player"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Reel Player"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; ProgID + "Open with" entries. Windows asks the user before changing the actual default app.
Root: HKA; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueData: "Reel Player media file"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueData: """{app}\{#AppExe}"",0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Reel Player"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
#define Exts "mp4 m4v mkv mka webm avi mov wmv asf flv ts m2ts mts mpg mpeg vob ogv ogg 3gp mxf rm rmvb divx mp3 aac m4a flac wav wma opus ac3 dts ape wv m3u m3u8 pls xspf"
#sub AddExt
  #define Ext Copy(Exts, 1, Pos(" ", Exts + " ") - 1)
Root: HKA; Subkey: "Software\Classes\.{#Ext}\OpenWithProgids"; ValueType: none; ValueName: "{#ProgId}"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".{#Ext}"; ValueData: ""; Tasks: associate
  #define public Exts Copy(Exts, Pos(" ", Exts + " ") + 1, 999)
#endsub
#for {0; Len(Exts) > 0; 0} AddExt

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,Reel Player}"; Flags: nowait postinstall skipifsilent
