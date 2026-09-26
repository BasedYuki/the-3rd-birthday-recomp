"""Press START/CROSS in PPSSPP until no SQEXTEC Movie thread is left (i.e. gameplay), then
print the thread list.

    python tools/oracle_skip_to_game.py [port]
"""

import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from ppsspp_ws import Dbg

d = Dbg(int(sys.argv[1]) if len(sys.argv) > 1 else 45679)


def movie_threads():
    return [t for t in d.request("hle.thread.list")["threads"] if "Movie" in t["name"]]


for i in range(40):
    m = movie_threads()
    print(f"step {i}: {len(m)} movie threads")
    if not m:
        break
    d.press("start" if i % 2 == 0 else "cross", 6)
    time.sleep(3)

for t in d.request("hle.thread.list")["threads"]:
    print(f"  {t['id']:>4} {t['name'][:32]:32} status={t.get('status')}")
