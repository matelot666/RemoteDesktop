#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installs all build dependencies for Remote Desktop Manager on Windows x64.

.DESCRIPTION
    Uses Chocolatey for toolchain, vcpkg for C libraries, aqtinstall for Qt 6,
    and builds FreeRDP 3 and libvterm from source.

    Run from an elevated PowerShell prompt:
        .\scripts\setup-windows.ps1

    After completion, build with:
        mkdir build && cd build
        cmake -G "Visual Studio 17 2022" -A x64 `
            -DCMAKE_PREFIX_PATH="C:\Qt\6.8.3\msvc2022_64" `
            -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" `
            -DFreeRDP_DIR="C:\freerdp\lib\cmake\FreeRDP3" `
            -DFreeRDP-Client_DIR="C:\freerdp\lib\cmake\FreeRDP-Client3" `
            -DWinPR_DIR="C:\freerdp\lib\cmake\WinPR3" `
            -DLIBVTERM_LIBRARY="C:\libvterm\build\Release\vterm.lib" `
            -DLIBVTERM_INCLUDE_DIR="C:\libvterm\include" ..
        cmake --build . --config Release
#>

param(
    [string]$QtVersion   = "6.8.3",
    [string]$QtArch      = "win64_msvc2022_64",
    [string]$QtDir       = "C:\Qt",
    [string]$VcpkgDir    = "C:\vcpkg",
    [string]$FreeRdpDir  = "C:\freerdp-src",
    [string]$FreeRdpInstall = "C:\freerdp",
    [string]$LibvtermDir = "C:\libvterm-src",
    [string]$LibvtermInstall = "C:\libvterm"
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) {
    Write-Host "`n===> $msg" -ForegroundColor Cyan
}

# ── Chocolatey ───────────────────────────────────────────────────────
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Step "Installing Chocolatey"
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12
    Invoke-Expression ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
    $env:Path = "$env:ALLUSERSPROFILE\chocolatey\bin;$env:Path"
}

# ── Build toolchain ──────────────────────────────────────────────────
Write-Step "Installing build tools via Chocolatey"
choco install -y cmake --installargs 'ADD_CMAKE_TO_PATH=System'
choco install -y git
choco install -y python3
choco install -y visualstudio2022buildtools --package-parameters `
    "--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"

# Refresh PATH
$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")

# ── vcpkg (OpenSSL + libssh2) ────────────────────────────────────────
Write-Step "Setting up vcpkg"
if (-not (Test-Path "$VcpkgDir\vcpkg.exe")) {
    git clone https://github.com/microsoft/vcpkg.git $VcpkgDir
    & "$VcpkgDir\bootstrap-vcpkg.bat" -disableMetrics
}
$env:Path = "$VcpkgDir;$env:Path"

Write-Step "Installing OpenSSL and libssh2 via vcpkg"
vcpkg install openssl:x64-windows libssh2:x64-windows
vcpkg integrate install

# ── Qt 6 via aqtinstall ──────────────────────────────────────────────
Write-Step "Installing Qt $QtVersion via aqtinstall"
pip install aqtinstall
if (-not (Test-Path "$QtDir\$QtVersion")) {
    aqt install-qt windows desktop $QtVersion $QtArch --outputdir $QtDir
}
$QtPrefix = "$QtDir\$QtVersion\msvc2022_64"
Write-Host "  Qt installed to: $QtPrefix"

# ── Find VS developer environment ───────────────────────────────────
function Enter-VsDevShell {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vsWhere -latest -property installationPath
    $devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
    if (Test-Path $devShell) {
        & $devShell -Arch amd64 -SkipAutomaticLocation
    } else {
        Write-Warning "Could not find VS dev shell. Make sure VS 2022 Build Tools are installed."
    }
}
Enter-VsDevShell

# ── FreeRDP 3 from source ────────────────────────────────────────────
Write-Step "Building FreeRDP 3 from source"
if (-not (Test-Path $FreeRdpDir)) {
    git clone --depth 1 --branch stable-3.0 https://github.com/FreeRDP/FreeRDP.git $FreeRdpDir
}
$freerdpBuild = "$FreeRdpDir\build"
if (-not (Test-Path "$FreeRdpInstall\lib\cmake\FreeRDP3")) {
    cmake -S $FreeRdpDir -B $freerdpBuild -G "Visual Studio 17 2022" -A x64 `
        -DCMAKE_INSTALL_PREFIX="$FreeRdpInstall" `
        -DCMAKE_TOOLCHAIN_FILE="$VcpkgDir\scripts\buildsystems\vcpkg.cmake" `
        -DWITH_X11=OFF `
        -DWITH_WAYLAND=OFF `
        -DWITH_SDL=OFF `
        -DWITH_CLIENT=OFF `
        -DWITH_SERVER=OFF `
        -DWITH_FFMPEG=OFF `
        -DWITH_OPUS=OFF `
        -DWITH_SAMPLE=OFF
    cmake --build $freerdpBuild --config Release --parallel
    cmake --install $freerdpBuild --config Release
}
Write-Host "  FreeRDP installed to: $FreeRdpInstall"

# ── libvterm from source ─────────────────────────────────────────────
Write-Step "Building libvterm from source"
if (-not (Test-Path $LibvtermDir)) {
    git clone --depth 1 https://github.com/neovim/libvterm.git $LibvtermDir
}

# libvterm doesn't ship a CMakeLists.txt — create a minimal one
$vtermCMake = @"
cmake_minimum_required(VERSION 3.20)
project(vterm C)

file(GLOB VTERM_SRC src/*.c)
add_library(vterm STATIC `${VTERM_SRC})
target_include_directories(vterm PUBLIC include)

