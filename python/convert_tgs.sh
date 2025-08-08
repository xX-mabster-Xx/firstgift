#!/usr/bin/env bash
set -euo pipefail

indir="${1:-.}"           # папка с .tgs (по умолчанию текущая)
outdir="${2:-converted}"  # куда класть результат

mkdir -p "$outdir"
shopt -s nullglob
found=0

for f in "$indir"/*.tgs; do
  found=1
  base="$(basename "${f%.tgs}")"
  # В GIF (можно поменять на .mp4 ниже)
  lottie_convert.py "$f" "$outdir/$base.gif" --fps 30
  echo "[OK] $f -> $outdir/$base.gif"
done

if [[ $found -eq 0 ]]; then
  echo "В папке '$indir' нет .tgs файлов."
fi
