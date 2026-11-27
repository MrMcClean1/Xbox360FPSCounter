# Makes a Peer Pressure sideload ini for one game and stages the files to copy.
#
# Usage:  .\make-ini.ps1 -TitleID 4D5307E6
#         .\make-ini.ps1 -TitleID 4D5307E6 -Dll fps_hello.xex
#
# Output goes to out\sideload\ . FTP everything in that folder to
#   Hdd1:\PeerPressure\SideLoading\

param(
    [Parameter(Mandatory = $true)][string]$TitleID,
    [string]$Dll = "fps_hello.xex"
)

$id = $TitleID.Trim().ToUpper().Replace("0X", "")
if ($id -notmatch '^[0-9A-F]{8}$') {
    Write-Host "TitleID must be 8 hex characters, like 4D5307E6. You gave: $TitleID"
    exit 1
}

$src = Join-Path "out" $Dll
if (-not (Test-Path $src)) {
    Write-Host "Cannot find $src . Run .\build.bat first."
    exit 1
}

$dir = "out\sideload"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Copy-Item $src (Join-Path $dir $Dll) -Force

$ini = @"
[DllConfig]
TitleID = 0x$id
ModuleVersion = "*.*.*.*"
FileName = "$Dll"
"@

$iniPath = Join-Path $dir ($id.ToLower() + ".ini")
Set-Content -Path $iniPath -Value $ini -Encoding ASCII

Write-Host "Wrote $iniPath"
Write-Host "FTP the contents of $dir to Hdd1:\PeerPressure\SideLoading\"
