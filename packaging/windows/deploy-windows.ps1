# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# deploy-windows.ps1
# ------------------
# Bundles verzeta-studio.exe (plus the verzeta-remote and verzeta-splash
# helper binaries) into a self-contained directory ready for packaging
# with Inno Setup (or zipping for portable distribution).
#
# Prerequisites (all via MSYS2 UCRT64):
#   pacman -S mingw-w64-ucrt-x86_64-qt6-tools   # provides windeployqt6
#
# Inno Setup 6 (optional, for -BuildInstaller):
#   https://jrsoftware.org/isdl.php
#
# Usage:
#   .\packaging\windows\deploy-windows.ps1
#   .\packaging\windows\deploy-windows.ps1 -Zip
#   .\packaging\windows\deploy-windows.ps1 -BuildInstaller
#   .\packaging\windows\deploy-windows.ps1 -Zip -BuildInstaller
#   .\packaging\windows\deploy-windows.ps1 -MSYS2 D:\msys64\ucrt64 -Zip

param(
    [string]$MSYS2      = "C:\msys64\ucrt64",
    # Defaults are computed relative to this script's location
    # (packaging\windows\). The script lives two levels under the
    # repo root, so `..\..\` resolves back to the repo root for the
    # build / source / output paths.
    [string]$BuildDir   = "$PSScriptRoot\..\..\build-windows\backend",
    [string]$DeployDir  = "$PSScriptRoot\..\..\deploy-windows",
    [string]$FrontendDir = "$PSScriptRoot\..\..\frontend",
    [string]$ZipOut     = "$PSScriptRoot\..\..\verzeta-studio-windows-x64-portable.zip",
    [switch]$BuildInstaller,
    [switch]$Zip
)

$ErrorActionPreference = "Stop"
$env:PATH = "$MSYS2\bin;$env:PATH"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Info([string]$msg) { Write-Host "  $msg" -ForegroundColor Cyan }
function Ok([string]$msg)   { Write-Host "  OK  $msg" -ForegroundColor Green }
function Warn([string]$msg) { Write-Host "  WARN $msg" -ForegroundColor Yellow }

# Resolve all DLL dependencies from ucrt64 for a given binary using ldd.
# Returns a list of full Windows paths.
function Get-Deps([string]$binary) {
    $unixBin  = ($binary -replace '\\', '/') -replace '^([A-Za-z]):', { '/' + $_.Value[0].ToString().ToLower() }
    $unixMSYS = ($MSYS2  -replace '\\', '/') -replace '^([A-Za-z]):', { '/' + $_.Value[0].ToString().ToLower() }
    # Write the ldd+awk pipeline to a temp script to avoid PowerShell escaping issues
    $script = [System.IO.Path]::GetTempFileName() + ".sh"
    [System.IO.File]::WriteAllText($script, "export PATH=/usr/bin:/bin:`$PATH`nldd '$unixBin' 2>/dev/null | grep '$unixMSYS' | awk '{print `$3}'`n")
    $out = & "C:\msys64\usr\bin\bash.exe" $script 2>$null
    Remove-Item $script -ErrorAction SilentlyContinue
    $out -split "`n" | Where-Object { $_ -match '\.dll$' } | ForEach-Object {
        # Convert /c/msys64/... -> C:\msys64\...
        $_ -replace '^/([a-z])/', '$1:/' -replace '/', '\'
    }
}

# Iteratively copy all ucrt64 DLLs reachable from any binary currently in
# $DeployDir (up to 5 passes to handle transitive dependencies).
function Copy-AllDeps {
    $copied = @{}
    for ($pass = 0; $pass -lt 5; $pass++) {
        $before = $copied.Count
        Get-ChildItem $DeployDir -Recurse -Include "*.exe","*.dll" | ForEach-Object {
            Get-Deps $_.FullName | ForEach-Object {
                $name = Split-Path $_ -Leaf
                $dest = "$DeployDir\$name"
                if ((Test-Path $_) -and -not $copied.ContainsKey($name)) {
                    Copy-Item $_ $dest -Force
                    $copied[$name] = $true
                }
            }
        }
        if ($copied.Count -eq $before) { break }   # stable -- no new DLLs
    }
    Ok "$($copied.Count) ucrt64 DLLs copied"
}

