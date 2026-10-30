<#
.SYNOPSIS
    Builds LAIKA 64 and packages a release zip ready for an RGH/JTAG Xbox 360.

.DESCRIPTION
    1. Rebuilds the Release | Xbox 360 configuration with Visual Studio 2010
       and the Xbox 360 SDK (skip with -NoBuild).
    2. Checks the output (a fresh LAIKA64.xex with the title data).
    3. Assembles release\LAIKA 64\ and zips it to
       release\LAIKA 64-<version>.zip, where <version> comes from VERSION.

    The zip holds one folder, LAIKA 64\, to extract and copy as-is to the
    console (for example to Hdd1:\Emulators\):

      LAIKA 64\
        default.xex           the emulator (dashboards scan for default.xex)
        roms\                 put your games here
        art\                  cover, background and icon for the dashboard
                              (art\games\ is made on the console: box art)
        tools\                fetch_game_art.py (box art from a PC)
        README.md, LICENSE, THIRD_PARTY.md, INSTALL.txt

    Saves and settings are created by the emulator on the console. No ROMs,
    saves, logs or local settings are ever packaged.

.PARAMETER NoBuild
    Package the existing build instead of rebuilding.

.EXAMPLE
    .\build_release.ps1
    .\build_release.ps1 -NoBuild
