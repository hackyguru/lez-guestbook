#!/usr/bin/env bash
# Read the guestbook straight from the LEZ testnet — no Basecamp, no module, no
# wallet. Anyone, anywhere can run this to check what the module shows.
#   ./verify.sh          print the signature count and the newest 10 entries
set -euo pipefail
SEQ="${SEQ:-https://testnet.lez.logos.co}"
PROGRAM="${PROGRAM:-Edc6RDHB6bjCUfseKfff7jkhEwWqDHuBieWpE2YiyHq}"
HEADER="${HEADER:-DkshwcR5xRf4bFvmugbBxt8P5rCzVvyUmJVHs9LPmEjp}"

python3 - "$SEQ" "$PROGRAM" "$HEADER" <<'PY'
import hashlib, json, sys, urllib.request
seq, program, header = sys.argv[1:4]
A = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
def b58d(s):
    n = 0
    for c in s: n = n * 58 + A.index(c)
    raw = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return b'\0' * (len(s) - len(s.lstrip('1'))) + raw
def b58e(b):
    n = int.from_bytes(b, 'big'); s = ''
    while n: n, r = divmod(n, 58); s = A[r] + s
    return '1' * (len(b) - len(b.lstrip(b'\0'))) + s
def rpc(method, params):
    req = urllib.request.Request(seq, json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode(),
                                 {'content-type': 'application/json'})
    return json.load(urllib.request.urlopen(req, timeout=15))['result']
def shard(acct):
    return bytes(rpc('getAccount', [acct])['data']['shards'].get(program, []))
# Entry accounts are public PDAs: sha256(prefix ‖ program ‖ seed), seed = "guestbook/entry/" ‖ index_le ‖ zeros
PREFIX = b"/LEE/v0.2/AccountId/PDA/" + b"\0" * 8
def entry_account(i):
    seed = b"guestbook/entry/" + i.to_bytes(8, 'little') + b"\0" * 8
    return b58e(hashlib.sha256(PREFIX + b58d(program) + seed).digest())
h = shard(header)
count = int.from_bytes(h, 'little') if h else 0
print(f"{count} signatures   (program {program})")
for i in range(count - 1, max(-1, count - 11), -1):
    d = shard(entry_account(i)); at = 32
    def s():
        global at
        n = int.from_bytes(d[at:at + 4], 'little'); v = d[at + 4:at + 4 + n].decode(); at += 4 + n; return v
    author = b58e(d[:32]); name = s() or "Anonymous"; text = s()
    print(f"  #{i + 1:<3} {name:<12} {text}   ({author[:4]}…{author[-4:]})")
PY
