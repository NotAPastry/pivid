#!/bin/sh
# embed_asset.sh <html> <css> <symbol> > out.h
# Emits a C header: static const char <symbol>[] = "...";
# Replaces the literal token __PICO_CSS__ in <html> with the contents of <css>.
set -eu
HTML="$1"; CSS="$2"; SYM="$3"

# Build the combined asset (CSS spliced in) on stdout via awk, then C-escape it.
awk -v cssfile="$CSS" '
  BEGIN { while ((getline line < cssfile) > 0) css = css line "\n"; }
  { gsub(/__PICO_CSS__/, css); print }
' "$HTML" | awk '
  BEGIN { printf "/* GENERATED FILE -- DO NOT EDIT. Build artifact (not in git).\n";
          printf "   Edit the dashboard source: apps/pivid-config-agent/web/index.html\n";
          printf "   (styles: web/pico.classless.min.css). `make agent` regenerates this. */\n";
          printf "static const char SYMBOL[] =\n"; }
  {
    s = $0;
    gsub(/\\/, "\\\\", s);
    gsub(/"/, "\\\"", s);
    printf "\"%s\\n\"\n", s;
  }
  END { printf ";\n"; }
' | sed "s/SYMBOL/$SYM/"