# ---------------------------------------------------------------------------
# 1. Prepare deploy directory
# ---------------------------------------------------------------------------
Write-Host "`nPreparing deploy directory..." -ForegroundColor White
Remove-Item -Recurse -Force $DeployDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $DeployDir | Out-Null
$exe = "$DeployDir\verzeta-studio.exe"
Copy-Item "$BuildDir\verzeta-studio.exe" $exe
Ok "Copied verzeta-studio.exe -> $DeployDir"

# Secondary binaries built alongside verzeta-studio by the same CMake
# project: verzeta-remote (the WebSocket daemon, spawned on demand from
# the Remote Access settings) and verzeta-splash (the startup splash,
# spawned at launch). Both must sit next to verzeta-studio.exe in the
# bundle — verzeta-studio resolves them via
# QCoreApplication::applicationDirPath().
#
# Nothing else in this script needs to change for them: section 4a copies
# every ucrt64 DLL wholesale, section 4b's Copy-AllDeps walks EVERY .exe
# in $DeployDir for transitive deps, windeployqt6's `--qmldir $FrontendDir`
# scan already covers verzeta-splash's QML (frontend\splash\Splash.qml
# lives under $FrontendDir), and both the zip and the .iss package the
# whole $DeployDir. We only need to land the .exe files here.
#
# A missing secondary binary is not fatal to the app (remote is opt-in;
# the splash degrades gracefully when absent — verzeta-studio checks for
# it before spawning), so warn rather than abort the whole package build.
# verzeta-inference is the local-AI sidecar: present only in the
# localai lane (VERZETA_ENABLE_LLAMACPP=ON builds); the base lane
# warns and ships without it, which is exactly the intended
# remote-providers-only flavour.
foreach ($aux in @("verzeta-remote.exe", "verzeta-splash.exe", "verzeta-inference.exe")) {
    $auxSrc = "$BuildDir\$aux"
    if (Test-Path $auxSrc) {
        Copy-Item $auxSrc "$DeployDir\$aux"
        Ok "Copied $aux -> $DeployDir"
    } else {
        Warn "$aux not found at $auxSrc -- bundle will ship without it"
    }
}

# Plan 62 -- sqlite-vec loadable (vec0.dll). Downloaded at CMake configure
# time and staged next to the .exe by a POST_BUILD step. Optional: if absent,
# the app falls back to brute-force vector search, so warn rather than abort.
$vecSrc = "$BuildDir\vec0.dll"
if (Test-Path $vecSrc) {
    Copy-Item $vecSrc "$DeployDir\vec0.dll"
    Ok "Copied vec0.dll (sqlite-vec) -> $DeployDir"
} else {
    Warn "vec0.dll not found at $vecSrc -- bundle will use brute-force vector search"
}

# ---------------------------------------------------------------------------
# 2. windeployqt6 -- Qt DLLs, plugins, Qt QML modules
# ---------------------------------------------------------------------------
Write-Host "`nRunning windeployqt6..." -ForegroundColor White
# windeployqt6 calls qmlimportscanner internally; on MSYS2 that tool lives in
# share/qt6/bin/ rather than bin/, so add it to PATH.
$env:PATH = "$MSYS2\share\qt6\bin;$env:PATH"
# windeployqt6 emits a non-fatal warning about MSYS2's missing catalogs.json
# and exits with code 1. Temporarily allow errors so we can check success ourselves.
$ErrorActionPreference = "Continue"
& windeployqt6 --qmldir $FrontendDir --no-translations $exe 2>&1 | Out-Null
$ErrorActionPreference = "Stop"
if (-not (Test-Path $exe)) { throw "windeployqt6 failed - exe missing" }
Ok "windeployqt6 complete"

