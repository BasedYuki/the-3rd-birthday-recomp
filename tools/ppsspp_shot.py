"""Save PPSSPP's current display as a PNG (pauses the CPU briefly).

    python tools/ppsspp_shot.py <out.png> [port]
"""

import base64
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from ppsspp_ws import Dbg

out = sys.argv[1]
d = Dbg(int(sys.argv[2]) if len(sys.argv) > 2 else 45679)
d.request("cpu.stepping", expect="cpu.stepping")
try:
    r = d.request("gpu.buffer.screenshot", type="uri", timeout=20)
    uri = r.get("uri", "")
    if not uri.startswith("data:image/png;base64,"):
        raise SystemExit(f"unexpected reply: { {k: str(v)[:60] for k, v in r.items()} }")
    open(out, "wb").write(base64.b64decode(uri.split(",", 1)[1]))
    print(f"saved {out} ({r.get('width')}x{r.get('height')})")
finally:
    d.request("cpu.resume", expect="cpu.resume")
