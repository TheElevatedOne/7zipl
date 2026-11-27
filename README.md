# 7zipl

![Static Badge](https://img.shields.io/badge/CLAUDE-VIBECODED?style=for-the-badge&logo=claude&label=VIBECODED&labelColor=%23333333&color=%23de7356) [![AUR Version](https://img.shields.io/aur/version/7zipl-bin?style=for-the-badge&logo=archlinux)](https://aur.archlinux.org/packages/7zipl-bin) [![AUR Version](https://img.shields.io/aur/version/7zipl-git?style=for-the-badge&logo=archlinux)](https://aur.archlinux.org/packages/7zipl-git)

<p align="center"><img src="data/icons/org.sevenzipl.SevenZipL.svg" alt="7zipl icon: a blue box with a zipper between a 7 and a z" width="128"></p>

The 7-Zip File Manager for Linux, rewritten with plain GTK 4 (no libadwaita). It is only the front-end: every
archive operation uses the library of the [7-Zip] installed from your distribution's `7zip` package (`7z.so`),
so you get its formats, compression settings and security updates, with the File Manager's window, dialogs and keyboard
shortcuts.

The Windows File Manager (`7zFM.exe`) and `7zG.exe` don't exist on Linux; 7zipl replaces both. This repository
contains no 7-Zip code.

## List of Contents

- **[Parts](#parts)**
- **[Features](#features)**
- **[Requirements](#requirements)**
- **[Installation](#installation)**
  - [Arch Linux (AUR)](#arch-linux-aur)
  - [Release tarball](#release-tarball)
  - [Building](#building)
- **[Desktop integration](#desktop-integration)**
  - [Default application for archives and ISOs](#default-application-for-archives-and-isos)
  - [Context menus of file managers](#context-menus-of-file-managers)
- **[Quick start](#quick-start)**
  - [Command line](#command-line)
  - [Keyboard shortcuts](#keyboard-shortcuts)
- **[Coming from 7-Zip on Windows](#coming-from-7-zip-on-windows)**
- **[Settings](#settings)**
- **[Testing](#testing)**
- **[Known limitations](#known-limitations)**
- **[License](#license)**

## Parts

| Part | Binary / file | Use it for |
|---|---|---|
| File manager | `7zipl` (window class `7zipl`) | browsing folders and archives, all archive operations |
| File-manager actions | `7zipl --extract`, `--extract-here`, `--compress`, … | "Extract here" / "Add to archive" entries in other file managers (what `7zG` does on Windows) |
| Engine | `7z.so` from the `7zip` package (not part of 7zipl) | every archive operation; 7zipl links against it |
| Desktop entry | `org.sevenzipl.SevenZipL.desktop` | launchers and "Open With" for archive types |

## Features

- **Uses the system 7-Zip library**: 7zipl links against `7z.so` from the `7zip` package and calls its archive
  handlers, codecs and hashers directly, the way 7-Zip's own programs do. The library ships without a header, so
  7zipl has its own (`gtk/src/sevenzip.hpp`), written from 7-Zip's interface headers. Formats, hash
  methods and compression settings come from the library, and progress, Pause and Cancel work per byte.
  Passwords stay inside the process. Links that would point outside the output folder are refused when
  extracting, as in 7-Zip.
- **Archives as folders**: open an archive and browse it like a folder; archives inside archives (`.tar.gz` →
  `.tar` → files) open on <kbd>Enter</kbd>, one level per archive. The address bar takes paths inside archives
  (`~/a.7z/docs/img`).
- **Create**: 7z, zip, tar, wim, gzip, bzip2 and xz, with 7-Zip's Add to Archive dialog: level, method (LZMA2,
  LZMA, PPMd, BZip2, Deflate, Deflate64), dictionary, word size, solid block size, CPU threads, volumes,
  update mode (add/update/freshen/synchronize), path mode, `-snl`/`-snh`/`-ssw`/`-sdel`, and free-form
  parameters (`qs=on mtf=off`).
- **Encryption**: AES-256 for 7z (with encrypted file names) and zip (AES-256 or ZipCrypto). Encrypted archives
  ask for the password once and keep it while they are open; updating them keeps header encryption on.
- **Extract and test** every format 7-Zip reads: 7z, zip, rar/rar5, tar, gz, bz2, xz, zstd, lzma, iso, udf, cab,
  chm, dmg, hfs, apfs, wim, vhd(x), vmdk, qcow, squashfs, cramfs, ext, fat, ntfs, rpm, deb, cpio, arj, lzh, msi,
  nsis, and more.
- **Change archives in place**: add files (into the open folder), create folders, delete and rename items.
- **Checksums**: **File → CRC** with every hasher of the installed 7-Zip (CRC32, CRC64, XXH64, MD5, SHA1, SHA256,
  SHA384, SHA512, SHA3-224 to SHA3-512, BLAKE2sp, CKSUM; `*` for all), for files on disk and items inside archives.
- **The File Manager's tools**: 2 panels (<kbd>F9</kbd>) with Copy/Move between them, Large Icons / Small
  Icons / List / Details views, Flat View, View (<kbd>F3</kbd>) and Edit (<kbd>F4</kbd>) with any program —
  a file edited inside an archive is put back into it after you save it — Split and Combine files, Diff, Link
  (symbolic and hard links), Folders History, Auto Refresh, and the 7-Zip benchmark.
- **Desktop integration the Linux way**: 7zipl sets itself as the default application for the archive types you
  tick (`mimeapps.list`, like "Open With" does), installs its desktop entry for the current user if it isn't
  installed, and adds "Extract Here" / "Add to .7z" entries to the context menus of Dolphin, Nautilus, Nemo,
  PCManFM-Qt/PCManFM/Caja and Beluga.
- **Progress like 7-Zip**: elapsed and remaining time, speed, sizes, compression ratio, Pause and Cancel; errors
  and warnings end in a message list instead of disappearing.
- **Plain GTK 4**: system theme colours (pywal themes work), square corners, no client-side title bar.

## Requirements

- Linux with GTK 4.14 or newer and GLib 2.76 or newer
- 7-Zip 23 or newer from your distribution, which provides `7z.so`:

  | Distribution | Package | Library |
  |---|---|---|
  | Arch Linux | `7zip` | `/usr/lib/7zip/7z.so` |
  | Debian 13, Ubuntu 24.04 and newer | `7zip` | `/usr/lib/7zip/7z.so` |
  | Fedora | `7zip` | `/usr/libexec/7zip/7z.so` |

  Debian 12's `7zip` package has no `7z.so`, and the old `p7zip` (16.02) is not supported.
- To build: a C++17 compiler (GCC or Clang), CMake 3.16+, Ninja or Make, pkg-config, and the `7zip` package
  (CMake finds `7z.so` in the folders above; `-DSZL_7Z_LIBRARY=/path/to/7z.so` picks another one)

## Installation

### Arch Linux (AUR)

| Package | Description |
|---|---|
| [`7zipl-bin`](https://aur.archlinux.org/packages/7zipl-bin) | Prebuilt x86_64 binary from the GitHub release |
| [`7zipl-git`](https://aur.archlinux.org/packages/7zipl-git) | Built from the latest commit |

```sh
paru -S 7zipl-bin   # or 7zipl-git
```

Both pull in the `7zip` package for `7z.so`.

### Release tarball

Each [release](https://github.com/TheElevatedOne/7zipl/releases) has `7zipl-<version>-linux-x86_64.tar.gz`, built
on Arch Linux. It contains `bin/7zipl` and `share/` (desktop file, metainfo, icon). It needs GTK 4.14+ and the
`7zip` package's `/usr/lib/7zip/7z.so` (the binary looks for it there).

```sh
tar -xzf 7zipl-<version>-linux-x86_64.tar.gz
sudo cp -r 7zipl-<version>-linux-x86_64/{bin,share} /usr/local/
```

### Building

```sh
git clone https://github.com/TheElevatedOne/7zipl.git
cd 7zipl
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

## Desktop integration

### Default application for archives and ISOs

A system install (`sudo cmake --install build` or the AUR package) puts `org.sevenzipl.SevenZipL.desktop` in
`/usr/share/applications`. It lists every archive and disk-image type 7zipl opens (7z, zip, rar, tar and its
compressed forms, ISO, wim, cab, rpm, deb, dmg, squashfs, disk images, …), so 7zipl shows up in **Open With**
and can be the default.

**Tools → Options → System** is 7-Zip's "Associate 7-Zip with" page:

- It shows each type, its extensions and the program that opens it now.
- Tick types and press OK, and 7zipl becomes their default for the current user. This is the same
  `~/.config/mimeapps.list` setting that file managers' "Open With" dialogs change.
- Untick a type and it goes back to the program it had before.
- There is no "All users" column: system-wide defaults belong to the distribution.

Running 7zipl without installing it (a build folder, a portable copy)? **Install for the current user** on the same
page writes `~/.local/share/applications/org.sevenzipl.SevenZipL.desktop`, pointing to the running binary, and its
icon. Types that already open with another program keep it. **Remove the user entry** takes the entry and its
associations away again.

### Context menus of file managers

**Tools → Options → 7-Zip** is 7-Zip's "Integrate 7-Zip to shell context menu" page. Tick the file managers and the
items (Open archive, Extract files..., Extract Here, Extract to folder, Test archive, Add to archive..., Add to .7z,
Add to .zip, CRC SHA), with or without a "7-Zip" submenu:

| File manager | What 7zipl writes |
|---|---|
| Dolphin, Konqueror | KIO service menus `~/.local/share/kio/servicemenus/7zipl-*.desktop` |
| Files (Nautilus) | scripts in `~/.local/share/nautilus/scripts/` (the "7-Zip" folder is the submenu) |
| Nemo | `~/.local/share/nemo/actions/7zipl-*.nemo_action` |
| PCManFM-Qt, PCManFM, Caja | file-manager actions `~/.local/share/file-manager/actions/7zipl-*.desktop` |
| Beluga | custom actions in `~/.config/beluga/actions.conf` (your own actions stay) |

The entries run the [command-line modes](#command-line). Unticking removes only what 7zipl wrote. Restart an
open file manager to see the change. Thunar keeps all custom actions in one file and isn't changed: add the
commands in **Edit → Configure custom actions**.

## Quick start

```sh
7zipl                      # opens the last folder
7zipl ~/Downloads/a.7z     # opens an archive
7zipl ~/a.7z/docs          # opens a folder inside an archive
```

Select files and press **Add** to create an archive; select archives and press **Extract**. Inside an archive,
**Extract** extracts the selection (or the open folder) without the path of the folder you are in, like 7-Zip.

### Command line

These run without the main window, for file-manager context menus:

| Command | Does |
|---|---|
| `7zipl --extract FILE...` | Extract dialog for the archives |
| `7zipl --extract-here FILE...` | extracts into the archive's folder |
| `7zipl --extract-to-folder FILE...` | extracts each archive into `<name>/` next to it |
| `7zipl --compress FILE...` | Add to Archive dialog |
| `7zipl --compress-to 7z FILE...` | adds to `<name>.7z` with the saved settings (also `zip`, `tar`, `xz`, …) |
| `7zipl --test FILE...` | tests the archives |
| `7zipl --hash SHA256 FILE...` | checksums (`*` for every method) |
| `7zipl --version` | prints the 7zipl version and the version and path of the 7-Zip library it uses |
| `7zipl --profile DIR` | uses `DIR` for settings instead of `~/.config/7zipl` |

### Keyboard shortcuts

| Key | Action |
|---|---|
| <kbd>Enter</kbd> | open: folders and archives inside, other files with their application |
| <kbd>Ctrl</kbd>+<kbd>PgDn</kbd> / <kbd>Shift</kbd>+<kbd>Enter</kbd> | Open Inside / Open Outside |
| <kbd>Backspace</kbd> | up one level (out of an archive at its root) |
| <kbd>F2</kbd>, <kbd>F5</kbd>, <kbd>F6</kbd>, <kbd>F7</kbd> | Rename, Copy To, Move To, Create Folder |
| <kbd>Delete</kbd> / <kbd>Shift</kbd>+<kbd>Delete</kbd> | to the Trash (on disk) or from the archive / delete permanently |
| <kbd>Alt</kbd>+<kbd>Enter</kbd> | Properties (item and archive information) |
| <kbd>Ctrl</kbd>+<kbd>A</kbd>, numpad <kbd>+</kbd> <kbd>-</kbd> <kbd>*</kbd> | select all, select / deselect by mask, invert |
| <kbd>Ctrl</kbd>+<kbd>R</kbd>, <kbd>Ctrl</kbd>+<kbd>H</kbd>, <kbd>Ctrl</kbd>+<kbd>L</kbd> | refresh, hidden files, address bar |
| <kbd>Alt</kbd>+<kbd>Shift</kbd>+<kbd>0</kbd>–<kbd>9</kbd> / <kbd>Alt</kbd>+<kbd>0</kbd>–<kbd>9</kbd> | set / go to a favorite folder |
| <kbd>F3</kbd> / <kbd>F4</kbd> | View / Edit (from inside an archive too) |
| <kbd>F9</kbd>, <kbd>Tab</kbd> | 2 panels, switch panel (Copy/Move go to the other panel) |
| <kbd>Alt</kbd>+<kbd>F12</kbd>, <kbd>\\</kbd> | Folders History, root folder |
| <kbd>Ctrl</kbd>+<kbd>1</kbd>–<kbd>4</kbd> | Large Icons, Small Icons, List, Details |
| <kbd>Ctrl</kbd>+<kbd>F3</kbd>–<kbd>F7</kbd> | sort by name, type, date, size; unsorted |
| <kbd>Alt</kbd>+numpad <kbd>+</kbd> <kbd>-</kbd> | select / deselect by type |

Single-key shortcuts work in the file list only, so typing in the address bar never deletes or renames files.

## Coming from 7-Zip on Windows

The same: toolbar (Add, Extract, Test, Copy, Move, Delete, Info), the menus, the Add to Archive and Extract
dialogs and their option names, the progress window, "Confirm File Replace", favorites, the column set per archive
format, folders first, 2 panels, the benchmark.

Windows features that manage the system have a Linux equivalent:

| 7-Zip on Windows | 7zipl |
|---|---|
| Options → System: associate with 7-Zip (registry) | default application per MIME type in `mimeapps.list`, current user ([details](#default-application-for-archives-and-isos)) |
| Options → System: "All users" | the distribution's defaults; not changed by an app |
| Options → 7-Zip: Explorer context menu (shell extension DLL) | context-menu files for each file manager ([details](#context-menus-of-file-managers)) |
| Options → 7-Zip: Eliminate duplication of root folder | the same, for "Extract to folder" |
| Options → Folders: working folder | the same (`-w`); default "Current", because `/tmp` is usually in RAM |
| Options → Editor: View / Editor / Diff programs | the same; empty means the system's default application |
| Options → Settings: memory limit for unpacking | the same (only RAR 7 archives with huge dictionaries ask) |
| Delete to the Recycle Bin | the Trash (freedesktop.org); <kbd>Shift</kbd>+<kbd>Delete</kbd> deletes |
| Attributes (`RHSA`) | Unix permissions (`-rw-r--r--`) |
| File → Link: hard link, symbolic link, junction | hard link, symbolic link, relative symbolic link |
| Tools → Delete Temporary Files | the same, for `$XDG_RUNTIME_DIR/7zipl` |

Not there, because Linux has no such thing: alternate streams (NTFS ADS), the Zone.Identifier option, "show system
menu", large memory pages, full-row select and alternative selection modes. Not there yet: SFX archives, Benchmark 2,
language files.

## Settings

**Tools → Options** has 7-Zip's pages: System, 7-Zip, Folders, Editor and Settings. They're stored in
`~/.config/7zipl/7zipl.conf` (a key file), together with the view options, window size, last folders, favorites,
the Add to Archive and Extract defaults, and the folder history. `--profile DIR` or `SZL_PROFILE=DIR` uses another
folder.

Temporary copies (View/Edit inside an archive, nested archives) go to `$XDG_RUNTIME_DIR/7zipl/` and are removed when
7zipl exits. **Tools → Delete Temporary Files** cleans up after a 7zipl that crashed.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

runs `7zipl --selftest` (130 checks; it needs 7-Zip installed):

- The engine (through `7z.so`): create, list, extract (with a folder prefix removed), test, rename, delete, hash,
  encrypted headers, zip/tar/gz, symbolic links and the link safety check, Open Inside #, the benchmark, and the
  working folder.
- Split and Combine.
- Desktop integration: the user desktop entry, default applications, and every context-menu writer.
- The main window: browsing into archives, adding a file into an archive folder, Flat View, view modes, 2 panels,
  copying out of an archive, and Edit putting a changed file back.

It runs with its own temporary XDG folders, so your settings, `mimeapps.list` and context menus are never touched.
It needs a display and exits with 77 (skipped) without one. To run it without a screen:

```sh
gtk4-broadwayd :5 &
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 GDK_DEBUG=no-portals ./build/7zipl --selftest
```

## Known limitations

- Archives inside archives are read-only (changes would have to be packed back into the outer archive).
- Formats 7-Zip can only read (rar, iso, dmg, …) can't be changed; delete, rename and add are disabled for them.
- zstd archives can be extracted but not created (7-Zip has a zstd decoder only).

## License

7zipl is GNU LGPL 2.1 or later; see [COPYING](COPYING). 7-Zip itself comes from your distribution under its
own license. 7zipl isn't made or endorsed by the 7-Zip author.

[7-Zip]: https://7-zip.org
