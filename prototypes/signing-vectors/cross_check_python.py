import time, statistics, json
from eth_account import Account
from hyperliquid.utils.signing import (
    construct_phantom_agent, l1_payload, sign_inner, action_hash, sign_l1_action,
    order_request_to_order_wire, order_wires_to_order_action,
)

PK  = "e908f86dbb4d55ac876378565aafeabc187f6690f046459397b17d9b9a19688e"
CID = bytes.fromhex("de6c4037798a4434ca03cd05f00e3b803126221375cd1e7eaaaf041768be06eb")
EXP = {
 True : "fa8a41f6a3fa728206df80801a83bcbfbab08649cd34d9c0bfba7c7b2f99340f53a00226604567b98a1492803190d65a201d6805e5831b7044f17fd530aec7841c",
 False: "1713c0fc661b792a50e8ffdd59b637b1ed172d9a3aa4d801d9d88646710fb74b33959f4d075a7ccbec9f2374a6da21ffa4448d58d0413a0d335775f680a881431c",
}
w = Account.from_key(bytes.fromhex(PK))
print("signer address:", w.address)

print("\n=== 1. SIGNATURE VECTOR: python SDK vs our rust prototype ===")
for is_mainnet in (True, False):
    pa  = construct_phantom_agent(CID, is_mainnet)
    sig = sign_inner(w, l1_payload(pa))
    got = f'{sig["r"][2:].rjust(64,"0")}{sig["s"][2:].rjust(64,"0")}{sig["v"]:02x}'
    print(f'mainnet={is_mainnet!s:<5} py ={got}')
    print(f'              rust={EXP[is_mainnet]}')
    print(f'              MATCH: {got == EXP[is_mainnet]}')

print("\n=== 2. ACTION_HASH cross-implementation fixture ===")
order = {"coin":"ETH","is_buy":True,"sz":0.02,"limit_px":1800.0,
         "reduce_only":False,"order_type":{"limit":{"tif":"Gtc"}},"cloid":None}
wire   = order_request_to_order_wire(order, 1)          # asset id 1
action = order_wires_to_order_action([wire])
NONCE  = 1690393044548
print("action  =", json.dumps(action, separators=(",",":")))
print("nonce   =", NONCE)
h = action_hash(action, None, NONCE, None)
print("hash    = 0x" + h.hex())
sig = sign_l1_action(w, action, None, NONCE, None, True)
print("sig     =", json.dumps(sig, separators=(",",":")))

print("\n=== 3. SIGNING LATENCY (python) ===")
for label, fn in [("action_hash+sign_l1_action", lambda: sign_l1_action(w, action, None, NONCE, None, True))]:
    fn()  # warm
    ts = []
    for _ in range(200):
        t0 = time.perf_counter(); fn(); ts.append((time.perf_counter()-t0)*1e6)
    ts.sort()
    print(f"{label}: median={statistics.median(ts):.0f}us  p99={ts[int(len(ts)*0.99)]:.0f}us  min={ts[0]:.0f}us")

print("\n=== 4. IMPORT COST ===")
t0=time.perf_counter()
import hyperliquid.exchange, hyperliquid.info  # noqa
print(f"import hyperliquid.exchange+info: {(time.perf_counter()-t0)*1e3:.0f} ms")
