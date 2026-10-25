<#
.SYNOPSIS
    Downloads harissa64v2.log from the Xbox 360 over FTP and summarizes the
    [hts] lines (hi-res texture pack lookups), so you don't have to pull the
    whole file by hand and paste it into chat every time.

.DESCRIPTION
    Most homebrew FTP servers on Xbox 360 (Aurora/FSD's built-in one, or a
    separate one like iXtreme/FTPDaemon) accept anonymous logins on port 21.
    If yours needs a user/password, pass -User/-Password.

.PARAMETER ConsoleIP
    IP address of the console, e.g. 192.168.1.50

.PARAMETER RemotePath
    FTP path to harissa64v2.log on the console, next to the .xex (for example
    "/Hdd1:/Emulators/Harissa64/harissa64v2.log" if that's where you installed
    it). Defaults to the INSTALL.txt example path.

.PARAMETER User / -Password
    FTP credentials. Leave empty for an anonymous login.

.PARAMETER OutDir
    Where to save the downloaded log (default: tools\logs next to this
    script). Each pull is saved with a timestamp, plus a "latest.log" copy.

.PARAMETER Tail
    How many of the most recent [hts] miss / use-fmt lines to print (the
    in-game logger itself stops after the first 20-60 of each kind, so more
    than that won't show anything new from a single run).

.EXAMPLE
    .\pull_hts_log.ps1 -ConsoleIP 192.168.1.50
    .\pull_hts_log.ps1 -ConsoleIP 192.168.1.50 -RemotePath "/Hdd1:/Emulators/Harissa64/harissa64v2.log"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $ConsoleIP,
    [string] $RemotePath = "/Hdd1:/Emulators/Harissa64/harissa64v2.log",
    [string] $User = "",
    [string] $Password = "",
    [string] $OutDir = (Join-Path $PSScriptRoot "logs"),
    [int] $Tail = 40
)

$ErrorActionPreference = 'Stop'

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

# ---------------------------------------------------------------- download --
Step "Downloading $RemotePath from $ConsoleIP"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$localFile = Join-Path $OutDir "harissa64v2_$stamp.log"
$latestFile = Join-Path $OutDir "harissa64v2_latest.log"

$uri = "ftp://$ConsoleIP$RemotePath"
$request = [System.Net.FtpWebRequest]::Create($uri)
$request.Method = [System.Net.WebRequestMethods+Ftp]::DownloadFile
$request.UsePassive = $true
$request.UseBinary = $true
if ($User) {
    $request.Credentials = New-Object System.Net.NetworkCredential($User, $Password)
}
else {
    $request.Credentials = New-Object System.Net.NetworkCredential("anonymous", "anonymous")
}

try {
    $response = $request.GetResponse()
    $stream = $response.GetResponseStream()
    $fileStream = [System.IO.File]::Create($localFile)
    $stream.CopyTo($fileStream)
    $fileStream.Close()
    $stream.Close()
    $response.Close()
}
catch {
    throw "FTP download failed: $($_.Exception.Message). Check -ConsoleIP, -RemotePath and -User/-Password."
}

Copy-Item $localFile $latestFile -Force
$size = (Get-Item $localFile).Length
Step "Saved $localFile ($size bytes)"

# ---------------------------------------------------------------- summarize --
Step "Summary"
$lines = Get-Content $localFile

$hts = $lines | Where-Object { $_ -match '^\[hts\]' }
$lastSummary = $hts | Where-Object { $_ -match '^\[hts\] lookups ' } | Select-Object -Last 1
$misses = $hts | Where-Object { $_ -match '^\[hts\] miss ' }
$hits = $hts | Where-Object { $_ -match '^\[hts\] hit ' }
$useFmt = $hts | Where-Object { $_ -match 'use fmt' }
$noLoadSrc = $hts | Where-Object { $_ -match 'no load source' }
$srcOutOfRange = $hts | Where-Object { $_ -match 'source out of range' }
$tooLarge = $hts | Where-Object { $_ -match 'too large' }
$notSupported = $hts | Where-Object { $_ -match 'not supported' }

Write-Host ""
Write-Host "Final tally: $lastSummary"
Write-Host "miss lines logged: $($misses.Count)   hit lines logged: $($hits.Count)   use-fmt lines logged: $($useFmt.Count)"
Write-Host "no load source: $($noLoadSrc.Count)   source out of range: $($srcOutOfRange.Count)   too large: $($tooLarge.Count)   not supported: $($notSupported.Count)"

if ($noLoadSrc.Count -gt 0) { Write-Host "`n-- no load source (first 5) --"; $noLoadSrc | Select-Object -First 5 }
if ($srcOutOfRange.Count -gt 0) { Write-Host "`n-- source out of range (first 5) --"; $srcOutOfRange | Select-Object -First 5 }
if ($tooLarge.Count -gt 0) { Write-Host "`n-- too large (first 5) --"; $tooLarge | Select-Object -First 5 }
if ($notSupported.Count -gt 0) { Write-Host "`n-- not supported (first 5) --"; $notSupported | Select-Object -First 5 }

Write-Host "`n-- miss (last $Tail) --"
$misses | Select-Object -Last $Tail

Write-Host "`n-- use fmt (last $Tail) --"
$useFmt | Select-Object -Last $Tail

Write-Host "`nFull log: $localFile"
