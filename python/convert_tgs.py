#!/usr/bin/env python3
import argparse, pathlib
from rlottie_python import LottieAnimation

def iter_tgs(paths):
    for p in paths:
        p = pathlib.Path(p)
        if p.is_dir():
            yield from sorted(p.glob("*.tgs"))
        elif p.suffix.lower() == ".tgs":
            yield p

def convert_one(src: pathlib.Path, outdir: pathlib.Path, fmt: str) -> pathlib.Path:
    outdir.mkdir(parents=True, exist_ok=True)
    out = outdir / f"{src.stem}.{fmt}"
    with LottieAnimation.from_tgs(str(src)) as anim:
        if fmt in ("gif", "webp", "apng"):
            anim.save_animation(str(out))            # рендерит всю анимацию
        elif fmt == "png":
            anim.render_pillow_frame(0).save(out)    # первый кадр
        else:
            raise ValueError(f"Unsupported fmt: {fmt}")
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help=".tgs файлы или папки")
    ap.add_argument("--to", choices=["gif", "webp", "apng", "png"], default="gif")
    ap.add_argument("--outdir", default="converted")
    args = ap.parse_args()

    outdir = pathlib.Path(args.outdir)
    files = list(iter_tgs(args.inputs))
    if not files:
        raise SystemExit("Нет .tgs для обработки")

    for f in files:
        try:
            dst = convert_one(f, outdir, args.to)
            print(f"[OK] {f} -> {dst}")
        except Exception as e:
            print(f"[FAIL] {f}: {e}")

if __name__ == "__main__":
    main()
