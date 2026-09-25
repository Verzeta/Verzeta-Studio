# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# setup-windows-build.ps1
# -----------------------
# One-time bootstrap for a fresh Windows VM that will run the
# Gitea Actions windows-build job (.gitea/workflows/Windows.yaml).
# Installs MSYS2 + the UCRT64 Qt 6 / KF6 toolchain + Inno Setup +
# Git for Windows. Idempotent - re-running is safe (existing
# installs are detected and skipped, pacman is invoked with -S
# --needed).
#
# Run as Administrator from an elevated PowerShell on the VM:
#
#   powershell -ExecutionPolicy Bypass -File .\packaging\windows\setup-windows-build.ps1
#
# After this completes, act_runner has everything cmake / ninja /
# windeployqt6 / ISCC need to execute the workflow steps end-to-end.
# Verify with:
#   cmake --preset windows-native
#   cmake --build build-windows --parallel
#   .\packaging\windows\deploy-windows.ps1 -Zip -BuildInstaller

$ErrorActionPreference = 'Stop'

function Write-Step { param([string]$m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "  OK $m"  -ForegroundColor Green }
function Write-Warn2 { param([string]$m) Write-Host "  WARN $m" -ForegroundColor Yellow }
function Stop-Setup { param([string]$m) Write-Host "ERROR $m" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
# 0. Sanity checks
# ---------------------------------------------------------------------------
Write-Step 'Checking environment'

$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Stop-Setup 'This script must be run as Administrator.'
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host 'winget is not on PATH. Install App Installer from the Microsoft' -ForegroundColor Red
    Write-Host 'Store (or from https://aka.ms/getwinget) and re-run this script.' -ForegroundColor Red
    exit 1
}
Write-Ok 'Running as Administrator with winget available.'

# ---------------------------------------------------------------------------
# 1. winget-installable prerequisites
#    - MSYS2     : Qt 6 + KF6 toolchain (we add packages via pacman below)
#    - Inno Setup: Windows installer compiler (ISCC.exe)
#    - Git       : the Gitea Actions checkout step shells out to it
# ---------------------------------------------------------------------------
Write-Step 'Installing MSYS2, Inno Setup 6, Git for Windows via winget'

# IMPORTANT: --scope machine. The act_runner Windows service runs as
# LOCAL SYSTEM (a service-only account), NOT as the interactive user
# who runs this bootstrap script. winget's DEFAULT scope is per-user,
# which lands installs under %LOCALAPPDATA%\Programs\<App>\ for the
# bootstrap user -- and LOCAL SYSTEM cannot read another user's
# LOCALAPPDATA. The CI workflow then fails at runtime because
# ISCC.exe / git.exe / etc. are invisible to the service account.
#
# Forcing machine scope puts everything under C:\Program Files\
# (x86) where every account on the box can resolve it. MSYS2 always
# installs machine-wide regardless of this flag, so the scope tag on
# its line is a no-op but kept for consistency.
#
# If the package author hasn't published a machine-scope manifest
# variant, winget falls back to user-scope and prints a one-line
# warning. Both Inno Setup 6 and Git.Git ship machine-scope manifests,
# so this is reliable.
$wingetPackages = @(
    @{ Id = 'MSYS2.MSYS2';          Name = 'MSYS2' },
    @{ Id = 'JRSoftware.InnoSetup'; Name = 'Inno Setup 6' },
    @{ Id = 'Git.Git';              Name = 'Git for Windows' }
)

foreach ($pkg in $wingetPackages) {
    Write-Step ('  {0} ({1})' -f $pkg.Name, $pkg.Id)
    # Splatting avoids fragile backtick line-continuation. --silent
    # suppresses GUI prompts; --accept-* swallows EULA confirmations.
    # --scope machine forces system-wide install (see block comment).
    $wingetArgs = @(
        'install',
        '--id', $pkg.Id,
        '--scope', 'machine',
        '--silent',
        '--accept-package-agreements',
        '--accept-source-agreements',
        '--disable-interactivity'
    )
    & winget @wingetArgs 2>&1 | Out-Host
    # winget exits 0 when installed, -1978335135 (0x8A150061) for "already
    # installed" on idempotent re-runs. Anything else is a real failure.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335135) {
        Write-Warn2 ('winget install {0} returned exit code {1}' -f $pkg.Id, $LASTEXITCODE)
    } else {
        Write-Ok ('  {0} ready' -f $pkg.Name)
    }
}

