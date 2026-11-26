#!/bin/sh
# Regenerates cmake/sevenzip-sources.cmake from the 7zz bundle's makefile (run after an upstream 7-Zip update).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root/CPP/7zip/Bundles/Alone2"
{
  echo "# 7-Zip backend sources (the 7zz bundle in CPP/7zip/Bundles/Alone2 without its console front-end)."
  echo "# Regenerate: see cmake/gen-sources.sh"
  echo "set(SEVENZIP_BACKEND_SOURCES"
  make -n -f makefile.gcc O=/nonexistent 2>/dev/null | grep -oE '[^ ]+\.(c|cpp)\b' | sort -u | grep -v 'UI/Console/' |
    sed 's#^\.\./\.\./\.\./\.\./#  #; s#^\.\./\.\./#  CPP/7zip/#; s#^\.\./\.\./\.\./#  CPP/#'
  echo ")"
} > "$root/cmake/sevenzip-sources.cmake"
