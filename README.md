# Serat

A desktop app for Markdown notes. Your notes stay plain `.md` files in a folder (a
vault), so any other editor, sync tool or git can work with them too. Serat understands
`[[wikilinks]]`, embeds, callouts and task lists, and updates links when you rename or
move a note.

Built with Qt 6 and C++20. Runs on Linux, Windows and macOS.

## Features

**Writing**
- Live preview while editing: Markdown symbols stay hidden except where the cursor is
  (`#` on its line, `**` or `[[ ]]` only on the word it touches). Lists show bullets,
  tasks show checkboxes you can click, and a click on a link opens it.
  View > Live preview switches to plain source.
- `[[` autocompletes note names; Enter continues lists; Tab and Shift+Tab indent
- Right-click in a note to format the selection, insert a table, checkbox, note link or
  rule, open or copy a link, or search the vault for the selected text
- Paste a screenshot and it is saved into the vault as `Pasted image <timestamp>.png`
- A new note you leave without a name or any text is removed instead of piling up as
  `Untitled` files

**Tables**
- Tables are edited as a grid, no pipes to type: click a cell and write
- Tab in the last cell or the + under the table adds a row; the + on the right adds a column
- Right-click a cell to insert or delete rows and columns, align a column, or switch that
  table to its Markdown source
- Only the rows you change are rewritten; a table padded into neat columns stays padded

**Reading**
- Reading view with tables, nested and task lists, callouts, code blocks with a copy
  button, frontmatter, and embedded images and notes (`![[x.png]]`, `![[note#Heading]]`)
- Click a checkbox in the reading view to tick it in the file

**Links and navigation**
- Renaming or moving a note or folder rewrites every link that points into it
- Backlinks, outgoing links and outline panels
- Vault-wide search and a quick switcher
- Tabs with their own back and forward history, restored on the next start
- Right-click a file, tab or panel entry to copy its wikilink, vault path, full path or
  a link that opens it; files can also be duplicated or moved to another folder
- `serat://open?vault=<name>&file=<path>` links open the note in Serat; clicking one while
  Serat runs reuses the open window
- Edits made outside the app (sync, git, another editor) show up immediately

**Look**
- Light and dark theme that follows the system, or set it under View > Theme

Not yet: graph view, tags pane, properties editor.

## Shortcuts

| Key | Action |
|---|---|
| Ctrl+O | Quick switcher (Ctrl+Enter opens in a new tab) |
| Ctrl+N | New note |
| Ctrl+E | Switch between editing and reading |
| Ctrl+Shift+E | Live preview on / off |
| Ctrl+Shift+F | Search the vault |
| Ctrl+F | Find in note |
| Ctrl+B / Ctrl+I | Bold / italic (selection or the word at the cursor) |
| F2 | Rename note |
| Ctrl+click | Follow a link (Ctrl+Shift+click: in a new tab) |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+Tab, Ctrl+W | Next tab, close tab |
| Ctrl+\\, Ctrl+Shift+\\ | Show or hide the left / right sidebar |
| Ctrl+= / Ctrl+- / Ctrl+0 | Zoom |

In a table: Tab moves to the next cell, Enter edits a cell, Delete clears it, Esc goes
back to the text below the table.

## Install from source

### Linux

Arch:

```sh
sudo pacman -S --needed qt6-base qt6-svg md4c syntax-highlighting cmake ninja
```

Debian/Ubuntu:

```sh
sudo apt install qt6-base-dev libqt6svg6-dev libmd4c-dev libkf6syntaxhighlighting-dev cmake ninja-build
```

Then:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local
```

### Windows

Install Qt 6 (MSVC, with the Qt SVG module) using the online installer, then from a
"Qt 6 for Desktop" prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix dist
```

`dist` is a self-contained folder. md4c is downloaded and built automatically when it is
not installed.

### macOS

```sh
brew install qt md4c cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
cmake --install build --prefix dist   # produces Serat.app
```

KSyntaxHighlighting is optional everywhere; without it code blocks are not coloured.
Run the tests with `ctest --test-dir build`.

## Usage

```sh
serat ~/Notes
```

Without an argument Serat reopens the last vault, or shows a list of vaults to pick from.

## Credits

Serat follows how [Obsidian](https://obsidian.md) stores a vault: the `[[wikilink]]` and
`![[embed]]` syntax, callouts, the link resolution rules and the "Pasted image" file
names. A vault can move between the two apps without changes, and the copy menu also
offers an `obsidian://` link for a note. The welcome screen lists vaults registered in
Obsidian, and Serat reads `.obsidian/app.json` only to honour its "confirm file
deletion" setting; it never writes inside `.obsidian/`. Serat is an independent
project, contains no Obsidian code, and is not affiliated with or endorsed by Obsidian
or Dynalist Inc. "Obsidian" is a trademark of its owners.

Built on:

- [Qt 6](https://www.qt.io) (LGPL-3.0), linked dynamically
- [md4c](https://github.com/mity/md4c) by Martin Mitáš (MIT), the Markdown parser
- [KSyntaxHighlighting](https://invent.kde.org/frameworks/syntax-highlighting) from KDE
  Frameworks (MIT), colours code blocks

## License

MIT, see [LICENSE](LICENSE).
