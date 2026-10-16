cmake_minimum_required(VERSION 3.21)
project(serat VERSION 0.1.0 DESCRIPTION "Native Markdown notes, compatible with Obsidian vaults" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg Network)
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

# Everything but main(), shared by the app and the tests.
add_library(serat_core STATIC
    src/Vault.cpp src/Vault.h
    src/Recovery.cpp src/Recovery.h
    src/Markdown.cpp src/Markdown.h
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
    src/Editor.cpp src/Editor.h
    src/Reader.cpp src/Reader.h
    src/NoteView.cpp src/NoteView.h
    src/Panels.cpp src/Panels.h
    src/QuickSwitcher.cpp src/QuickSwitcher.h
    src/CommandPalette.cpp src/CommandPalette.h
    src/MainWindow.cpp src/MainWindow.h
)
target_include_directories(serat_core PUBLIC src)
target_link_libraries(serat_core PUBLIC Qt6::Widgets Qt6::Svg Qt6::Network md4c::md4c)
target_compile_definitions(serat_core PUBLIC QT_NO_KEYWORDS SERAT_VERSION="${PROJECT_VERSION}")
if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(serat_core PUBLIC KF6::SyntaxHighlighting)
    target_compile_definitions(serat_core PUBLIC SERAT_HAVE_KSH)
endif()

file(GLOB serat_icons RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS data/icons/*.svg)

qt_add_executable(serat src/main.cpp)
qt_add_resources(serat "icons" PREFIX "/" BASE data FILES data/serat.svg ${serat_icons})
target_link_libraries(serat PRIVATE serat_core)

set_target_properties(serat PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_GUI_IDENTIFIER id.icqn.Serat
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/data/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME Serat
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_vault tests/tst_vault.cpp)
    target_link_libraries(tst_vault PRIVATE serat_core Qt6::Test)
    add_test(NAME tst_vault COMMAND tst_vault)

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
else()
    # windeployqt / macdeployqt via Qt's deploy script.
    qt_generate_deploy_app_script(TARGET serat OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${deploy_script})
endif()
