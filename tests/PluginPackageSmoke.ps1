# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory)] [string] $Repository,
    [Parameter(Mandatory)] [string] $Version
)
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:RUNNER_TEMP ('execpp-wingup-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $root | Out-Null

function Save-Archive($url, $name, $destination) {
    $archive = Join-Path $root $name
    Invoke-WebRequest -Uri $url -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $destination
    return $archive
}

$release = Invoke-RestMethod "https://api.github.com/repos/$Repository/releases/tags/v$Version"
$name = "exec-plus-plus-v$Version-plugins-admin-x64.zip"
$asset = $release.assets | Where-Object name -eq $name
if (!$asset) { throw "The release is missing $name." }
$candidate = Join-Path $root 'candidate'
$archive = Save-Archive $asset.browser_download_url $name $candidate
$sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
if ($asset.digest -ne "sha256:$sha256") { throw 'Release ZIP checksum does not match GitHub.' }
$expectedDll = Get-Item -LiteralPath (Join-Path $candidate 'exec++.dll')
if ($expectedDll.VersionInfo.FileVersion -ne "$Version.0") { throw 'Unexpected DLL version in release ZIP.' }
$expectedHash = (Get-FileHash -LiteralPath $expectedDll.FullName).Hash

$npp = Join-Path $root 'notepad'
[void] (Save-Archive 'https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.9.9/npp.8.9.9.portable.x64.zip' 'notepad.zip' $npp)
$exe = Join-Path $npp 'notepad++.exe'
$updater = Join-Path $npp 'updater'
$gup = Join-Path $updater 'GUP.exe'
$plugins = Join-Path $npp 'plugins'
$installed = Join-Path $plugins 'exec++/exec++.dll'

function Invoke-PluginInstall([switch] $Update) {
    $options = @('-unzipTo')
    if ($Update) { $options += '-clean' }
    $options += "`"$exe`"", "`"$plugins`"", "`"exec++ $($asset.browser_download_url) $sha256`""
    $process = Start-Process -FilePath $gup -WorkingDirectory $updater -ArgumentList $options -PassThru
    if (!$process.WaitForExit(60000)) {
        Stop-Process -Id $process.Id -Force
        throw 'WinGUp did not finish within 60 seconds.'
    }
    if ($process.ExitCode -ne 0) { throw "WinGUp failed with exit code $($process.ExitCode)." }
    try {
        if ((Get-FileHash -LiteralPath $installed).Hash -ne $expectedHash) {
            throw 'WinGUp installed a different DLL than the release ZIP.'
        }
    } finally {
        # WinGUp reopens the test copy of Notepad++ after each operation.
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        do {
            $apps = Get-Process 'notepad++' -ErrorAction SilentlyContinue | Where-Object Path -eq $exe
            if (!$apps) { Start-Sleep -Milliseconds 100 }
        } until ($apps -or [DateTime]::UtcNow -ge $deadline)
        foreach ($app in $apps) {
            [void] $app.CloseMainWindow()
            if (!$app.WaitForExit(3000)) { Stop-Process -Id $app.Id -Force }
        }
    }
}

Invoke-PluginInstall
$old = Join-Path $root 'old'
[void] (Save-Archive 'https://github.com/fernandocpaz/exec-/releases/download/v0.3.0/exec-plus-plus-v0.3.0-windows-x64.zip' 'old.zip' $old)
Copy-Item -LiteralPath (Join-Path $old 'exec++/exec++.dll') -Destination $installed -Force
$staleFile = Join-Path (Split-Path $installed) 'old-package.txt'
New-Item -ItemType File -Path $staleFile | Out-Null
Invoke-PluginInstall -Update
if (Test-Path -LiteralPath $staleFile) { throw 'WinGUp did not clean the superseded plugin folder.' }
Write-Host "WinGUp installed exec++ v$Version and upgraded the v0.3.0 fixture to v$Version."