#>
[CmdletBinding()]
param(
    [switch] $NoBuild
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$version = (Get-Content (Join-Path $repo 'VERSION') -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+(\.\d+)?$') { throw "VERSION must look like 1.2 or 1.2.3 (got '$version')." }

$build = Join-Path $repo 'platform\xbox360\Release'
$built = Join-Path $build 'harissa64v2.xex'   # what the XDK's packaging step names it
$xex = Join-Path $build 'LAIKA64.xex'        # the executable's name, copied from it
$art = Join-Path $repo 'platform\xbox360\title\art'
$releaseDir = Join-Path $repo 'release'
$stage = Join-Path $releaseDir 'LAIKA 64'
$zip = Join-Path $releaseDir "LAIKA 64-$version.zip"

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

# ---------------------------------------------------------------- build --
if (-not $NoBuild) {
    Step "Building Release | Xbox 360"
    $vcvars = 'E:\tools\Visual Studio 10.0\VC\vcvarsall.bat'
    if (-not (Test-Path $vcvars)) {
        $vsTools = [Environment]::GetEnvironmentVariable('VS100COMNTOOLS', 'Machine')
        if (-not $vsTools) { $vsTools = $env:VS100COMNTOOLS }
        if ($vsTools) { $vcvars = Join-Path $vsTools '..\..\VC\vcvarsall.bat' }
    }
    if (-not (Test-Path $vcvars)) { throw "Visual Studio 2010 (vcvarsall.bat) not found." }
    $msbuild = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\MSBuild.exe'
    $sln = Join-Path $repo 'platform\xbox360\harissa64v2.sln'
    $log = Join-Path $env:TEMP 'harissa64_release_build.log'
    $started = Get-Date
    # The XEX packaging step does not always run again on its own: start from no xex.
    if (Test-Path $built) { Remove-Item -LiteralPath $built -Force }
    cmd /c "`"$vcvars`" x86 >nul && `"$msbuild`" `"$sln`" /t:Rebuild /p:Configuration=Release /p:Platform=`"Xbox 360`" /m /nologo /v:minimal /fl /flp:logfile=`"$log`";verbosity=normal"
    if ($LASTEXITCODE -ne 0) { throw "Build failed (see $log)." }
    if (-not (Test-Path $built) -or (Get-Item $built).LastWriteTime -lt $started) {
        throw "Build finished but $built was not regenerated (see $log)."
    }
    Copy-Item $built $xex -Force
}

# ---------------------------------------------------------------- checks --
Step "Checking build output"
if (-not (Test-Path $xex)) { throw "Missing $xex. Build first or drop -NoBuild." }
foreach ($f in 'cover.png', 'background.png', 'icon.png') {
    if (-not (Test-Path (Join-Path $art $f))) { throw "Missing $(Join-Path $art $f)." }
}

# ---------------------------------------------------------------- stage --
Step "Staging release\LAIKA 64"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $xex (Join-Path $stage 'default.xex')
Copy-Item (Join-Path $repo 'README.md'), (Join-Path $repo 'LICENSE'), (Join-Path $repo 'THIRD_PARTY.md') $stage
# The folders the emulator uses (it makes any missing one at each start; here so they can be filled first).
foreach ($d in 'roms', 'wallpapers', 'sounds', 'saves', 'screenshots', 'hires_texture') {
    New-Item -ItemType Directory -Force (Join-Path $stage $d) | Out-Null
}
# The setup assistant's music (tools/gen_wizard_music.py).
New-Item -ItemType Directory -Force (Join-Path $stage 'assistant') | Out-Null
Copy-Item (Join-Path $repo 'assets\assistant\laika_orbit.mp3') (Join-Path $stage 'assistant')
# ... and the menus' default music (Config::musicTrack).
New-Item -ItemType Directory -Force (Join-Path $stage 'music') | Out-Null
Copy-Item (Join-Path $repo 'assets\assistant\laika_orbit.mp3') (Join-Path $stage 'music')
New-Item -ItemType Directory -Force (Join-Path $stage 'art') | Out-Null
foreach ($f in 'cover.png', 'background.png', 'icon.png') { Copy-Item (Join-Path $art $f) (Join-Path $stage 'art') }
# The PC script that fills art\games\ (box art, game information) for consoles without internet.
New-Item -ItemType Directory -Force (Join-Path $stage 'tools') | Out-Null
Copy-Item (Join-Path $repo 'tools\fetch_game_art.py'), (Join-Path $repo 'tools\make_hts.py') (Join-Path $stage 'tools')

$install = @"
LAIKA 64 $version - Nintendo 64 emulator for Xbox 360 (RGH/JTAG)
==================================================================

1. Copy the whole LAIKA 64 folder to your console,
   for example to Hdd1:\Emulators\LAIKA 64\

2. Put your own games (.z64, .n64, .v64 or .zip) in LAIKA 64\roms\
   File names must be at most 42 characters, without commas, '+', ';', '='
   or accents (limits of the Xbox 360 file system).

3. In Aurora, add the parent folder (e.g. Hdd1:\Emulators\) to the scan
   paths, or browse to LAIKA 64 and launch default.xex. It shows up as
   "LAIKA 64". The art folder has a cover and a background you can load
   from the game's asset menu in Aurora.

Folders (LAIKA 64 makes any missing one by itself):
   roms\           your games
   wallpapers\     your own JPG or PNG backgrounds for the menus
   music\          MP3 or WAV music for the menus
   sounds\         your own interface sounds
   hires_texture\  HD texture packs (.hts), one folder per game:
                   hires_texture\<GAME NAME>\pack.hts
   saves\          in-game saves and save states (back this one up)
   screenshots\    screenshots taken in game

Box art: when the console is online, the game list downloads the box art,
screenshots, logos and information of your games into LAIKA 64\art\games\.
Without internet, run tools\fetch_game_art.py on a PC (Python 3) and copy
the art\games folder it makes into LAIKA 64\.

Updates: when a new version is out, the game list offers to install it.

In a game: a short press on BACK opens the menu (save and load states,
settings, back to the game list). BACK + RB saves a state, BACK + LB loads it.
Your saves go to LAIKA 64\saves\.

Games we tested and their speed: docs/games.md on the project page.
LAIKA 64: https://github.com/ErickaguirreD/Laika64
Based on Harissa64 by kernel64: https://github.com/kernel64/Harissa64
Licence: GNU GPL v2 (see LICENSE).
"@
[IO.File]::WriteAllText((Join-Path $stage 'INSTALL.txt'), $install.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# ---------------------------------------------------------------- zip --
Step "Creating $zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
# CreateFromDirectory keeps the empty roms\ folder.
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)

# Sanity check: no ROMs or local files slipped in.
$zipFile = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $bad = $zipFile.Entries | Where-Object { $_.FullName -match '(?i)\.(z64|n64|v64|log|ini|h64s|bin|bmp)$' }
    if ($bad) { throw "Unexpected files in the zip: $($bad.FullName -join ', ')" }
    $count = $zipFile.Entries.Count
}
finally { $zipFile.Dispose() }

# The updater's files (attached to the GitHub release next to the zip): default.xex and its SHA-1,
# which the console checks before it puts a downloaded version in place (xb_update.cpp).
$sha = (Get-FileHash -Algorithm SHA1 -LiteralPath $xex).Hash.ToLower()
[IO.File]::WriteAllText((Join-Path $releaseDir 'default.xex.sha1'), "$sha  default.xex`n")
Copy-Item $xex (Join-Path $releaseDir 'default.xex') -Force

$size = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Step "Done: release\LAIKA 64-$version.zip ($size MB, $count entries)"