# ---------------------------------------------------------------------------
# 3. KDE QML modules (Kirigami, qqc2-desktop-style, syntax-highlighting, etc.)
#    windeployqt6 scans --qmldir and copies what it detects, but may miss
#    KDE modules it doesn't know to scan for. We copy the full org/kde tree
#    into qml/org/kde/ so that qt.conf's QmlImports=qml path covers them.
# ---------------------------------------------------------------------------
Write-Host "`nCopying KDE QML modules..." -ForegroundColor White
$kdeQmlSrc  = "$MSYS2\share\qt6\qml\org\kde"
$kdeQmlDest = "$DeployDir\qml\org\kde"
New-Item -ItemType Directory -Force $kdeQmlDest | Out-Null
Copy-Item -Recurse -Force "$kdeQmlSrc\*" $kdeQmlDest
Ok "KDE QML modules -> $kdeQmlDest"

# ---------------------------------------------------------------------------
# 4a. Wholesale copy of EVERY .dll under C:\msys64\ucrt64\bin\.
#
#     User explicitly chose size-over-risk after a fresh-Windows
#     install reported missing libstdc++-6.dll, libgcc_s_seh-1.dll,
#     libKF6Notifications.dll, libKF6BreezeIcons.dll, libKF6Archive.dll
#     and several others. The previous "explicit list" approach was
#     fragile (any missed entry = broken bundle on fresh OS), and the
#     ldd-via-bash heuristic in Copy-AllDeps below silently swallows
#     errors via `2>$null` so its coverage isn't reliable either.
#
#     Wholesale copy guarantees ZERO risk of missing a required DLL
#     at the cost of bundle size: ucrt64\bin\ is typically 200-400
#     MB of DLLs depending on which pacman packages are installed.
#     Includes some DLLs we don't strictly need (other tooling
#     pulled in by pacman dependency chains) but the safety
#     guarantee is worth the size.
#
#     User said: "I don't mind an inflated size but I don't want to
#     risk not having anything".
#
#     Skip-if-exists keeps windeployqt6's earlier copies (e.g. Qt6
#     plugins it placed under plugins/) authoritative and avoids
#     overwriting anything already in DeployDir.
# ---------------------------------------------------------------------------
Write-Host "`nCopying ALL DLLs from $MSYS2\bin (wholesale)..." -ForegroundColor White
$wholesaleSrc = Get-ChildItem "$MSYS2\bin\*.dll" -ErrorAction SilentlyContinue
$wholesaleCount = 0
$wholesaleSkip  = 0
foreach ($dll in $wholesaleSrc) {
    $dest = Join-Path $DeployDir $dll.Name
    if (Test-Path $dest) {
        $wholesaleSkip++
    } else {
        Copy-Item $dll.FullName $dest -Force
        $wholesaleCount++
    }
}
Ok "$wholesaleCount DLLs copied wholesale ($wholesaleSkip already present, skipped)"

# ---------------------------------------------------------------------------
# 4b. Belt-and-braces: ldd-recursion across what's now in the bundle
#     to catch any DLLs that live OUTSIDE $MSYS2\bin (e.g. some Qt
#     plugins reach into $MSYS2\share\qt6\plugins\.../<plugin>.dll
#     which has its own deps). The wholesale copy above already
#     covers >99% of cases; this just sweeps anything else.
# ---------------------------------------------------------------------------
Write-Host "`nResolving any remaining transitive DLL dependencies..." -ForegroundColor White
Copy-AllDeps

# ---------------------------------------------------------------------------
# 5. qt.conf -- redirect Qt path resolution to the bundle layout.
#    Qt6Core.dll computes prefix one level above its location. In MSYS2,
#    binaries are in ucrt64/bin/ so prefix = ucrt64/. When deployed all DLLs
#    land in the bundle root, so prefix ends up one level above the bundle,
#    making QmlImportsPath and PluginsPath point to the wrong place.
#    A qt.conf next to the exe overrides this so Qt finds qml/ and plugins/.
# ---------------------------------------------------------------------------
Write-Host "`nWriting qt.conf..." -ForegroundColor White
@"
[Paths]
Prefix=.
Plugins=plugins
Qml2Imports=qml
QmlImports=qml
Libraries=.
"@ | Set-Content "$DeployDir\qt.conf" -Encoding UTF8
Ok "qt.conf written"