# ---------------------------------------------------------------------------
# 2. MSYS2 location resolution
#    winget installs MSYS2 at C:\msys64 by default. The CMake preset
#    (windows-native) hardcodes that path, so we verify it exists rather
#    than try to relocate.
# ---------------------------------------------------------------------------
$msys2Root = 'C:\msys64'
$bashExe   = Join-Path $msys2Root 'usr\bin\bash.exe'
if (-not (Test-Path $bashExe)) {
    Write-Host "MSYS2 not found at $msys2Root after winget install." -ForegroundColor Red
    Write-Host 'The CMake preset windows-native expects exactly this path.'   -ForegroundColor Red
    Write-Host 'Re-run winget manually and confirm the install location:'     -ForegroundColor Red
    Write-Host '    winget install MSYS2.MSYS2 --location C:\msys64'          -ForegroundColor Red
    exit 1
}
Write-Ok "MSYS2 located at $msys2Root"

# ---------------------------------------------------------------------------
# 3. MSYS2 first-run + base update
#    First invocation of bash.exe initialises /home/<user>, /etc, and the
#    keyring. We run it once non-interactively so subsequent pacman calls
#    don't get blocked on first-run prompts.
# ---------------------------------------------------------------------------
Write-Step 'Bootstrapping MSYS2 (first-run init + keyring)'
& $bashExe -lc 'true'
if ($LASTEXITCODE -ne 0) { Stop-Setup 'MSYS2 first-run bootstrap failed.' }

Write-Step 'pacman -Syu (refresh + upgrade core)'
# pacman -Syu on a stale install can require a "shut down all msys2
# processes and re-run" cycle. Two passes with --noconfirm: first pass
# updates the package db + core, second picks up anything deferred.
& $bashExe -lc 'pacman -Syu --noconfirm' | Out-Host
& $bashExe -lc 'pacman -Syu --noconfirm' | Out-Host

