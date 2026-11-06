$InnoCompiler = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if (-not (Test-Path -Path $InnoCompiler))
{
    Write-Host "ISCC.exe not found. Exiting."
    exit
}

$Version = (Select-String -Path CMakeLists.txt -Pattern 'project\s*\([^)]*?VERSION\s+([\d.]+)').Matches[0].Groups[1].Value
$ReleaseBuildOutput = [System.IO.Path]::GetFullPath(".\build\release\bin")
$ReleaseDestination = ".\Release\v$Version"
if (Test-Path -Path $ReleaseDestination)
{
    Remove-Item -Force -Recurse $ReleaseDestination
}
New-Item -Path $ReleaseDestination -ItemType Directory

$InnoSpecPath = "$ReleaseDestination\Spec.iss"
$ReleaseFilename = "Tori-$Version-amd64"
$InnoSpec = @"
[Setup]
AppName=Tori
AppVersion=$Version
DefaultDirName={localappdata}\Programs\Tori
DefaultGroupName=Tori
OutputDir=.
OutputBaseFilename=$ReleaseFilename
Compression=lzma2
SolidCompression=yes
SetupIconFile=..\..\Resource\icon.ico
PrivilegesRequired=lowest
VersionInfoVersion=$Version

[Files]
Source: "$ReleaseBuildOutput\Tori.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "$ReleaseBuildOutput\ToriCli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "$ReleaseBuildOutput\ToriCore.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "$ReleaseBuildOutput\XenUI.dll"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Icons]
Name: "{group}\Tori"; Filename: "{app}\Tori.exe"
Name: "{autodesktop}\Tori"; Filename: "{app}\Tori.exe"; Tasks: desktopicon
"@

Set-Content -Path $InnoSpecPath -Value $InnoSpec -Encoding UTF8
& $InnoCompiler $InnoSpecPath
Remove-Item $InnoSpecPath

$OutputFile = "$ReleaseDestination\$ReleaseFilename.exe"
& gpg --yes --local-user "AFE7640913AD240BC7A5D84778DBEC4C03F1056A" --detach-sign --armor --output "$OutputFile.sig" $OutputFile > $null
Copy-Item "$env:HOMEPATH\.gpg\Jake_Rieger_0x78DBEC4C03F1056A_public.asc" "$ReleaseDestination\signature.asc" > $null

Write-Host ""
Write-Host "== Generated release -> $ReleaseDestination\$ReleaseFilename.exe"

exit $LASTEXITCODE