#!/bin/sh
# Builds Serat-<version>-x86_64.AppImage. Run from the source folder with the Qt to bundle on PATH (or QMAKE set).
set -eu
build=${1:-build-appimage}
source=$(pwd)
version=$(sed -n 's/^project(serat VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build "$build"
build=$(cd "$build" && pwd)
rm -rf "$build/AppDir"
DESTDIR="$build/AppDir" cmake --install "$build"

tools="$build/tools"
mkdir -p "$tools"
for tool in linuxdeploy linuxdeploy-plugin-qt; do
    file="$tools/$tool-x86_64.AppImage"
    if [ ! -x "$file" ]; then
        curl -fsSL -o "$file" "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-x86_64.AppImage"
        chmod +x "$file"
    fi
done

export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
if [ -e "$plugins/platforms/libqwayland-generic.so" ]; then
    export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
fi
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export APPIMAGE_EXTRACT_AND_RUN=1 # no FUSE needed, as on CI runners
export NO_STRIP=1 # the strip inside linuxdeploy is too old for libraries from newer distributions
cd "$build"
PATH="$tools:$PATH" "$tools/linuxdeploy-x86_64.AppImage" --appdir AppDir --plugin qt --output appimage \
    --desktop-file AppDir/usr/share/applications/id.icqn.Serat.desktop \
    --icon-file AppDir/usr/share/icons/hicolor/scalable/apps/id.icqn.Serat.svg \
    --custom-apprun "$source/packaging/AppRun"
