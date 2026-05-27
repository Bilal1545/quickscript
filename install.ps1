# qsc installer for Windows — downloads the latest prebuilt binary from GitHub Releases.
#
# Usage (PowerShell):
#   iwr -useb https://raw.githubusercontent.com/Bilal1545/quickscript/main/install.ps1 | iex
#
# Environment:
#   QSC_REPO     GitHub "owner/repo"           (default: Bilal1545/quickscript)
#   QSC_VERSION  Release tag, or "latest"      (default: latest)
#   QSC_PREFIX   Install directory             (default: $env:LOCALAPPDATA\qsc)

$ErrorActionPreference = "Stop"

$Repo    = if ($env:QSC_REPO)    { $env:QSC_REPO }    else { "Bilal1545/quickscript" }
$Tag     = if ($env:QSC_VERSION) { $env:QSC_VERSION } else { "latest" }
$Prefix  = if ($env:QSC_PREFIX)  { $env:QSC_PREFIX }  else { Join-Path $env:LOCALAPPDATA "qsc" }

$Asset = "qsc-windows-x86_64"
$Url = if ($Tag -eq "latest") {
    "https://github.com/$Repo/releases/latest/download/$Asset.tar.gz"
} else {
    "https://github.com/$Repo/releases/download/$Tag/$Asset.tar.gz"
}

if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
    throw "qsc: 'tar' is required (built-in on Windows 10 1803+ / Windows 11)"
}
if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Write-Warning "qsc: 'gcc' not found in PATH; qsc requires it at runtime (install via MSYS2)"
}

Write-Host "qsc: downloading $Asset ($Tag) from $Repo"

$Tmp = New-Item -ItemType Directory -Force -Path (Join-Path $env:TEMP "qsc-install-$([guid]::NewGuid().ToString('N'))")
try {
    $TarPath = Join-Path $Tmp "qsc.tar.gz"
    Invoke-WebRequest -Uri $Url -OutFile $TarPath -UseBasicParsing
    tar -xzf $TarPath -C $Tmp
    if ($LASTEXITCODE -ne 0) { throw "qsc: failed to extract archive" }

    New-Item -ItemType Directory -Force -Path $Prefix | Out-Null

    Move-Item -Force (Join-Path $Tmp "$Asset.exe")     (Join-Path $Prefix "qsc.exe")
    Move-Item -Force (Join-Path $Tmp "msys-2.0.dll")   (Join-Path $Prefix "msys-2.0.dll") -ErrorAction SilentlyContinue
    Move-Item -Force (Join-Path $Tmp "runtime.c")      (Join-Path $Prefix "runtime.c")
    Move-Item -Force (Join-Path $Tmp "runtime.h")      (Join-Path $Prefix "runtime.h")

    # Persist QSC_RUNTIME_DIR for the user.
    [Environment]::SetEnvironmentVariable("QSC_RUNTIME_DIR", $Prefix, "User")
    $env:QSC_RUNTIME_DIR = $Prefix

    # Add install dir to user PATH if missing.
    $UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")
    if (-not $UserPath) { $UserPath = "" }
    if (($UserPath -split ';') -notcontains $Prefix) {
        $NewPath = if ($UserPath) { "$UserPath;$Prefix" } else { $Prefix }
        [Environment]::SetEnvironmentVariable("PATH", $NewPath, "User")
        Write-Host "qsc: added '$Prefix' to user PATH (open a new shell to use 'qsc')"
    }

    Write-Host "qsc: installed -> $Prefix\qsc.exe"
}
finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}