install(TARGETS vterm ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
"@

$vtermCMakePath = "$LibvtermDir\CMakeLists.txt"
if (-not (Test-Path $vtermCMakePath)) {
    Set-Content -Path $vtermCMakePath -Value $vtermCMake
}

$vtermBuild = "$LibvtermDir\build"
if (-not (Test-Path "$LibvtermInstall\lib\vterm.lib")) {
    cmake -S $LibvtermDir -B $vtermBuild -G "Visual Studio 17 2022" -A x64 `
        -DCMAKE_INSTALL_PREFIX="$LibvtermInstall"
    cmake --build $vtermBuild --config Release --parallel
    cmake --install $vtermBuild --config Release
}
Write-Host "  libvterm installed to: $LibvtermInstall"

# ── Summary ──────────────────────────────────────────────────────────
Write-Step "All dependencies installed"
Write-Host ""
Write-Host "Build the project with:" -ForegroundColor Green
Write-Host ""
Write-Host "  cd $(Get-Location)"
Write-Host "  mkdir build; cd build"
Write-Host "  cmake -G `"Visual Studio 17 2022`" -A x64 ``"
Write-Host "      -DCMAKE_PREFIX_PATH=`"$QtPrefix`" ``"
Write-Host "      -DCMAKE_TOOLCHAIN_FILE=`"$VcpkgDir\scripts\buildsystems\vcpkg.cmake`" ``"
Write-Host "      -DFreeRDP_DIR=`"$FreeRdpInstall\lib\cmake\FreeRDP3`" ``"
Write-Host "      -DFreeRDP-Client_DIR=`"$FreeRdpInstall\lib\cmake\FreeRDP-Client3`" ``"
Write-Host "      -DWinPR_DIR=`"$FreeRdpInstall\lib\cmake\WinPR3`" ``"
Write-Host "      -DLIBVTERM_LIBRARY=`"$LibvtermInstall\lib\vterm.lib`" ``"
Write-Host "      -DLIBVTERM_INCLUDE_DIR=`"$LibvtermInstall\include`" .."
Write-Host "  cmake --build . --config Release"
Write-Host ""
