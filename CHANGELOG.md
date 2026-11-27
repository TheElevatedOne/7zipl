# Changelog

## 0.1.0

- First release: the 7-Zip File Manager rewritten with plain GTK 4 for Linux. It links against the library of
  the distribution's `7zip` package (`7z.so`) for every archive operation.
- Browse folders and archives (nested archives too), create archives with the Add to Archive dialog, extract,
  test, add, delete and rename inside archives, checksums, favorites, AES-256 encryption.
- 2 panels (F9), Large Icons / Small Icons / List / Details, Flat View, Auto Refresh, Folders History (Alt+F12),
  Select by Type, Unsorted.
- View (F3) and Edit (F4) with any program; files edited inside an archive are put back after saving.
- Split file, Combine files, Diff, Link (symbolic, relative symbolic and hard links), Open Inside * and #, the
  7-Zip benchmark, Delete Temporary Files.
- Tools → Options with 7-Zip's pages done the Linux way: default application for archive and ISO types
  (`mimeapps.list`), a per-user desktop entry, context menus for Dolphin, Nautilus, Nemo, PCManFM-Qt/PCManFM/Caja
  and Beluga, working folder, View/Edit/Diff programs, memory limit for unpacking.
- `--extract`, `--extract-here`, `--extract-to-folder`, `--compress`, `--compress-to`, `--test` and `--hash` for
  file-manager context menus; `--selftest`, `--screenshot` and `--profile` for testing.
