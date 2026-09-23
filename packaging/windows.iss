#ifndef Payload
#error Payload is required
#endif
#ifndef Output
#error Output is required
#endif
#ifndef Arch
#error Arch is required
#endif
#ifndef Version
#error Version is required
#endif
#ifndef PackageName
#error PackageName is required
#endif

[Setup]
SetupArchitecture=x86
AppId=RingAire.RingEcho.{#Arch}
AppName=RingEcho
AppVersion={#Version}
VersionInfoVersion={#NumericVersion}
AppPublisher=RingAire
DefaultDirName={localappdata}\Programs\RingEcho\{#Arch}
DefaultGroupName=RingEcho
PrivilegesRequired=lowest
OutputDir={#Output}
OutputBaseFilename={#PackageName}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\bin\rev.exe
LicenseFile={#Payload}\LICENSE
#if Arch == "x64"
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#elif Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#elif Arch == "x86"
ArchitecturesAllowed=x86compatible
#else
#error Unsupported architecture
#endif

[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\使用说明"; Filename: "{app}\INSTALL.zh.md"
Name: "{group}\卸载 RingEcho"; Filename: "{uninstallexe}"