# ---------------------------------------------------------------------------
# 4. Install the UCRT64 toolchain + Qt 6 + KF6 + qqc2-desktop-style.
#    NOTE on naming: MSYS2 dropped the kf6- prefix once KF5 was retired -
#    the framework packages are bare names (kirigami / karchive / etc.).
# ---------------------------------------------------------------------------
$pacmanPackages = @(
    # Toolchain - gcc, g++, gdb, binutils, mingw-w64 headers
    'mingw-w64-ucrt-x86_64-toolchain',

    # CMake + Ninja - the build driver itself
    'mingw-w64-ucrt-x86_64-cmake',
    'mingw-w64-ucrt-x86_64-ninja',
    'mingw-w64-ucrt-x86_64-pkgconf',
    'mingw-w64-ucrt-x86_64-extra-cmake-modules',

    # Qt 6 modules verzeta links against. windeployqt6.exe ships in
    # qt6-base; qt6-tools brings designer/linguist/lrelease.
    'mingw-w64-ucrt-x86_64-qt6-base',
    'mingw-w64-ucrt-x86_64-qt6-declarative',
    'mingw-w64-ucrt-x86_64-qt6-multimedia',
    'mingw-w64-ucrt-x86_64-qt6-svg',
    'mingw-w64-ucrt-x86_64-qt6-imageformats',
    'mingw-w64-ucrt-x86_64-qt6-tools',
    'mingw-w64-ucrt-x86_64-qt6-translations',
    'mingw-w64-ucrt-x86_64-qt6-websockets',

    # KDE Frameworks 6 - bare names, no kf6- prefix in MSYS2.
    #
    # The breeze QStyle plugin (mingw-w64-ucrt-x86_64-breeze, ships
    # share/qt6/plugins/styles/breeze6.dll) has runtime deps on a
    # whole chain of KF6 libs that pacman would resolve transitively
    # IF kirigami were the only entry point -- but the wholesale DLL
    # copy in deploy-windows.ps1 only catches what's actually present
    # in $MSYS2\bin\, so a transitive dep that pacman skipped because
    # something else dragged in a compatible version doesn't get
    # bundled. Explicit-listing every breeze runtime dep eliminates
    # that silent-failure mode.
    #
    # Confirmed deps (from packages.msys2.org for breeze + breeze-icons):
    #   breeze        -> breeze-icons, kcolorscheme, kconfig,
    #                    kcoreaddons, kguiaddons, ki18n, kiconthemes,
    #                    kirigami, kwindowsystem, qt6-base,
    #                    qt6-declarative, hicolor-icon-theme, cc-libs
    #   breeze-icons  -> qt6-base
    #
    # cc-libs is the C++ runtime (libstdc++, libgcc) -- pulled in
    # by the toolchain pkg, listed explicitly here for documentation.
    # hicolor-icon-theme is the FreeDesktop icon-theme baseline that
    # breeze-icons inherits from.
    'mingw-w64-ucrt-x86_64-kirigami',
    'mingw-w64-ucrt-x86_64-knotifications',
    'mingw-w64-ucrt-x86_64-syntax-highlighting',
    'mingw-w64-ucrt-x86_64-breeze-icons',
    'mingw-w64-ucrt-x86_64-kiconthemes',
    'mingw-w64-ucrt-x86_64-breeze',
    'mingw-w64-ucrt-x86_64-karchive',
    # Explicit transitive deps of breeze QStyle plugin
    'mingw-w64-ucrt-x86_64-kcolorscheme',
    'mingw-w64-ucrt-x86_64-kconfig',
    'mingw-w64-ucrt-x86_64-kcoreaddons',
    'mingw-w64-ucrt-x86_64-kguiaddons',
    'mingw-w64-ucrt-x86_64-ki18n',
    'mingw-w64-ucrt-x86_64-kwindowsystem',
    'mingw-w64-ucrt-x86_64-hicolor-icon-theme',

    # qqc2-desktop-style - required at runtime by org.kde.desktop QML style
    'mingw-w64-ucrt-x86_64-qqc2-desktop-style',

    # SQLite - Qt's Sql plugin links against the system lib
    'mingw-w64-ucrt-x86_64-sqlite3',

    # OpenSSL - Qt Network's TLS backend
    'mingw-w64-ucrt-x86_64-openssl'
)

Write-Step ('Installing UCRT64 toolchain + Qt 6 + KF6 packages ({0} packages)' -f $pacmanPackages.Count)

# --needed makes pacman skip up-to-date packages (idempotency on re-runs).
$pkgList    = $pacmanPackages -join ' '
$pacmanCmd  = "pacman -S --needed --noconfirm $pkgList"
& $bashExe -lc $pacmanCmd | Out-Host
if ($LASTEXITCODE -ne 0) {
    Stop-Setup ('pacman -S failed (exit {0}). See output above for the failed package.' -f $LASTEXITCODE)
}
Write-Ok 'All MSYS2 packages installed.'

# ---------------------------------------------------------------------------
# 5. Smoke test - confirm the toolchain works end-to-end
# ---------------------------------------------------------------------------
Write-Step 'Verifying toolchain'
$ucrt64bin = Join-Path $msys2Root 'ucrt64\bin'

$expectedExes = @('gcc.exe', 'g++.exe', 'cmake.exe', 'ninja.exe', 'windeployqt6.exe')
foreach ($exe in $expectedExes) {
    $exePath = Join-Path $ucrt64bin $exe
    if (Test-Path $exePath) {
        Write-Ok ('  {0} present' -f $exe)
    } else {
        Stop-Setup ('  {0} MISSING at {1} - pacman did not install the expected package.' -f $exe, $exePath)
    }
}

# Stronger check: invoke cmake via its absolute path. Confirms the binary
# is not just present but actually executable in this environment.
# Capture all output first so $LASTEXITCODE is settled before we inspect
# it - piping into Select-Object can race with the native exit code.
$cmakeExe    = Join-Path $ucrt64bin 'cmake.exe'
$cmakeOutput = & $cmakeExe --version 2>&1
if ($LASTEXITCODE -ne 0) { Stop-Setup 'cmake.exe present but failed to run.' }
$firstLine = ($cmakeOutput | Select-Object -First 1)
Write-Ok ('  cmake works: {0}' -f $firstLine)

