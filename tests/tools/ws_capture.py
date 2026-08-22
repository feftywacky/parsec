#!/usr/bin/env python3
"""Measure Hyperliquid WebSocket feed cadence from this machine's vantage point.

Dependency-free raw RFC 6455 client -- no websockets/websocket-client package needed.
Records the numbers in docs/09-measurements.md; re-run when the network vantage changes.

    python3 tests/tools/ws_capture.py BTC 180

Separates the two l2Book variants by level count, since both arrive on the same channel name.
"""
import socket, ssl, os, base64, struct, json, time, sys, collections

HOST="api.hyperliquid-testnet.xyz"; PATH="/ws"
key=base64.b64encode(os.urandom(16)).decode()
raw=socket.create_connection((HOST,443),timeout=15)
s=ssl.create_default_context().wrap_socket(raw,server_hostname=HOST)
s.send(("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"%(PATH,HOST,key)).encode())
buf=b""
while b"\r\n\r\n" not in buf: buf+=s.recv(4096)
hdr,rest=buf.split(b"\r\n\r\n",1)
assert b"101" in hdr.split(b"\r\n")[0], hdr[:200]

def send_text(t):
    p=t.encode(); m=os.urandom(4)
    n=len(p)
    if n<126: h=struct.pack("!BB",0x81,0x80|n)
    elif n<65536: h=struct.pack("!BBH",0x81,0x80|126,n)
    else: h=struct.pack("!BBQ",0x81,0x80|127,n)
    s.send(h+m+bytes(b^m[i%4] for i,b in enumerate(p)))

pending=bytearray(rest)
def recv_exact(n):
    while len(pending)<n:
        d=s.recv(65536)
        if not d: raise EOFError
        pending.extend(d)
    out=bytes(pending[:n]); del pending[:n]; return out

def read_frame():
    b1,b2=recv_exact(2); op=b1&0xF; n=b2&0x7F
    if n==126: n=struct.unpack("!H",recv_exact(2))[0]
    elif n==127: n=struct.unpack("!Q",recv_exact(8))[0]
    return op, recv_exact(n)

coin=sys.argv[1] if len(sys.argv)>1 else "BTC"
dur=float(sys.argv[2]) if len(sys.argv)>2 else 60.0
for sub in [{"type":"bbo","coin":coin},
            {"type":"l2Book","coin":coin,"fast":True},
            {"type":"trades","coin":coin},
            {"type":"activeAssetCtx","coin":coin}]:
    send_text(json.dumps({"method":"subscribe","subscription":sub}))

stats=collections.defaultdict(lambda:{"n":0,"bytes":0,"last":None,"gaps":[],"dupes":0,"prev":None})
nonjson=[]
t0=time.time(); s.settimeout(5)
while time.time()-t0<dur:
    try: op,payload=read_frame()
    except (socket.timeout, ssl.SSLWantReadError): continue
    except EOFError: break
    if op==8: break
    if op!=1: continue
    now=time.time()
    txt=payload.decode("utf8","replace")
    try: msg=json.loads(txt)
    except Exception:
        nonjson.append(txt[:80]); continue
    ch=msg.get("channel","?")
    if ch=="l2Book":
        try: ch="l2Book/%dlv"%len(msg["data"]["levels"][0])
        except Exception: pass
    st=stats[ch]; st["n"]+=1; st["bytes"]+=len(payload)
    if st["last"] is not None: st["gaps"].append(now-st["last"])
    st["last"]=now
    if st["prev"]==txt: st["dupes"]+=1
    st["prev"]=txt

el=time.time()-t0
print(f"\n=== {coin} testnet, {el:.1f}s capture ===")
print(f"{'channel':16}{'msgs':>7}{'rate/s':>9}{'bytes/msg':>11}{'median gap':>12}{'p95 gap':>10}{'max gap':>10}{'dupes':>7}")
for ch,st in sorted(stats.items()):
    g=sorted(st["gaps"])
    med=g[len(g)//2] if g else 0; p95=g[int(len(g)*0.95)] if g else 0; mx=g[-1] if g else 0
    print(f"{ch:16}{st['n']:>7}{st['n']/el:>9.2f}{st['bytes']//max(st['n'],1):>11}{med:>12.3f}{p95:>10.3f}{mx:>10.3f}{st['dupes']:>7}")
if nonjson: print("non-JSON frames:", nonjson[:3])

