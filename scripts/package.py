#!/usr/bin/env python3
"""Pack build/arm/audio-stream.so + module.json into dist/audio-stream-module.tar.gz.

Modes are set explicitly (dirs 0755, .so 0755, module.json 0644, owner root)
so the archive extracts correctly on the Move no matter which OS built it.
(GNU tar on Windows produced read-only directories that Schwung could not extract.)
"""
import io, os, sys, tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MID = "audio-stream"
FILES = [(os.path.join(ROOT, "module.json"), "module.json", 0o644),
         (os.path.join(ROOT, "build", "arm", MID + ".so"), MID + ".so", 0o755)]


def main():
    out = os.path.join(ROOT, "dist", MID + "-module.tar.gz")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    for src, _, _ in FILES:
        if not os.path.exists(src):
            sys.exit("missing %s (run the build first)" % src)

    def norm(ti, mode):
        ti.mode, ti.uid, ti.gid, ti.uname, ti.gname, ti.mtime = mode, 0, 0, "root", "root", 0
        return ti

    with tarfile.open(out, "w:gz", format=tarfile.GNU_FORMAT) as tf:
        d = tarfile.TarInfo(MID); d.type = tarfile.DIRTYPE; tf.addfile(norm(d, 0o755))
        for src, name, mode in FILES:
            data = open(src, "rb").read()
            ti = tarfile.TarInfo(MID + "/" + name); ti.size = len(data)
            tf.addfile(norm(ti, mode), io.BytesIO(data))
    print("wrote", out)


if __name__ == "__main__":
    main()
