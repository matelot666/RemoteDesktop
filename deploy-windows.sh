#!/usr/bin/env bash
set -euo pipefail

HOST="jumpbox"
SRC_DIR="C:/remotedesktop"
BUILD_DIR="C:/remotedesktop/build"
RELEASE_DIR="C:/remotedesktop/build/Release"
INSTALL_DIR="C:\\Program Files\\RemoteDesktop"
QT_DIR="C:/Qt/6.8.3/msvc2022_64"
FREERDP_DIR="C:/freerdp"
VCPKG_BIN="C:/vcpkg/installed/x64-windows/bin"
VTERM_INC="C:/libvterm/include"
VTERM_LIB="C:/libvterm/lib/vterm.lib"

echo "==> Copying source to $HOST..."
scp -r src CMakeLists.txt resources "$HOST:$SRC_DIR/"

echo "==> Configuring..."
ssh "$HOST" "cd /d $SRC_DIR && rmdir /s /q build 2>nul & mkdir build && cd build && cmake .. \
  -G \"Visual Studio 17 2022\" -A x64 \
  -DCMAKE_PREFIX_PATH=\"$QT_DIR;$FREERDP_DIR;C:/vcpkg/installed/x64-windows\" \
  -DLIBVTERM_INCLUDE_DIR=$VTERM_INC \
  -DLIBVTERM_LIBRARY=$VTERM_LIB"

echo "==> Building Release..."
ssh "$HOST" "cd /d $BUILD_DIR && cmake --build . --config Release"

echo "==> Running windeployqt..."
ssh "$HOST" "$QT_DIR/bin/windeployqt.exe $RELEASE_DIR/RemoteDesktop.exe"

echo "==> Copying runtime DLLs into Release dir..."
ssh "$HOST" "robocopy C:\freerdp\bin $RELEASE_DIR *.dll /NP /NFL /NDL" || true
ssh "$HOST" "robocopy $VCPKG_BIN $RELEASE_DIR libssh2.dll libssl-3-x64.dll libcrypto-3-x64.dll zlib1.dll /NP /NFL /NDL" || true

echo "==> Installing to $INSTALL_DIR..."
ssh "$HOST" "robocopy $RELEASE_DIR \"$INSTALL_DIR\" /MIR /NP /NFL /NDL" || true

echo "==> Creating desktop shortcut..."
printf '$ws = New-Object -ComObject WScript.Shell\r\n$lnk = $ws.CreateShortcut("$([Environment]::GetFolderPath('"'"'Desktop'"'"'))\\RemoteDesktop.lnk")\r\n$lnk.TargetPath = "%s\\RemoteDesktop.exe"\r\n$lnk.WorkingDirectory = "%s"\r\n$lnk.Description = "Remote Desktop Manager"\r\n$lnk.Save()\r\n' \
  "$INSTALL_DIR" "$INSTALL_DIR" \
  | ssh "$HOST" "powershell -Command -"

echo "==> Done. RemoteDesktop installed to $INSTALL_DIR on $HOST."