# ---------------------------------------------------------------------------
# 6. Icon RCC data
#    icontheme.rcc carries the entire breeze icon theme as a Qt resource.
#    libKF6BreezeIcons.dll alone has NO icons -- the rcc is the payload.
#    KIconTheme::initTheme() (called from main.cpp) registers this rcc by
#    walking a list of standard locations; the canonical one in upstream
#    KF6 is <exe-dir>/icontheme.rcc (bundle root). We ALSO copy to
#    <exe-dir>/data/icontheme.rcc as belt-and-braces:
#      - matches the path the old QResource::registerResource code used
#        (which BreezeIcons::initIcons + manual register also looked at);
#      - covers any KIconTheme version whose path list includes data/.
#    Both copies cost ~6 MB total; eliminates lookup-path uncertainty.
# ---------------------------------------------------------------------------
Write-Host "`nCopying icon data..." -ForegroundColor White
New-Item -ItemType Directory -Force "$DeployDir\data" | Out-Null
Copy-Item "$MSYS2\bin\data\icontheme.rcc" "$DeployDir\icontheme.rcc"      -Force
Copy-Item "$MSYS2\bin\data\icontheme.rcc" "$DeployDir\data\icontheme.rcc" -Force
$rccBytes = (Get-Item "$DeployDir\icontheme.rcc").Length
Ok ("icontheme.rcc copied to <root> AND <data>\ ({0:N1} MB each)" -f ($rccBytes / 1MB))