# ---------------------------------------------------------------------------
# 5a. Persist UCRT64 bin to the SYSTEM PATH (Machine scope, requires Admin
#     which we already verified). Without this, every fresh PowerShell
#     window on the VM has to manually prepend $env:PATH before running
#     cmake / ninja / windeployqt6 / etc. The CI workflow itself already
#     prepends inline at every step (Windows.yaml), so this is purely
#     for the post-install manual verification + interactive use.
#
#     Idempotent - we only append if the directory isn't already on PATH.
#     Effective in NEW shells (and services started after this point).
#     Existing windows / running services keep their cached environment
#     until restart.
# ---------------------------------------------------------------------------
Write-Step 'Persisting UCRT64 bin to system PATH'
$systemPath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$pathSegments = $systemPath -split ';' | Where-Object { $_ -ne '' }
$alreadyOnPath = $pathSegments | Where-Object { $_.TrimEnd('\') -ieq $ucrt64bin.TrimEnd('\') }

if ($alreadyOnPath) {
    Write-Ok ('{0} already on system PATH.' -f $ucrt64bin)
} else {
    $newSystemPath = "$ucrt64bin;$systemPath"
    [Environment]::SetEnvironmentVariable('Path', $newSystemPath, 'Machine')
    Write-Ok ('Added {0} to system PATH (Machine scope).' -f $ucrt64bin)
    Write-Warn2 'New PATH takes effect in NEW PowerShell windows and on next service restart.'
}

# Also update the current process so the rest of this script (and any
# manual cmake invocation in the same window) sees cmake immediately.
if (-not ($env:PATH -split ';' | Where-Object { $_.TrimEnd('\') -ieq $ucrt64bin.TrimEnd('\') })) {
    $env:PATH = "$ucrt64bin;$env:PATH"
}

# Inno Setup ISCC.exe lives in one of two locations depending on installer bitness.
$isccPaths = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles        'Inno Setup 6\ISCC.exe')
)
$iscc = $isccPaths | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
    Write-Ok ('  ISCC.exe present at {0}' -f $iscc)
} else {
    Write-Warn2 '  ISCC.exe NOT found in either Program Files location.'
    Write-Warn2 '  Inno Setup may not have completed installation; re-run this script.'
}

# Git
if (Get-Command git -ErrorAction SilentlyContinue) {
    $gitVer = (git --version)
    Write-Ok ('  git on PATH ({0})' -f $gitVer)
} else {
    Write-Warn2 '  git not on PATH - open a NEW PowerShell so the updated PATH takes effect.'
}

# ---------------------------------------------------------------------------
# 6. Done
# ---------------------------------------------------------------------------
Write-Host ''
Write-Step 'Setup complete.'
Write-Host ''
Write-Host 'Next steps:'
Write-Host '  1. Make sure act_runner is registered with the Gitea server using'
Write-Host "     the 'windows' label (matches the runs-on field in"
Write-Host '     .gitea/workflows/Windows.yaml).'
Write-Host '  2. Confirm act_runner is set to autostart on login (Services panel'
Write-Host '     -> look for the runner service -> Startup type: Automatic).'
Write-Host '  3. Optional - verify the toolchain by running a manual build now.'
Write-Host '     OPEN A NEW PowerShell window first so the updated system PATH'
Write-Host '     is picked up (the window that ran this script keeps its cached'
Write-Host '     PATH). Then:'
Write-Host '        git clone https://github.com/Verzeta/Verzeta-Studio.git'
Write-Host '        cd Verzeta'
Write-Host '        cmake --preset windows-native'
Write-Host '        cmake --build build-windows --parallel'
Write-Host '        .\packaging\windows\deploy-windows.ps1 -Zip -BuildInstaller'
Write-Host '     If those commands succeed, the CI workflow will too.'
Write-Host '  4. Restart the act_runner service so it picks up the new system PATH'
Write-Host '     (Services panel -> right-click act_runner -> Restart). Otherwise'
Write-Host '     the service will keep the PATH it had at last start - the workflow'
Write-Host '     prepends UCRT64 inline at every step so it would still work, but'
Write-Host '     restarting is cleaner.'
Write-Host '  5. Shut down the VM cleanly. The Linux host will start it on demand'
Write-Host '     when the next push to main fires the Windows.yaml workflow.'
