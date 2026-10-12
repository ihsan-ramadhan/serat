# Serat

A native Markdown notes app that opens Obsidian vaults as they are. Notes stay plain
`.md` files in a folder; Serat reads `[[wikilinks]]`, embeds, callouts and task lists the
way Obsidian writes them, and rewrites links when you rename or move a note.

Built with Qt 6 Widgets and C++20, so the same code runs on Linux, Windows and macOS.
Markdown is parsed with [md4c](https://github.com/mity/md4c); code blocks are coloured
by KSyntaxHighlighting when it is available.

## What works

- File tree with new, rename, delete (to the system trash) and drag-and-drop move
- Tabs with per-tab back/forward history, restored on the next start
- Editor with styled source: heading sizes, bold/italic/strike, code, links, callouts
- Reading view: tables, nested and task lists, callouts, code blocks with a copy link,
  image and note embeds (`![[x.png]]`, `![[note#Heading]]`), frontmatter
- `[[` autocomplete, Enter continues lists, Tab/Shift+Tab indent, checkboxes toggle
  from the reading view
- Link resolution follows Obsidian: exact path, then file name, preferring the note's
  own folder, then the shortest path
- Renaming a note or folder rewrites every link pointing into it, keeping headings and
  aliases; links that still resolve are left untouched
- Backlinks, outgoing links and outline panels; vault search; quick switcher
- Pasted screenshots are saved as `Pasted image <timestamp>.png`, like Obsidian
- Changes made outside the app (sync, git, another editor) are picked up live

Not there yet: graph view, canvas, live preview that hides markup while editing, tags
pane, properties editor, community plugins.

## Shortcuts

| Key | Action |
|---|---|
| Ctrl+O | Quick switcher (Ctrl+Enter opens in a new tab) |
| Ctrl+N | New note |
| Ctrl+E | Toggle reading view |
| Ctrl+Shift+F | Search vault |
| Ctrl+F | Find in note |
| F2 | Rename note |
| Ctrl+click | Follow a link in the editor (Ctrl+Shift+click: new tab) |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+Tab, Ctrl+W | Next tab, close tab |
| Ctrl+\\, Ctrl+Shift+\\ | Toggle left / right sidebar |
| Ctrl+= / Ctrl+- / Ctrl+0 | Zoom |

## Build

### Linux (Arch)

```sh
sudo pacman -S --needed qt6-base md4c syntax-highlighting cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build --prefix ~/.local
```

Debian/Ubuntu: `qt6-base-dev libmd4c-dev libkf6syntaxhighlighting-dev cmake ninja-build`.

### Windows

Install Qt 6 (MSVC) with the online installer, then from a "Qt 6 for Desktop" prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix dist
```

md4c is downloaded and built automatically when no system copy is found. The install
step runs `windeployqt`, so `dist` is a self-contained folder. KSyntaxHighlighting is
optional; without it code blocks render uncoloured.

### macOS

```sh
brew install qt md4c cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
cmake --install build --prefix dist   # runs macdeployqt, produces Serat.app
```

## Usage

```sh
serat ~/Documents/Obsidian\ Vault
```

Without an argument Serat reopens the last vault, or shows the vaults Obsidian knows
about. Serat never writes to `.obsidian/`; it only reads `app.json` to honour the
"confirm file deletion" setting.

## Credits

Serat exists because of [Obsidian](https://obsidian.md). The vault format, the
`[[wikilink]]` and `![[embed]]` syntax, callouts, the link resolution rules and the
"Pasted image" naming all follow how Obsidian does things, so a vault can move between
the two apps without changes. Serat is an independent project: it contains no Obsidian
code and is not affiliated with or endorsed by Obsidian or Dynalist Inc. "Obsidian" is a
trademark of its owners.

Built on:

- [Qt 6](https://www.qt.io) (LGPL-3.0), linked dynamically
- [md4c](https://github.com/mity/md4c) by Martin Mitáš (MIT), the Markdown parser
- [KSyntaxHighlighting](https://invent.kde.org/frameworks/syntax-highlighting) from KDE
  Frameworks (MIT), colours code blocks

## License

MIT, see [LICENSE](LICENSE).
