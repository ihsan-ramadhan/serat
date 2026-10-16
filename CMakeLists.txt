cmake_minimum_required(VERSION 3.21)
project(serat VERSION 0.1.0 DESCRIPTION "Native Markdown notes, compatible with Obsidian vaults" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg Network PrintSupport)
qt_standard_project_setup()

# md4c: the system package, or its single source file built from upstream.
find_package(md4c CONFIG QUIET)
if(NOT TARGET md4c::md4c)
    include(FetchContent)
    FetchContent_Declare(md4c_src
        URL https://github.com/mity/md4c/archive/refs/tags/release-0.5.2.tar.gz
        URL_HASH SHA256=55d0111d48fb11883aaee91465e642b8b640775a4d6993c2d0e7a8092758ef21
        SOURCE_SUBDIR do-not-add)
    FetchContent_MakeAvailable(md4c_src)
    add_library(md4c_vendored STATIC ${md4c_src_SOURCE_DIR}/src/md4c.c)
    target_include_directories(md4c_vendored PUBLIC ${md4c_src_SOURCE_DIR}/src)
    add_library(md4c::md4c ALIAS md4c_vendored)
endif()

# Optional: syntax highlighting for fenced code blocks.
find_package(KF6SyntaxHighlighting QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(HUNSPELL QUIET IMPORTED_TARGET hunspell)
endif()

# Everything but main(), shared by the app and the tests.
add_library(serat_core STATIC
    src/Vault.cpp src/Vault.h
    src/Recovery.cpp src/Recovery.h
    src/Markdown.cpp src/Markdown.h
    src/MathRender.cpp src/MathRender.h
    src/CodeHighlight.cpp src/CodeHighlight.h
    src/MarkdownHighlighter.cpp src/MarkdownHighlighter.h
    src/Theme.cpp src/Theme.h
    src/PathActions.cpp src/PathActions.h
    src/TableEditor.cpp src/TableEditor.h
    src/Frontmatter.cpp src/Frontmatter.h
    src/SearchQuery.cpp src/SearchQuery.h
    src/Settings.cpp src/Settings.h
    src/SettingsDialog.cpp src/SettingsDialog.h
    src/RecoveryDialog.cpp src/RecoveryDialog.h
    src/TextMerge.cpp src/TextMerge.h
    src/Properties.cpp src/Properties.h
    src/EmbedBlocks.cpp src/EmbedBlocks.h
    src/Diagrams.cpp src/Diagrams.h
    src/Export.cpp src/Export.h
    src/Spelling.cpp src/Spelling.h
    src/Graph.cpp src/Graph.h
    src/Editor.cpp src/Editor.h
    src/Reader.cpp src/Reader.h
    src/NoteView.cpp src/NoteView.h
    src/Panels.cpp src/Panels.h
    src/QuickSwitcher.cpp src/QuickSwitcher.h
    src/CommandPalette.cpp src/CommandPalette.h
    src/MainWindow.cpp src/MainWindow.h
)
target_include_directories(serat_core PUBLIC src)
target_link_libraries(serat_core PUBLIC Qt6::Widgets Qt6::Svg Qt6::Network Qt6::PrintSupport md4c::md4c)
target_compile_definitions(serat_core PUBLIC QT_NO_KEYWORDS SERAT_VERSION="${PROJECT_VERSION}")
if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(serat_core PUBLIC KF6::SyntaxHighlighting)
    target_compile_definitions(serat_core PUBLIC SERAT_HAVE_KSH)
endif()
if(HUNSPELL_FOUND)
    target_link_libraries(serat_core PUBLIC PkgConfig::HUNSPELL)
    target_compile_definitions(serat_core PUBLIC SERAT_HAVE_HUNSPELL)
endif()
# Interface translations: lupdate refreshes the .ts files (cmake --build build --target update_translations),
# lrelease turns them into .qm files built into the app. qt_id.ts has the few Qt strings Serat shows.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(Qt6LinguistTools_FOUND)
    qt_add_lupdate(serat_core TS_FILES translations/serat_id.ts OPTIONS -no-obsolete -locations none)
    qt_add_lrelease(serat_core TS_FILES translations/serat_id.ts translations/qt_id.ts QM_FILES_OUTPUT_VARIABLE serat_qm)
    qt_add_resources(serat_core "translations" PREFIX "/i18n" BASE ${CMAKE_CURRENT_BINARY_DIR} FILES ${serat_qm})
endif()

# Dictionaries next to the binary in the build tree, and in share/serat once installed.
file(COPY data/dictionaries DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(GLOB serat_icons RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS data/icons/*.svg)

qt_add_executable(serat src/main.cpp)
if(WIN32)
    target_sources(serat PRIVATE data/serat.rc)
elseif(APPLE)
    target_sources(serat PRIVATE data/serat.icns)
    set_source_files_properties(data/serat.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
qt_add_resources(serat "icons" PREFIX "/" BASE data FILES data/serat.svg ${serat_icons})
target_link_libraries(serat PRIVATE serat_core)

set_target_properties(serat PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_GUI_IDENTIFIER id.icqn.Serat
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/data/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME Serat
    MACOSX_BUNDLE_ICON_FILE serat.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_vault tests/tst_vault.cpp)
    target_link_libraries(tst_vault PRIVATE serat_core Qt6::Test)
    add_test(NAME tst_vault COMMAND tst_vault)
    set_tests_properties(tst_vault PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Drives the real windows offscreen: typing, clicks, menus and dialogs.
    qt_add_executable(tst_ui tests/tst_ui.cpp)
    qt_add_resources(tst_ui "icons" PREFIX "/" BASE data FILES data/serat.svg ${serat_icons})
    target_link_libraries(tst_ui PRIVATE serat_core Qt6::Test)
    add_test(NAME tst_ui COMMAND tst_ui)
    set_tests_properties(tst_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
endif()

include(GNUInstallDirs)
install(TARGETS serat BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES data/id.icqn.Serat.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/serat.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME id.icqn.Serat.svg)
    install(DIRECTORY data/dictionaries DESTINATION ${CMAKE_INSTALL_DATADIR}/serat)
    install(FILES data/id.icqn.Serat.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
else()
    if(APPLE)
        install(DIRECTORY data/dictionaries DESTINATION serat.app/Contents/Resources)
    else()
        install(DIRECTORY data/dictionaries DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    # windeployqt / macdeployqt via Qt's deploy script.
    qt_generate_deploy_app_script(TARGET serat OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${deploy_script})
endif()

# Packages: cpack -G NSIS (Windows installer), DragNDrop (macOS disk image) or TGZ/ZIP.
# Linux AppImage and Flatpak builds are in packaging/.
set(CPACK_PACKAGE_NAME Serat)
set(CPACK_PACKAGE_VENDOR "Muhammad Ihsan Ramadhan")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Markdown notes in a plain folder")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ihsan-ramadhan/serat")
set(CPACK_PACKAGE_INSTALL_DIRECTORY Serat)
set(CPACK_PACKAGE_EXECUTABLES serat Serat)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_FILE_NAME "Serat-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_NSIS_MUI_ICON ${CMAKE_CURRENT_SOURCE_DIR}/data/serat.ico)
set(CPACK_NSIS_INSTALLED_ICON_NAME bin/serat.exe)
set(CPACK_NSIS_DISPLAY_NAME Serat)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_CREATE_ICONS_EXTRA "CreateShortCut '$DESKTOP\\Serat.lnk' '$INSTDIR\\bin\\serat.exe'")
set(CPACK_NSIS_DELETE_ICONS_EXTRA "Delete '$DESKTOP\\Serat.lnk'")
set(CPACK_DMG_VOLUME_NAME Serat)
include(CPack)
