[Setup]
; Application Information
AppName=Tori
AppVersion=1.0.0
AppPublisher=Jake Rieger
AppPublisherURL=https://github.com/jakerieger/Tori
AppSupportURL=https://github.com/jakerieger/Tori
AppUpdatesURL=https://github.com/jakerieger/Tori
DefaultDirName={autopf}\Tori
DisableProgramGroupPage=yes
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=.
OutputBaseFilename=Tori_Setup_1.0.0_Windows_x64
SetupIconFile=icon.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "addtopath"; Description: "Add Tori to the system PATH environment variable"; GroupDescription: "Installation options:"

[Files]
; Main executable
Source: "..\build\Release\bin\tori.exe"; DestDir: "{app}"; Flags: ignoreversion
; All DLL files
Source: "..\build\Release\bin\*.dll"; DestDir: "{app}"; Flags: ignoreversion

[Code]
const
  EnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';

procedure EnvAddPath(Path: string);
var
  Paths: string;
begin
  { Retrieve current path }
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
  begin
    Paths := '';
  end;

  { Skip if string already found in path }
  if Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(Paths) + ';') > 0 then exit;
  if Pos(';' + Uppercase(Path) + '\;', ';' + Uppercase(Paths) + ';') > 0 then exit;

  { App dir found in path, skip }
  if Uppercase(Paths) = Uppercase(Path) then exit;

  { Append App Install Path to the end of the path variable }
  if Paths = '' then
    Paths := Path
  else
    Paths := Paths + ';' + Path;

  { Overwrite (or create if missing) path environment variable }
  if RegWriteStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
  begin
    Log(Format('The [%s] added to PATH: [%s]', [Path, Paths]));
  end
  else
  begin
    Log(Format('Error adding [%s] to PATH', [Path]));
  end;
end;

procedure EnvRemovePath(Path: string);
var
  Paths: string;
  P: Integer;
begin
  { Skip if registry entry not exists }
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    exit;

  { Skip if string not found in path }
  P := Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(Paths) + ';');
  if P = 0 then exit;

  { Update path variable }
  Delete(Paths, P - 1, Length(Path) + 1);

  { Overwrite path environment variable }
  if RegWriteStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
  begin
    Log(Format('The [%s] removed from PATH: [%s]', [Path, Paths]));
  end
  else
  begin
    Log(Format('Error removing [%s] from PATH', [Path]));
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
  begin
    EnvAddPath(ExpandConstant('{app}'));
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    EnvRemovePath(ExpandConstant('{app}'));
  end;
end;