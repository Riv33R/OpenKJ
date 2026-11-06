<#
.SYNOPSIS
    Скрипт установки зависимостей (Qt 5.15.2 MinGW, MinGW-w64 8.1, GStreamer, Ninja)
    и одновременной сборки OpenKJ под Windows x64 (build) и Windows x86 (build_x86).
#>

param(
    [ValidateSet("all", "x64", "x86")]
    [string]$Arch = "all",
    [switch]$InstallDeps = $false,
    [string]$DepsDir = "F:\Karaoke\deps",
    [string]$GstVersion = "1.26.11"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectRoot

$BasePath = $env:PATH
$PythonScripts = "C:\Users\dimag\AppData\Local\Programs\Python\Python312\Scripts"

function Resolve-GstRoot {
    param(
        [string]$ExtractDir,
        [string]$SubDirName
    )
    $candidates = @(
        "$ExtractDir\PFiles64\gstreamer\1.0\$SubDirName",
        "$ExtractDir\PFiles\gstreamer\1.0\$SubDirName",
        "$ExtractDir\gstreamer\1.0\$SubDirName"
    )
    foreach ($c in $candidates) {
        if (Test-Path "$c\include\gstreamer-1.0") {
            return $c
        }
    }
    $found = Get-ChildItem -Path $ExtractDir -Directory -Recurse -Filter $SubDirName -ErrorAction SilentlyContinue |
        Where-Object { Test-Path "$($_.FullName)\include\gstreamer-1.0" } |
        Select-Object -First 1
    if ($found) {
        return $found.FullName
    }
    return $candidates[0]
}

function Download-GstMsi {
    param(
        [string]$MsiPrefix,
        [string]$OutFile,
        [string[]]$Versions
    )
    if ((Test-Path $OutFile) -and ((Get-Item $OutFile).Length -gt 50MB)) {
        return
    }
    if (Test-Path $OutFile) {
        Remove-Item $OutFile -Force -ErrorAction SilentlyContinue
    }
    foreach ($ver in $Versions) {
        $url = "https://gstreamer.freedesktop.org/data/pkg/windows/$ver/mingw/$MsiPrefix-$ver.msi"
        Write-Host "Downloading GStreamer package (16 parallel streams): $url"
        $pyCode = @"
import sys, os, urllib.request, concurrent.futures

url, out_file = sys.argv[1], sys.argv[2]
part_file = out_file + '.part'
req = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'Mozilla/5.0'})
with urllib.request.urlopen(req, timeout=30) as resp:
    total = int(resp.headers['Content-Length'])
print(f'Total size: {total / (1024*1024):.1f} MB', flush=True)

with open(part_file, 'wb') as f:
    f.truncate(total)

num_chunks = 32
chunk_size = (total + num_chunks - 1) // num_chunks
ranges = [(i * chunk_size, min(total - 1, (i + 1) * chunk_size - 1)) for i in range(num_chunks) if i * chunk_size < total]

def fetch_range(r):
    start, end = r
    for attempt in range(5):
        try:
            req = urllib.request.Request(url, headers={'Range': f'bytes={start}-{end}', 'User-Agent': 'Mozilla/5.0'})
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = resp.read()
            if len(data) != (end - start + 1):
                raise RuntimeError('Incomplete chunk')
            with open(part_file, 'r+b') as f:
                f.seek(start)
                f.write(data)
            return len(data)
        except Exception as e:
            if attempt == 4:
                raise

done = 0
with concurrent.futures.ThreadPoolExecutor(max_workers=16) as ex:
    for sz in ex.map(fetch_range, ranges):
        done += sz
        print(f'Downloaded {done / (1024*1024):.1f} / {total / (1024*1024):.1f} MB', flush=True)

os.replace(part_file, out_file)
"@
        $pyScript = Join-Path $DepsDir "downloads\fast_dl.py"
        Set-Content -Path $pyScript -Value $pyCode -Encoding UTF8
        & python $pyScript "$url" "$OutFile"
        if ($LASTEXITCODE -eq 0 -and (Test-Path $OutFile)) {
            Write-Host "Downloaded $MsiPrefix-$ver.msi"
            return
        }
        if (Test-Path $OutFile) {
            Remove-Item $OutFile -Force -ErrorAction SilentlyContinue
        }
    }
    throw "Failed to download $MsiPrefix for versions: $($Versions -join ', ')"
}

