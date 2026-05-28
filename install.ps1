# qsc installer for Windows — downloads the latest prebuilt binary from GitHub Releases
# and bundles a portable Tiny C Compiler (TCC) so no external toolchain is required.
#
# Usage (PowerShell):
#   iwr -useb https://raw.githubusercontent.com/Bilal1545/quickscript/main/install.ps1 | iex
#
# Environment:
#   QSC_REPO         GitHub "owner/repo"          (default: Bilal1545/quickscript)
#   QSC_VERSION      Release tag, "latest", or "twilight" (default: latest).
#                    "twilight" pulls the most recent main-branch CI build via
#                    nightly.link instead of a published release.
#   QSC_PREFIX       Install directory            (default: $env:LOCALAPPDATA\qsc)
#   QSC_TCC_URL      Override TCC download URL    (default: savannah official 0.9.27 win64)
#   QSC_SKIP_TCC     Set to 1 to skip TCC bundle  (use your own gcc via $env:QSC_CC)

$ErrorActionPreference = "Stop"

$Repo    = if ($env:QSC_REPO)    { $env:QSC_REPO }    else { "Bilal1545/quickscript" }
$Tag     = if ($env:QSC_VERSION) { $env:QSC_VERSION } else { "latest" }
$Prefix  = if ($env:QSC_PREFIX)  { $env:QSC_PREFIX }  else { Join-Path $env:LOCALAPPDATA "qsc" }
$TccUrl  = if ($env:QSC_TCC_URL) { $env:QSC_TCC_URL } else { "https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip" }
$SkipTcc = $env:QSC_SKIP_TCC -eq "1"

$Asset = "qsc-windows-x86_64"
$Twilight = ($Tag -eq "twilight")
$Url = if ($Twilight) {
    "https://nightly.link/$Repo/workflows/build/main/$Asset.zip"
} elseif ($Tag -eq "latest") {
    "https://github.com/$Repo/releases/latest/download/$Asset.tar.gz"
} else {
    "https://github.com/$Repo/releases/download/$Tag/$Asset.tar.gz"
}

if (-not $Twilight -and -not (Get-Command tar -ErrorAction SilentlyContinue)) {
    throw "qsc: 'tar' is required (built-in on Windows 10 1803+ / Windows 11)"
}

Write-Host "qsc: downloading $Asset ($Tag) from $Repo"

$Tmp = New-Item -ItemType Directory -Force -Path (Join-Path $env:TEMP "qsc-install-$([guid]::NewGuid().ToString('N'))")
try {
    if ($Twilight) {
        $ZipPath = Join-Path $Tmp "qsc.zip"
        Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
        Expand-Archive -Path $ZipPath -DestinationPath $Tmp -Force
    } else {
        $TarPath = Join-Path $Tmp "qsc.tar.gz"
        Invoke-WebRequest -Uri $Url -OutFile $TarPath -UseBasicParsing
        tar -xzf $TarPath -C $Tmp
        if ($LASTEXITCODE -ne 0) { throw "qsc: failed to extract archive" }
    }

    New-Item -ItemType Directory -Force -Path $Prefix | Out-Null

    Move-Item -Force (Join-Path $Tmp "$Asset.exe")     (Join-Path $Prefix "qsc.exe")
    Move-Item -Force (Join-Path $Tmp "runtime.c")      (Join-Path $Prefix "runtime.c")
    Move-Item -Force (Join-Path $Tmp "runtime.h")      (Join-Path $Prefix "runtime.h")
    New-Item -ItemType Directory -Force -Path (Join-Path $Prefix "vendor") | Out-Null
    Move-Item -Force (Join-Path $Tmp "vendor\re.h")    (Join-Path $Prefix "vendor\re.h")
    Move-Item -Force (Join-Path $Tmp "vendor\re.c")    (Join-Path $Prefix "vendor\re.c")

    # Older installs may have a leftover msys-2.0.dll from the MSYS-built binary;
    # remove it so the new native MINGW build isn't confused by stale runtimes.
    Remove-Item -Force (Join-Path $Prefix "msys-2.0.dll") -ErrorAction SilentlyContinue

    if (-not $SkipTcc) {
        Write-Host "qsc: downloading bundled C compiler (TCC) from $TccUrl"
        $TccZip = Join-Path $Tmp "tcc.zip"
        $TccDir = Join-Path $Tmp "tcc-extract"
        Invoke-WebRequest -Uri $TccUrl -OutFile $TccZip -UseBasicParsing
        Expand-Archive -Path $TccZip -DestinationPath $TccDir -Force

        # The zip contains a single top-level directory (e.g. "tcc/"); flatten it into $Prefix.
        $TccRoot = Get-ChildItem -Path $TccDir -Directory | Select-Object -First 1
        if (-not $TccRoot) { throw "qsc: TCC archive layout unexpected — no top-level directory" }
        Get-ChildItem -Path $TccRoot.FullName -Force | ForEach-Object {
            $Dest = Join-Path $Prefix $_.Name
            if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
            Move-Item -Force -Path $_.FullName -Destination $Dest
        }
        if (-not (Test-Path (Join-Path $Prefix "tcc.exe"))) {
            throw "qsc: TCC archive did not contain tcc.exe"
        }
    } else {
        Write-Host "qsc: skipping TCC bundle (QSC_SKIP_TCC=1) — set `$env:QSC_CC to your own compiler"
    }

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
