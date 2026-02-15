cmake_minimum_required(VERSION 3.25)
project(RemoteDesktop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ── Qt 6 ──────────────────────────────────────────────────────────────
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt")
elseif(WIN32)
    # Set this to your Qt installation, e.g.:
    #   cmake -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64 ..
    # or via environment variable QT_DIR
endif()
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

# ── FreeRDP 3 ─────────────────────────────────────────────────────────
if(APPLE)
    set(FreeRDP_DIR "/opt/homebrew/opt/freerdp3-custom/lib/cmake/FreeRDP3")
    set(FreeRDP-Client_DIR "/opt/homebrew/opt/freerdp3-custom/lib/cmake/FreeRDP-Client3")
    set(WinPR_DIR "/opt/homebrew/opt/freerdp3-custom/lib/cmake/WinPR3")
elseif(WIN32)
    # Set these to your FreeRDP build/install, e.g.:
    #   cmake -DFreeRDP_DIR=C:/freerdp/lib/cmake/FreeRDP3 ..
endif()
find_package(FreeRDP REQUIRED)
find_package(FreeRDP-Client REQUIRED)
find_package(WinPR REQUIRED)

# ── libssh2 ───────────────────────────────────────────────────────────
if(APPLE)
    find_library(LIBSSH2_LIBRARY ssh2 HINTS /opt/homebrew/opt/libssh2/lib)
    find_path(LIBSSH2_INCLUDE_DIR libssh2.h HINTS /opt/homebrew/opt/libssh2/include)
elseif(WIN32)
    # vcpkg provides this automatically via toolchain file, or set manually:
    #   cmake -DLIBSSH2_LIBRARY=C:/vcpkg/installed/x64-windows/lib/libssh2.lib
    #         -DLIBSSH2_INCLUDE_DIR=C:/vcpkg/installed/x64-windows/include ..
    find_library(LIBSSH2_LIBRARY NAMES ssh2 libssh2)
    find_path(LIBSSH2_INCLUDE_DIR libssh2.h)
endif()

# ── libvterm ──────────────────────────────────────────────────────────
if(APPLE)
    find_library(LIBVTERM_LIBRARY vterm HINTS /opt/homebrew/opt/libvterm/lib)
    find_path(LIBVTERM_INCLUDE_DIR vterm.h HINTS /opt/homebrew/opt/libvterm/include)
elseif(WIN32)
    # libvterm must be built from source on Windows. Set paths manually:
    #   cmake -DLIBVTERM_LIBRARY=C:/libvterm/build/Release/vterm.lib
    #         -DLIBVTERM_INCLUDE_DIR=C:/libvterm/include ..
    find_library(LIBVTERM_LIBRARY NAMES vterm)
    find_path(LIBVTERM_INCLUDE_DIR vterm.h)
endif()

# ── OpenSSL 3 ─────────────────────────────────────────────────────────
if(APPLE)
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
elseif(WIN32)
    # vcpkg provides this automatically, or set:
    #   cmake -DOPENSSL_ROOT_DIR=C:/vcpkg/installed/x64-windows ..
endif()
find_package(OpenSSL REQUIRED)

# ── Sources ───────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/app/Application.cpp
    src/core/ConnectionEntry.cpp
    src/core/Folder.cpp
    src/core/Credential.cpp
    src/core/config/ConfigManager.cpp
    src/core/connectiondb/ConnectionDatabase.cpp
    src/core/userdb/UserDatabase.cpp
    src/core/credentials/CredentialVault.cpp
    src/core/migration/LegacyMigrator.cpp
    src/core/importexport/DevolutionsImporter.cpp
    src/core/importexport/ConnectionExporter.cpp
    src/core/rdp/RdpClient.cpp
    src/core/rdp/RdpSession.cpp
    src/core/ssh/SshSession.cpp
    src/core/ssh/SshChannel.cpp
    src/ui/mainwindow/MainWindow.cpp
    src/ui/treeview/TreeItem.cpp
    src/ui/treeview/ConnectionTreeModel.cpp
    src/ui/treeview/ConnectionTreeView.cpp
    src/ui/tabview/SessionTabWidget.cpp
    src/ui/sessionview/RdpSessionWidget.cpp
    src/ui/sessionview/SshSessionWidget.cpp
    src/ui/dialogs/ConnectionDialog.cpp
    src/ui/dialogs/FolderDialog.cpp
    src/ui/dialogs/MasterPasswordDialog.cpp
    src/ui/dialogs/CredentialDialog.cpp
    src/ui/dialogs/CredentialManagerDialog.cpp
    src/ui/dialogs/SetupDialog.cpp
)

set(HEADERS
    src/app/Application.h
    src/core/Enums.h
    src/core/ConnectionEntry.h
    src/core/Folder.h
    src/core/Credential.h
    src/core/config/ConfigManager.h
    src/core/connectiondb/Schema.h
    src/core/connectiondb/ConnectionDatabase.h
    src/core/userdb/UserSchema.h
    src/core/userdb/UserDatabase.h
    src/core/credentials/CredentialVault.h
    src/core/migration/LegacyMigrator.h
    src/core/importexport/DevolutionsImporter.h
    src/core/importexport/ConnectionExporter.h
    src/core/rdp/RdpClient.h
    src/core/rdp/RdpSession.h
    src/core/ssh/SshSession.h
    src/core/ssh/SshChannel.h
    src/ui/mainwindow/MainWindow.h
    src/ui/treeview/TreeItem.h
    src/ui/treeview/ConnectionTreeModel.h
    src/ui/treeview/ConnectionTreeView.h
    src/ui/tabview/SessionTabWidget.h
    src/ui/sessionview/RdpSessionWidget.h
    src/ui/sessionview/SshSessionWidget.h
    src/ui/dialogs/ConnectionDialog.h
    src/ui/dialogs/FolderDialog.h
    src/ui/dialogs/MasterPasswordDialog.h
    src/ui/dialogs/CredentialDialog.h
    src/ui/dialogs/CredentialManagerDialog.h
    src/ui/dialogs/SetupDialog.h
)

set(RESOURCES
    resources/resources.qrc
)

# ── Application icon ──────────────────────────────────────────────────
if(APPLE)
    set(MACOSX_BUNDLE_ICON_FILE appicon.icns)
    set(APP_ICON_MACOS "${CMAKE_SOURCE_DIR}/resources/appicon.icns")
    set_source_files_properties(${APP_ICON_MACOS} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
elseif(WIN32)
    set(APP_ICON_WIN32 "${CMAKE_SOURCE_DIR}/resources/appicon.rc")
endif()

# ── Executable ────────────────────────────────────────────────────────
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

if(APPLE)
    add_executable(${PROJECT_NAME} MACOSX_BUNDLE
        ${SOURCES} ${HEADERS} ${RESOURCES}
        ${APP_ICON_MACOS}
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Remote Desktop Manager"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.remotedesktop.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
elseif(WIN32)
    add_executable(${PROJECT_NAME} WIN32
        ${SOURCES} ${HEADERS} ${RESOURCES}
        ${APP_ICON_WIN32}
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
else()
    add_executable(${PROJECT_NAME}
        ${SOURCES} ${HEADERS} ${RESOURCES}
    )
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${FreeRDP_INCLUDE_DIR}
    ${LIBSSH2_INCLUDE_DIR}
    ${LIBVTERM_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Sql
    freerdp
    freerdp-client
    winpr
    ${LIBSSH2_LIBRARY}
    ${LIBVTERM_LIBRARY}
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Windows needs ws2_32 for networking
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 crypt32)
endif()