function Build-OpenKJTarget {
    param(
        [string]$TargetArch
    )

    if ($TargetArch -eq "x64") {
        $QtArch = "win64_mingw81"
        $MingwPkg = "qt.tools.win64_mingw810"
        $QtDir = "$DepsDir\Qt\5.15.2\mingw81_64"
        $MingwDir = "$DepsDir\Qt\Tools\mingw810_64"
        $GstExtractDir = "$DepsDir\gstreamer"
        $GstSubDir = "mingw_x86_64"
        $GstMsiArch = "mingw-x86_64"
        $BuildDirName = "build"
    } else {
        $QtArch = "win32_mingw81"
        $MingwPkg = "qt.tools.win32_mingw810"
        $QtDir = "$DepsDir\Qt\5.15.2\mingw81_32"
        $MingwDir = "$DepsDir\Qt\Tools\mingw810_32"
        $GstExtractDir = "$DepsDir\gstreamer_x86"
        $GstSubDir = "mingw_x86"
        $GstMsiArch = "mingw-x86"
        $BuildDirName = "build_x86"
    }

    New-Item -ItemType Directory -Force -Path "$DepsDir\Qt", "$GstExtractDir", "$DepsDir\downloads" | Out-Null

    $GstRoot = Resolve-GstRoot -ExtractDir $GstExtractDir -SubDirName $GstSubDir
    $NeedGstRuntime = -not (Test-Path "$GstRoot\bin\libgstreamer-1.0-0.dll")
    $NeedGstDevel   = -not (Test-Path "$GstRoot\include\gstreamer-1.0")
    $NeedDeps = $InstallDeps -or (-not (Test-Path $QtDir)) -or (-not (Test-Path $MingwDir)) -or $NeedGstRuntime -or $NeedGstDevel

    if ($NeedDeps) {
        Write-Host "=== Ensuring dependencies for $TargetArch ==="
        if (-not (Get-Command aqt -ErrorAction SilentlyContinue)) {
            python -m pip install --upgrade aqtinstall ninja
        }
        if (-not (Test-Path $QtDir)) {
            Write-Host "Installing Qt 5.15.2 ($QtArch)..."
            python -m aqt install-qt windows desktop 5.15.2 $QtArch -O "$DepsDir\Qt" -m qtnetworkauth
        }
        if (-not (Test-Path $MingwDir)) {
            Write-Host "Installing MinGW ($MingwPkg)..."
            python -m aqt install-tool windows desktop tools_mingw $MingwPkg -O "$DepsDir\Qt"
        }
        $CandidateVersions = @($GstVersion, "1.24.12", "1.22.12", "1.20.7") | Select-Object -Unique
        $ExtractPy = "$DepsDir\downloads\extract_msi.py"

        if ($NeedGstRuntime) {
            $RuntimePkg = "$DepsDir\downloads\gstreamer-runtime-$TargetArch.pkg"
            Download-GstMsi -MsiPrefix "gstreamer-1.0-$GstMsiArch" -OutFile $RuntimePkg -Versions $CandidateVersions
            Write-Host "Extracting GStreamer runtime ($TargetArch) to $GstExtractDir..."
            & python $ExtractPy "$RuntimePkg" "$GstExtractDir"
            if ($LASTEXITCODE -ne 0) { throw "Failed to extract $RuntimePkg" }
        }
        if ($NeedGstDevel) {
            $DevelPkg = "$DepsDir\downloads\gstreamer-devel-$TargetArch.pkg"
            if (Test-Path "$DepsDir\downloads\gstreamer-devel-$TargetArch.msi") {
                $DevelPkg = "$DepsDir\downloads\gstreamer-devel-$TargetArch.msi"
            } else {
                Download-GstMsi -MsiPrefix "gstreamer-1.0-devel-$GstMsiArch" -OutFile $DevelPkg -Versions $CandidateVersions
            }
            Write-Host "Extracting GStreamer devel ($TargetArch) to $GstExtractDir..."
            & python $ExtractPy "$DevelPkg" "$GstExtractDir"
            if ($LASTEXITCODE -ne 0) { throw "Failed to extract $DevelPkg" }
        }
        $GstRoot = Resolve-GstRoot -ExtractDir $GstExtractDir -SubDirName $GstSubDir
    }

    if (-not (Test-Path "$GstRoot\include\gstreamer-1.0")) {
        throw "GStreamer root not found for $TargetArch at $GstRoot"
    }

    Write-Host "=== Building OpenKJ ($TargetArch) in $BuildDirName ==="
    $env:PATH = "$MingwDir\bin;$QtDir\bin;$GstRoot\bin;$PythonScripts;$BasePath"

    $BuildDir = Join-Path $ProjectRoot $BuildDirName
    $CmakeArgs = @(
        "-B", $BuildDir,
        "-S", $ProjectRoot,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_PREFIX_PATH=$($QtDir -replace '\\','/')",
        "-DGST_BASE_PATH=$($GstRoot -replace '\\','/')"
    )

    $CachedSpdlog = Join-Path $ProjectRoot "build\_deps\spdlog-src"
    if (Test-Path "$CachedSpdlog\CMakeLists.txt") {
        $CmakeArgs += "-DFETCHCONTENT_SOURCE_DIR_SPDLOG=$($CachedSpdlog -replace '\\','/')"
    }

    & cmake @CmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $TargetArch" }

    & cmake --build $BuildDir --parallel
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $TargetArch" }

    Write-Host "=== Deploying portable runtime for $TargetArch ($BuildDirName) ==="
    & "$QtDir\bin\windeployqt.exe" "$BuildDir\openkj.exe"
    Copy-Item "$GstRoot\bin\*.dll" "$BuildDir\" -Force
    Copy-Item "$QtDir\bin\Qt5*.dll" "$BuildDir\" -Force
    if (Test-Path "$MingwDir\opt\bin") {
        Copy-Item "$MingwDir\opt\bin\*.dll" "$BuildDir\" -Force -ErrorAction SilentlyContinue
    }
    Copy-Item "$MingwDir\bin\libgcc_s_*.dll" "$BuildDir\" -Force -ErrorAction SilentlyContinue
    Copy-Item "$MingwDir\bin\libstdc++-6.dll" "$BuildDir\" -Force -ErrorAction SilentlyContinue
    Copy-Item "$MingwDir\bin\libwinpthread-1.dll" "$BuildDir\" -Force -ErrorAction SilentlyContinue

    if (Test-Path "$GstRoot\lib\gstreamer-1.0") {
        New-Item -ItemType Directory -Force -Path "$BuildDir\lib\gstreamer-1.0" | Out-Null
        Copy-Item "$GstRoot\lib\gstreamer-1.0\*.dll" "$BuildDir\lib\gstreamer-1.0\" -Force
    }
    if (Test-Path "$GstRoot\libexec\gstreamer-1.0\gst-plugin-scanner.exe") {
        Copy-Item "$GstRoot\libexec\gstreamer-1.0\gst-plugin-scanner.exe" "$BuildDir\" -Force
        New-Item -ItemType Directory -Force -Path "$BuildDir\libexec\gstreamer-1.0" | Out-Null
        Copy-Item "$GstRoot\libexec\gstreamer-1.0\gst-plugin-scanner.exe" "$BuildDir\libexec\gstreamer-1.0\" -Force
    }

    # Remove hardware/API-incompatible plugins that require external drivers or cause DLL collisions
    if ($TargetArch -eq "x86") {
        $IncompatiblePlugins = @(
            "libgstamfcodec.dll", "libgstasio.dll", "libgstd3d11.dll", "libgstd3d12.dll",
            "libgstdecklink.dll", "libgstdwrite.dll", "libgstmediafoundation.dll",
            "libgstnvcodec.dll", "libgstopenh264.dll", "libgstqsv.dll", "libgstsoundtouch.dll",
            "libgstsrt.dll", "libgsttaglib.dll", "libgstwasapi.dll", "libgstwebrtcdsp.dll",
            "libgstwin32ipc.dll"
        )
        foreach ($p in $IncompatiblePlugins) {
            $pPath = Join-Path $BuildDir "lib\gstreamer-1.0\$p"
            if (Test-Path $pPath) { Remove-Item $pPath -Force }
        }
    } else {
        $IncompatiblePlugins = @(
            "libgstamfcodec.dll", "libgstd3d11.dll", "libgstd3d12.dll", "libgstdwrite.dll",
            "libgstmediafoundation.dll", "libgstnvcodec.dll", "libgstqsv.dll",
            "libgstsoundtouch.dll", "libgstsrt.dll", "libgstwebrtcdsp.dll", "libgstwin32ipc.dll"
        )
        foreach ($p in $IncompatiblePlugins) {
            $pPath = Join-Path $BuildDir "lib\gstreamer-1.0\$p"
            if (Test-Path $pPath) { Remove-Item $pPath -Force }
        }
    }

    $LocalRunBat = Join-Path $BuildDir "run.bat"
    $LocalRunBatContent = @"
@echo off
setlocal
set "APP_DIR=%~dp0"
set "PATH=%APP_DIR%;%PATH%"
cd /d "%APP_DIR%"
start "" "%APP_DIR%openkj.exe" %*
"@
    Set-Content -Path $LocalRunBat -Value $LocalRunBatContent -Encoding ASCII

    $DeployDir = Join-Path $ProjectRoot "deploy_$TargetArch"
    if (Test-Path $DeployDir) { Remove-Item $DeployDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $DeployDir | Out-Null

    Get-ChildItem -Path $BuildDir -File | Where-Object { $_.Extension -in '.exe', '.dll', '.bat' } | Copy-Item -Destination $DeployDir -Force
    foreach ($sub in @('lib', 'libexec', 'platforms', 'sqldrivers', 'imageformats', 'iconengines', 'styles', 'bearer', 'translations')) {
        $srcSub = Join-Path $BuildDir $sub
        if (Test-Path $srcSub) {
            Copy-Item -Path $srcSub -Destination (Join-Path $DeployDir $sub) -Recurse -Force
        }
    }

    $ZipPath = Join-Path $ProjectRoot "OpenKJ-$TargetArch-portable.zip"
    Write-Host "=== Creating clean portable archive: $ZipPath ==="
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    Compress-Archive -Path "$DeployDir\*" -DestinationPath $ZipPath -Force
    Remove-Item $DeployDir -Recurse -Force -ErrorAction SilentlyContinue

    Write-Host "=== Successfully built and deployed $TargetArch -> $BuildDir\openkj.exe and $ZipPath ==="
}

if ($Arch -eq "all" -or $Arch -eq "x64") {
    Build-OpenKJTarget -TargetArch "x64"
}
if ($Arch -eq "all" -or $Arch -eq "x86") {
    Build-OpenKJTarget -TargetArch "x86"
}