# ---------------------------------------------------------------------------
# 6b. Mirror EVERY runtime file from `mingw-w64-ucrt-x86_64-breeze` and
#     `mingw-w64-ucrt-x86_64-breeze-icons` into the deploy bundle.
#
# Why a generic mirror, not explicit Copy-Item lines:
#
# The breeze QStyle plugin (share\qt6\plugins\styles\breeze6.dll)
# does NOT load with just the .dll alone. It has runtime deps on a
# chain of KF6 libs (KConfigGui, KColorScheme, KGuiAddons, etc.)
# AND on data files shipped by the same packages (kstyle themerc,
# colour schemes, icon themes). User-reported symptom when only the
# .dll was bundled: Qt lists "Breeze" in QStyleFactory::keys() (so
# the plugin metadata loaded) but `QApplication: invalid style
# override 'Breeze'` at instantiation -- the plugin's QStyleFactory
# create() returned null because deps weren't resolvable.
#
# Strategy: enumerate every file pacman knows about for both
# packages, copy the actual files at their relative paths, with
# Qt-specific path remapping so the QStyle plugin lands where qt.conf
# tells Qt to look (plugins\styles\, NOT share\qt6\plugins\styles\).
#
# Skip include\ and lib\cmake\ (build-time only). The wholesale
# bin\*.dll copy above (section 4a) already covers libKF6*.dll;
# this section adds the share\ + bin\data\ payloads it can't see.
# ---------------------------------------------------------------------------
function Copy-PackageRuntimeFiles {
    param(
        [Parameter(Mandatory)] [string]$Package,
        [Parameter(Mandatory)] [string]$BashExe
    )

    Write-Host "  Mirroring $Package ..." -ForegroundColor White

    # `pacman -Ql <pkg>` emits one line per file: `<pkg> <abs-path>`.
    # Strip the package prefix, drop directories (lines ending in `/`),
    # produce a list of absolute paths in /ucrt64/... form.
    $listCmd = "pacman -Ql $Package 2>/dev/null | awk '{print `$2}' | grep -v '/`$'"
    $rawList = & $BashExe -lc $listCmd
    if (-not $rawList -or $LASTEXITCODE -ne 0) {
        Warn "    pacman -Ql $Package returned nothing -- package not installed?"
        Warn "    Re-run packaging\windows\setup-windows-build.ps1 as Admin."
        return
    }

    $copied = 0
    $skipped = 0
    $missing = 0
    foreach ($unixPath in ($rawList -split "`n")) {
        $unixPath = $unixPath.Trim()
        if (-not $unixPath) { continue }

        # Strip the /ucrt64/ prefix so we get a relative path inside MSYS2.
        $rel = $unixPath -replace '^/ucrt64/', ''
        if ($rel -eq $unixPath) { continue }   # path didn't start with /ucrt64/

        # Source on Windows: $MSYS2\<rel> with backslashes
        $src = Join-Path $MSYS2 ($rel -replace '/', '\')
        if (-not (Test-Path -PathType Leaf $src)) {
            $missing++
            continue
        }

        # Skip build-time-only paths.
        if ($rel -match '^(include/|lib/cmake/|lib/pkgconfig/|share/man/|share/doc/)') {
            $skipped++
            continue
        }
        if ($rel -match '\.(dll\.a|h|cmake|pc)$') {
            $skipped++
            continue
        }

        # Path remapping for Qt:
        #   share/qt6/plugins/<X>  -> plugins/<X>          (qt.conf Plugins=plugins)
        #   share/qt6/qml/<X>      -> qml/<X>              (qt.conf Qml2Imports=qml)
        #   bin/<X>                -> <X>                  (root, next to .exe)
        # All other share/<X>, lib/<X>, etc. preserved as-is.
        $destRel = $rel
        if ($destRel -match '^share/qt6/plugins/') {
            $destRel = $destRel -replace '^share/qt6/', ''
        } elseif ($destRel -match '^share/qt6/qml/') {
            $destRel = $destRel -replace '^share/qt6/', ''
        } elseif ($destRel -match '^bin/') {
            $destRel = $destRel -replace '^bin/', ''
        }

        $dst = Join-Path $DeployDir ($destRel -replace '/', '\')
        $dstDir = Split-Path $dst -Parent
        if (-not (Test-Path $dstDir)) {
            New-Item -ItemType Directory -Force $dstDir | Out-Null
        }
        Copy-Item -Force $src $dst
        $copied++
    }

    Ok "    $Package : copied=$copied skipped=$skipped missing-on-disk=$missing"
}

Write-Host "`nMirroring breeze + breeze-icons package contents..." -ForegroundColor White
$bashExe = "C:\msys64\usr\bin\bash.exe"
if (-not (Test-Path $bashExe)) {
    Warn "MSYS2 bash not at $bashExe -- skipping breeze + breeze-icons mirror."
    Warn "App will run but breeze QStyle won't load (KStyle deps absent)."
} else {
    Copy-PackageRuntimeFiles -Package 'mingw-w64-ucrt-x86_64-breeze'       -BashExe $bashExe
    Copy-PackageRuntimeFiles -Package 'mingw-w64-ucrt-x86_64-breeze-icons' -BashExe $bashExe

    # Sanity-check the QStyle plugin landed where Qt expects.
    $expectedStyle = Join-Path $DeployDir 'plugins\styles\breeze6.dll'
    if (Test-Path $expectedStyle) {
        $sz = [math]::Round((Get-Item $expectedStyle).Length / 1KB, 1)
        Ok "  breeze6.dll present at plugins\styles\ ($sz KB)"
    } else {
        Warn "  breeze6.dll NOT at plugins\styles\ after mirror -- QStyle"
        Warn "  fallback will be windowsvista. Check setup-windows-build.ps1"
        Warn "  installed mingw-w64-ucrt-x86_64-breeze, then re-run deploy."
    }

    # KIconEnginePlugin.dll lives at a non-standard plugin path:
    #   $MSYS2/share/qt6/plugins/kiconthemes6/iconengines/KIconEnginePlugin.dll
    # which after the mirror remap (share/qt6/plugins/X -> plugins/X)
    # ends up at:
    #   <DeployDir>/plugins/kiconthemes6/iconengines/KIconEnginePlugin.dll
    # but Qt's plugin scanner only walks `<libpath>/iconengines/` -- it
    # never sees the kiconthemes6/ subdir. Upstream KIconTheme::initTheme
    # adds the full install-prefix path (KICONTHEMES_INSTALL_PLUGINDIR
    # baked at build time) to QCoreApplication::libraryPaths, but on
    # Windows after deploy the build-time path doesn't exist.
    # QCoreApplication::addLibraryPath at runtime did NOT take effect
    # in user testing (the kiconthemes6 dir was never scanned per
    # QT_DEBUG_PLUGINS=1 log).
    #
    # Reliable fix: copy the plugin into Qt's standard scan path,
    # <DeployDir>/iconengines/ (alongside qsvgicon.dll which Qt finds
    # there without issue). The plugin's metadata IID is
    # org.qt-project.Qt.QIconEngineFactoryInterface which Qt picks up
    # from any directory named iconengines/ on the libraryPaths.
    # Without the recoloured icon engine, mono Kirigami icons paint
    # black-on-dark in dark mode regardless of how kdeglobals + QStyle
    # are set up -- this is the root cause of the user-reported icon
    # symptom.
    $kiconEngineSrc1 = Join-Path $DeployDir 'plugins\kiconthemes6\iconengines\KIconEnginePlugin.dll'
    $kiconEngineSrc2 = Join-Path $MSYS2 'share\qt6\plugins\kiconthemes6\iconengines\KIconEnginePlugin.dll'
    $kiconEngineSrc = $null
    if (Test-Path $kiconEngineSrc1) {
        # Mirror already placed it; copy from there to the canonical
        # location (avoids re-reading from $MSYS2 if mirror already ran).
        $kiconEngineSrc = $kiconEngineSrc1
    } elseif (Test-Path $kiconEngineSrc2) {
        $kiconEngineSrc = $kiconEngineSrc2
    }
    if ($kiconEngineSrc) {
        # Copy to BOTH iconengines/ (root, where Qt looks first per
        # qsvgicon.dll precedent) AND plugins/iconengines/ (covered by
        # qt.conf's Plugins=plugins). Belt-and-braces; Qt picks up the
        # first that resolves.
        foreach ($dst in @(
            (Join-Path $DeployDir 'iconengines\KIconEnginePlugin.dll'),
            (Join-Path $DeployDir 'plugins\iconengines\KIconEnginePlugin.dll')
        )) {
            $dstDir = Split-Path $dst -Parent
            if (-not (Test-Path $dstDir)) {
                New-Item -ItemType Directory -Force $dstDir | Out-Null
            }
            Copy-Item -Force $kiconEngineSrc $dst
        }
        Ok "  KIconEnginePlugin.dll copied into iconengines\ and plugins\iconengines\"
    } else {
        Warn "  KIconEnginePlugin.dll not found at either of:"
        Warn "    $kiconEngineSrc1"
        Warn "    $kiconEngineSrc2"
        Warn "  Mono Kirigami icons WILL render black-on-dark in dark mode."
        Warn "  Check setup-windows-build.ps1 installed mingw-w64-ucrt-x86_64-kiconthemes."
    }
}

# ---------------------------------------------------------------------------
# 6. Summary
# ---------------------------------------------------------------------------
$sizeMB = [math]::Round((Get-ChildItem $DeployDir -Recurse | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Host "`nDeploy bundle ready: $DeployDir  ($sizeMB MB)" -ForegroundColor Green

# ---------------------------------------------------------------------------
# 7. Optional -- portable zip
# ---------------------------------------------------------------------------
if ($Zip) {
    Write-Host "`nCreating portable zip..." -ForegroundColor White
    $zipPath = [System.IO.Path]::GetFullPath($ZipOut)
    Remove-Item $zipPath -ErrorAction SilentlyContinue
    Compress-Archive -Path "$DeployDir\*" -DestinationPath $zipPath -CompressionLevel Optimal
    $zipMB = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
    Ok "Portable zip: $zipPath  ($zipMB MB)"
}

# ---------------------------------------------------------------------------
# 8. Optional -- build Inno Setup installer
# ---------------------------------------------------------------------------
if ($BuildInstaller) {
    Write-Host "`nBuilding installer..." -ForegroundColor White
    # PowerShell parses `$env:ProgramFiles(x86)` greedily -- it resolves
    # `$env:ProgramFiles` to "C:\Program Files" and then appends the
    # literal "(x86)\..." suffix, producing the wrong path
    # "C:\Program Files(x86)\Inno Setup 6\ISCC.exe" (no space). The
    # actual environment variable name contains parentheses, so we
    # MUST use the `${env:Name}` brace-delimited form to disambiguate.
    #
    # Probe ORDER matters: machine-scope locations first (visible to
    # every account, including LOCAL SYSTEM that act_runner runs as),
    # then per-user fallbacks for someone running this script
    # interactively without bootstrap re-run.
    $isccCandidates = @(
        # Machine-scope (winget --scope machine, default Inno Setup installer)
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles        'Inno Setup 6\ISCC.exe'),
        # Per-user (winget default scope) - only readable by THIS user.
        (Join-Path $env:LOCALAPPDATA        'Programs\Inno Setup 6\ISCC.exe')
    )
    $iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

    # PATH fallback: when the user has installed Inno Setup at a
    # non-standard location and added it to system PATH manually,
    # Get-Command finds it without us hardcoding the directory. Tried
    # AFTER the well-known paths so a stale PATH entry can't shadow
    # the canonical install.
    if (-not $iscc) {
        $cmd = Get-Command iscc.exe -ErrorAction SilentlyContinue
        if (-not $cmd) { $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue }
        if ($cmd) { $iscc = $cmd.Source }
    }

    if (-not $iscc) {
        # Hard-fail with a diagnostic listing the exact paths probed.
        # The most common failure mode is service-account mismatch:
        # winget installed Inno Setup per-user for the bootstrap admin,
        # but act_runner runs as LOCAL SYSTEM and can't see another
        # user's %LOCALAPPDATA%. setup-windows-build.ps1 now uses
        # `--scope machine` to land it in C:\Program Files (x86)\
        # which IS visible to SYSTEM. If you're seeing this in CI,
        # re-run setup-windows-build.ps1 on the VM as Admin.
        Write-Host "Probed paths (none existed):" -ForegroundColor Red
        foreach ($p in $isccCandidates) { Write-Host "  $p" -ForegroundColor Red }
        Write-Host ("Running as: {0}" -f [Security.Principal.WindowsIdentity]::GetCurrent().Name) -ForegroundColor Red
        throw "Inno Setup 6 ISCC.exe not found. If running under act_runner (LOCAL SYSTEM), re-run packaging\windows\setup-windows-build.ps1 as Administrator on the VM -- its updated --scope machine flag installs ISCC.exe to a system-wide location."
    }

    # Pre-flight: confirm the deploy bundle actually exists where the
    # .iss expects to find it. The iss has:
    #     #define DeployDir "..\..\deploy-windows"
    # which is relative to the .iss file's location. Resolve it to
    # an absolute path here so any "missing source" failure surfaces
    # BEFORE ISCC runs (with a useful path) instead of as a generic
    # "system cannot find the path specified" from inside ISCC.
    $issDir       = $PSScriptRoot
    $expectedDeploy = (Resolve-Path (Join-Path $issDir '..\..\deploy-windows')).Path
    if (-not (Test-Path $expectedDeploy)) {
        throw "Deploy bundle not found at $expectedDeploy (the path the .iss expects). Did the windeployqt6 step run?"
    }
    $deployFileCount = (Get-ChildItem $expectedDeploy -Recurse -File).Count
    Info ("Deploy bundle: {0} ({1} files)" -f $expectedDeploy, $deployFileCount)

    # Run ISCC with CWD set to the .iss directory. Inno Setup resolves
    # [Files] Source paths and `#define`s of relative paths against
    # CWD on some Windows configurations rather than the .iss file's
    # own directory; pinning CWD to the .iss directory makes both
    # interpretations coincide. The Push/Pop pair guarantees we
    # restore CWD even if ISCC throws.
    Push-Location $issDir
    try {
        & $iscc 'verzeta-studio.iss'
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    Ok ("Installer built via {0}" -f $iscc)
}
