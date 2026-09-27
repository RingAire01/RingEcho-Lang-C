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
AppId=RingAire.RingEcho.{#Arch}
AppName=RingEcho
AppVersion={#Version}
VersionInfoVersion={#NumericVersion}
AppPublisher=RingAire
DefaultDirName={localappdata}\Programs\RingEcho\{#Arch}
DefaultGroupName=RingEcho
PrivilegesRequired=lowest
ChangesEnvironment=yes
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

[Code]
procedure AddToPath(BinDir: String);
var
  OrigPath: String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
  begin
    RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', BinDir);
    exit;
  end;
  if Pos(';' + Uppercase(BinDir) + ';', ';' + Uppercase(OrigPath) + ';') = 0 then
  begin
    if (OrigPath <> '') and (OrigPath[Length(OrigPath)] <> ';') then
      OrigPath := OrigPath + ';';
    RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath + BinDir);
  end;
end;

procedure RemoveFromPath(BinDir: String);
var
  OrigPath: String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
    exit;
  StringChangeEx(OrigPath, BinDir + ';', '', True);
  StringChangeEx(OrigPath, ';' + BinDir, '', True);
  if OrigPath = BinDir then
    OrigPath := '';
  RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    AddToPath(ExpandConstant('{app}\bin'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveFromPath(ExpandConstant('{app}\bin'));
end;
