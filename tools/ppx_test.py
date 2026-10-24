"""PPSSPP texture packs: the textures PPSSPP saved for a scene ("Save new textures", in
memstick/PSP/TEXTURES/ULUS10567/new) are given to the port as a pack (textures/ in the test's own
folder); the port loads the same scene and logs which textures it found in the pack.

    python tools/ppx_test.py [scene]
"""

import os
import re
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios

scene = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 25
out = os.path.join(drive.SCRATCH, "ppx")
shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
src = os.path.join(drive.ROOT, "memstick", "PSP", "TEXTURES", "ULUS10567", "new")
shutil.copytree(src, os.path.join(out, "textures"))
ini = os.path.join(os.path.dirname(src), "textures.ini")
if os.path.exists(ini): shutil.copy(ini, os.path.join(out, "textures", "textures.ini"))   # the pack's options
if "tint" in sys.argv:   # make the pack's images visibly red, to see them replace the game's
    import subprocess
    ps = ("Add-Type -AssemblyName System.Drawing; Get-ChildItem '%s' -Filter *.png | ForEach-Object { "
          "$b = [Drawing.Bitmap]::FromFile($_.FullName); $c = New-Object Drawing.Bitmap $b; $b.Dispose(); "
          "for ($y = 0; $y -lt $c.Height; $y++) { for ($x = 0; $x -lt $c.Width; $x++) { $p = $c.GetPixel($x, $y); "
          "$c.SetPixel($x, $y, [Drawing.Color]::FromArgb($p.A, 255, [int]($p.G / 3), [int]($p.B / 3))) } }; "
          "$c.Save($_.FullName); $c.Dispose() }") % os.path.join(out, "textures")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True)
pack = {f[:-4] for f in os.listdir(src) if f.endswith(".png")}
tmp = os.path.join(drive.BUILD, "save_scene")
shutil.rmtree(tmp, ignore_errors=True); shutil.copytree(drive.SAVES["user"][0], tmp)
f = os.path.join(tmp, "ULUS10567DATA00", "3RDDATA.BIN")
d = bytearray(open(f, "rb").read()); struct.pack_into("<I", d, 0xB9D8, 0xFFFF0000 | scene); open(f, "wb").write(d)
g = drive.Port(out, tmp, extra_env={"SR_PPXLOG": "1"})
try:
    scenarios.to_title_menu(g)
    g.press("cross"); g.wait(4); g.press("cross"); g.wait(30)
    g.shot("scene")
finally:
    g.close()
log = open(os.path.join(out, "port.err"), errors="replace").read()
print([l for l in log.splitlines() if "PPSSPP pack" in l])
hit = set(re.findall(r"texture pack: ([0-9a-f]{24}) ->", log))
miss = set(re.findall(r"texture pack: no match ([0-9a-f]{24})", log))
print(f"pack has {len(pack)} textures; port: {len(hit)} distinct keys matched, {len(miss)} distinct keys with no match")
print("no-match keys that share the pack's cache key (hash differs):",
      sum(1 for k in miss if any(p[:16] == k[:16] for p in pack)))
print("no-match keys whose data hash is in the pack (cache key differs):",
      sum(1 for k in miss if any(p[16:] == k[16:] for p in pack)))
