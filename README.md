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
- `[[` autocompletes note names and aliases, `[[#` the headings of a note and `[[^` its
  paragraphs (adding a `^id` to the one you pick); Enter continues lists; Tab and
  Shift+Tab indent
- Brackets, quotes and backticks close themselves; with text selected, `*`, `_`, `~`, `=`
  or a backtick wraps it, and `[` twice makes it a link. Pasting a URL over selected text
  turns it into a link, and dropping a file from the tree links it.
- Fold headings, lists and callouts with the arrow beside them (folds are remembered per
  note); nested lists get indentation guides
- Format, Paragraph (lists, headings, quote) and Insert (table, callout, rule, code
  block) commands in the menu bar, the right-click menu and the command palette
- Properties: the frontmatter shows as rows you edit in place, with a type per property
  (text, list, number, checkbox, date, date and time), tags and aliases as removable
  pills, and Ctrl+; to add one. Unchanged lines are written back as they were, and YAML
  the panel cannot edit safely stays read-only; right-click > Edit source opens the text.
- Callouts show as coloured boxes with their icon and title (`[!note]-` starts folded);
  images (`![[x.png|200]]`,
  `![](path)`) show in place. Moving the cursor into either reveals the Markdown.
  Right-click an image to copy, open, rename, move or delete it.
- Find and replace in the note
- Paste a screenshot and it is saved as `Pasted image <timestamp>.png`, in the attachment
  folder chosen in Settings
- Daily notes (Go > Open today's daily note, or the calendar button above the file tree)
  and templates (Insert > Insert template) with `{{title}}`, `{{date}}` and `{{time}}`
- A new note you leave without any text (from Ctrl+N, the quick switcher or a link to a note
  that did not exist yet) is removed instead of piling up as empty files

**Tables**
- Tables are edited as a grid, no pipes to type: click a cell and write
- Tab in the last cell or the + under the table adds a row; the + on the right adds a column
- Right-click a cell to add, move, duplicate or delete rows and columns, align or sort
  by a column, or switch that table to its Markdown source
- Only the rows you change are rewritten; a table padded into neat columns stays padded

**Reading**
- Reading view with tables, nested and task lists, callouts, code blocks with a copy
  button, properties, and embedded images and notes (`![[x.png]]`, `![[note#Heading]]`)
- Click a checkbox in the reading view to tick it in the file; click a foldable callout's
  title to open or close it. `%%comments%%` and `^block-ids` are not shown.

**Links and navigation**
- Renaming or moving a note or folder rewrites every link that points into it
- Backlinks, outgoing links, tags and outline panels; the status bar counts backlinks
- Hold Ctrl over a link to preview the note it points to
- Bookmarks for notes, folders and searches
- Command palette with every command and its shortcut
- Vault-wide search with `"exact phrase"`, `/regex/`, `-exclude`, `OR`, `file:`, `path:`,
  `tag:`, `line:( )`, `section:( )`, `task:`, `task-todo:` and `task-done:`, match case
  and sort order; and a quick switcher
- File tree toolbar: new note or folder, sort by name, modified or created time,
  auto-reveal the open note, collapse or expand all; open folders stay open the next time
  the vault is opened
- Tabs with their own back and forward history, restored on the next start; split the
  view to the right, and pin a tab so links from it open in a new tab
- Shift+click files in the tree to move or delete several at once
- Right-click a file, folder, tab or panel entry to copy its path or a link to it, make a
  copy, move it, search inside a folder, or open it in another app; the ⋮ button on a
  note has the same file commands
- `serat://open?vault=<name>&file=<path>` links open the note in Serat; clicking one while
  Serat runs reuses the open window
- Edits made outside the app (sync, git, another editor) show up immediately and are merged
  line by line with what you are typing; if both change the same text, yours stays and the
  other version is kept next to it as `<note> (conflict <date>).md`. A note deleted outside
  the app while it has unsaved changes is written back instead of closed.
- File recovery (File > File recovery, or Version history in a note's ⋮ menu): a copy of
  each note every few minutes and before it is deleted, kept for a week outside the vault.
  Restore an earlier version, or bring back a deleted note.

**Look and settings**
- Light and dark theme that follows the system, or set it under View > Theme
- Settings (Ctrl+,) for the editor helpers, readable line length, where new notes and
  attachments go, daily notes and templates

Not yet: graph view.

## Shortcuts

| Key | Action |
|---|---|
| Ctrl+P | Command palette |
| Ctrl+O | Quick switcher (Ctrl+Enter opens in a new tab) |
| Ctrl+N | New note |
| Ctrl+E | Switch between editing and reading |
| Ctrl+Shift+E | Live preview on / off |
| Ctrl+Shift+F | Search the vault (the selected text becomes the query) |
| Ctrl+F / Ctrl+H | Find / replace in note |
| Ctrl+B / Ctrl+I | Bold / italic (selection or the word at the cursor) |
| Ctrl+/ | Comment (`%%…%%`) |
| Ctrl+K | Add external link |
| Ctrl+L | Toggle checkbox |
| Ctrl+D | Delete paragraph |
| Ctrl+; | Add file property |
| Alt+Enter / Ctrl+Enter | Follow the link at the cursor / in a new tab |
| Ctrl+Shift+V | Paste as plain text |
| Ctrl+Shift+[ | Fold or unfold the current heading, list or callout |
| Ctrl+hover | Preview the linked note |
| Ctrl+, | Settings |
| F2 | Rename note |
| Ctrl+click | Follow a link (Ctrl+Shift+click: in a new tab) |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+T, Ctrl+W | New tab, close tab |
| Ctrl+Shift+T | Reopen the last closed tab |
| Ctrl+Tab, Ctrl+1…9 | Next tab, go to tab 1–8 or the last one |
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
Run the tests with `ctest --test-dir build`: `tst_vault` covers parsing, search and merging,
and `tst_ui` drives the real window offscreen (typing, menus, dialogs) without showing it.

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
Obsidian. Until you change them in Serat's own settings, Serat takes the attachment folder,
new note location and deletion prompt from `.obsidian/app.json`, the daily note and
template settings from `daily-notes.json` and `templates.json`, and the first bookmarks
from `bookmarks.json`; it reads these files and never writes inside `.obsidian/`. Serat
is an independent project, contains no Obsidian code, and is not affiliated with or
endorsed by Obsidian or Dynalist Inc. "Obsidian" is a trademark of its owners.

Built on:

- [Qt 6](https://www.qt.io) (LGPL-3.0), linked dynamically
- [md4c](https://github.com/mity/md4c) by Martin Mitáš (MIT), the Markdown parser
- [KSyntaxHighlighting](https://invent.kde.org/frameworks/syntax-highlighting) from KDE
  Frameworks (MIT), colours code blocks

## License

MIT, see [LICENSE](LICENSE).
