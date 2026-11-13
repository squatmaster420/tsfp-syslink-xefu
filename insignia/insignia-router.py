#!/usr/bin/env python3
"""
insignia-router.py - EXPERIMENTAL host-side virtual router for putting the
Xbox-app emulator's original-Xbox network onto Insignia.

Status: research tool, not a finished product. It provides the *network path*
only (piece C of the plan in NOTES.md). It does NOT by itself connect
TimeSplitters to Xbox Live - a registered console identity must still be
injected into the emulator (piece A) and a saved Live account presented to the
game (piece B).

tsfp-syslink's bridge tunnels every raw Ethernet frame the emulated Xbox sends
as one UDP datagram to host:port (the XLink-Kai / xemu UDP-tunnel format), and
injects any datagram it receives back as a frame. Point that tunnel at this
script and it plays the LAN the console is plugged into:

  * ARP for the gateway,
  * DHCP  (console gets a real IP, with us as gateway + DNS),
  * DNS   (*.xboxlive.com / *.xbox.com -> Insignia),
  * ICMP  echo,
  * TCP   NAT: relays the console's TCP connections (e.g. to Insignia's
          MACS/AS/TGS and game servers) to the real hosts via host sockets.
  * other UDP/TCP is logged.

The TCP NAT is a small userspace stack: it answers the console's handshake,
opens a real host socket to the destination, and shuttles bytes both ways. It
is deliberately simple (no window scaling, minimal retransmit) - enough for the
request/response Live auth traffic, to be hardened once end-to-end works.

Usage:
  1. Close XLink Kai (this replaces it for Insignia testing).
  2. tsfp-syslink.ini:  [relay] host=127.0.0.1  port=40000  local_port=9369
  3. python3 insignia-router.py
  4. Run tsfp-syslink-build4.exe and start the game.
"""
import socket, struct, sys, threading, select, time

# --- configuration ---------------------------------------------------------
LISTEN_HOST = "127.0.0.1"
LISTEN_PORT = 40000
REPLY_HOST  = "127.0.0.1"
REPLY_PORT  = 9369

GW_IP       = "10.13.37.1"
CLIENT_IP   = "10.13.37.37"
NETMASK     = "255.255.255.0"
GW_MAC      = bytes([0x02, 0x00, 0x00, 0x13, 0x37, 0x01])

INSIGNIA_IP = "46.101.64.175"
LIVE_SUFFIXES = (b"xboxlive.com", b"xbox.com")

MSS = 1400
WIN = 64240

# --- low-level helpers -----------------------------------------------------
def ipb(s):  return socket.inet_aton(s)
def ips(b):  return socket.inet_ntoa(b)
def macs(b): return ":".join("%02x" % x for x in b)

def csum(data):
    if len(data) & 1: data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data)//2), data))
    s = (s >> 16) + (s & 0xFFFF); s += s >> 16
    return (~s) & 0xFFFF

def eth(dst, src, etype, payload):
    return dst + src + struct.pack("!H", etype) + payload

def ipv4(src, dst, proto, payload, ident=0):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20+len(payload), ident, 0x4000, 64, proto, 0, ipb(src), ipb(dst))
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + payload

def udp(src, dst, sport, dport, payload):
    length = 8 + len(payload)
    pseudo = ipb(src) + ipb(dst) + struct.pack("!BBH", 0, 17, length)
    u = struct.pack("!HHHH", sport, dport, length, 0)
    c = csum(pseudo + u + payload) or 0xFFFF
    return struct.pack("!HHHH", sport, dport, length, c) + payload

def tcp(src, dst, sport, dport, seq, ack, flags, payload=b"", win=WIN):
    off = (5 << 4)
    hdr = struct.pack("!HHIIBBHHH", sport, dport, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF,
                      off, flags, win, 0, 0)
    pseudo = ipb(src) + ipb(dst) + struct.pack("!BBH", 0, 6, len(hdr)+len(payload))
    c = csum(pseudo + hdr + payload)
    hdr = hdr[:16] + struct.pack("!H", c) + hdr[18:]
    return hdr + payload

# --- DHCP / DNS / ICMP (piece C, validated) --------------------------------
def dhcp_reply(req, climac, msg_type):
    xid = req[4:8]
    opts = b"\x63\x82\x53\x63" + bytes([53,1,msg_type])
    opts += bytes([54,4]) + ipb(GW_IP)
    opts += bytes([51,4]) + struct.pack("!I", 86400)
    opts += bytes([1,4]) + ipb(NETMASK)
    opts += bytes([3,4]) + ipb(GW_IP)
    opts += bytes([6,4]) + ipb(GW_IP)
    opts += bytes([255])
    bootp = struct.pack("!BBBB4sHH4s4s4s4s16s64s128s", 2,1,6,0, xid, 0, 0x8000,
                        b"\0"*4, ipb(CLIENT_IP), ipb(GW_IP), b"\0"*4,
                        climac + b"\0"*10, b"\0"*64, b"\0"*128)
    return bootp + opts

def dns_name(data, off):
    parts = []
    while True:
        l = data[off]
        if l == 0: off += 1; break
        parts.append(data[off+1:off+1+l]); off += 1+l
    return b".".join(parts), off

def dns_reply(query):
    tid = query[:2]
    qd, off = dns_name(query, 12); off += 4
    ip = INSIGNIA_IP if any(qd.lower().endswith(s) for s in LIVE_SUFFIXES) else None
    header = tid + struct.pack("!HHHHH", 0x8180, 1, 1 if ip else 0, 0, 0)
    body = query[12:off]
    if ip:
        body += struct.pack("!HHHIH", 0xC00C, 1, 1, 60, 4) + ipb(ip)
    return header + body

# --- TCP NAT ---------------------------------------------------------------
class Conn:
    __slots__ = ("sock","our_seq","their_seq","climac","gip","gport","dip","dport","established","closing")
    def __init__(self):
        self.sock = None; self.our_seq = 0; self.their_seq = 0
        self.climac = None; self.gip = None; self.gport = 0
        self.dip = None; self.dport = 0; self.established = False; self.closing = False

class Router:
    def __init__(self):
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind((LISTEN_HOST, LISTEN_PORT))
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.txlock = threading.Lock()
        self.conns = {}            # (gip,gport,dip,dport) -> Conn
        self.seen = set()
        threading.Thread(target=self.pump, daemon=True).start()

    def send(self, frame):
        with self.txlock:
            self.tx.sendto(frame, (REPLY_HOST, REPLY_PORT))

    def to_guest_tcp(self, c, flags, payload=b""):
        seg = tcp(c.dip, c.gip, c.dport, c.gport, c.our_seq, c.their_seq, flags, payload)
        self.send(eth(c.climac, GW_MAC, 0x0800, ipv4(c.dip, c.gip, 6, seg)))

    # background: real host sockets -> guest
    def pump(self):
        while True:
            socks = [c.sock for c in list(self.conns.values()) if c.sock and c.established and not c.closing]
            if not socks:
                time.sleep(0.02); continue
            try:
                r,_,_ = select.select(socks, [], [], 0.2)
            except (ValueError, OSError):
                continue
            for s in r:
                c = next((c for c in list(self.conns.values()) if c.sock is s), None)
                if not c: continue
                try:
                    data = s.recv(MSS)
                except OSError:
                    data = b""
                if data:
                    self.to_guest_tcp(c, 0x18, data)          # PSH,ACK
                    c.our_seq = (c.our_seq + len(data)) & 0xFFFFFFFF
                else:
                    self.to_guest_tcp(c, 0x11)                # FIN,ACK
                    c.our_seq = (c.our_seq + 1) & 0xFFFFFFFF
                    c.closing = True
                    try: s.close()
                    except OSError: pass

    def open_conn(self, c):
        try:
            c.sock = socket.create_connection((c.dip, c.dport), timeout=5)
            c.sock.setblocking(True)
            c.our_seq = (int(time.time()*1000) ^ (c.dport<<8)) & 0xFFFFFFFF
            self.to_guest_tcp(c, 0x12)                        # SYN,ACK
            c.our_seq = (c.our_seq + 1) & 0xFFFFFFFF
            c.established = True
            print("[tcp] connected %s:%d -> %s:%d%s" % (c.gip, c.gport, c.dip, c.dport,
                  " <-- Insignia" if c.dip == INSIGNIA_IP else ""))
        except OSError as ex:
            print("[tcp] connect FAILED %s:%d (%s) - RST to guest" % (c.dip, c.dport, ex))
            c.our_seq = 0
            self.to_guest_tcp(c, 0x14)                        # RST,ACK
            self.conns.pop((c.gip,c.gport,c.dip,c.dport), None)

    def handle_tcp(self, climac, gip, dip, l4):
        sport, dport, seq, ack = struct.unpack("!HHII", l4[0:12])
        offw = l4[12]; dataoff = (offw >> 4) * 4
        flags = l4[13]
        payload = l4[dataoff:]
        key = (gip, sport, dip, dport)
        c = self.conns.get(key)

        if flags & 0x02 and not (flags & 0x10):   # SYN
            c = Conn(); c.climac=climac; c.gip=gip; c.gport=sport; c.dip=dip; c.dport=dport
            c.their_seq = (seq + 1) & 0xFFFFFFFF
            self.conns[key] = c
            threading.Thread(target=self.open_conn, args=(c,), daemon=True).start()
            return
        if not c:
            return
        if flags & 0x04:                           # RST
            if c.sock:
                try: c.sock.close()
                except OSError: pass
            self.conns.pop(key, None); return
        if payload:
            # in-order data only (simple stack); re-ACK duplicates
            if seq == c.their_seq:
                try:
                    if c.sock: c.sock.sendall(payload)
                except OSError: pass
                c.their_seq = (c.their_seq + len(payload)) & 0xFFFFFFFF
            self.to_guest_tcp(c, 0x10)             # ACK
        if flags & 0x01:                           # FIN
            c.their_seq = (c.their_seq + 1) & 0xFFFFFFFF
            self.to_guest_tcp(c, 0x10)             # ACK the FIN
            if c.sock:
                try: c.sock.shutdown(socket.SHUT_WR)
                except OSError: pass

    # --- main receive loop -------------------------------------------------
    def run(self):
        print("insignia-router: %s:%d <-> reply %s:%d" % (LISTEN_HOST, LISTEN_PORT, REPLY_HOST, REPLY_PORT))
        print("  gateway %s  client %s  Xbox Live -> %s  (TCP NAT on)" % (GW_IP, CLIENT_IP, INSIGNIA_IP))
        while True:
            try:
                frame,_ = self.rx.recvfrom(2048)
            except KeyboardInterrupt:
                print("\nbye"); return
            if len(frame) < 14: continue
            dst, src = frame[0:6], frame[6:12]
            etype = struct.unpack("!H", frame[12:14])[0]

            if etype == 0x0806:
                op = struct.unpack("!H", frame[20:22])[0]
                sha, spa, tpa = frame[22:28], frame[28:32], frame[38:42]
                if op == 1 and ips(tpa) == GW_IP:
                    arp = struct.pack("!HHBBH",1,0x0800,6,4,2)+GW_MAC+ipb(GW_IP)+sha+spa
                    self.send(eth(sha, GW_MAC, 0x0806, arp))
                continue
            if etype != 0x0800: continue

            ihl = (frame[14] & 0x0F) * 4
            proto = frame[23]
            sip, dip = ips(frame[26:30]), ips(frame[30:34])
            l4 = frame[14+ihl:]

            if proto == 1:
                if l4 and l4[0] == 8:
                    rep = bytes([0,0]) + l4[2:]
                    rep = rep[:2] + struct.pack("!H", csum(rep)) + rep[4:]
                    self.send(eth(src, GW_MAC, 0x0800, ipv4(GW_IP, sip, 1, rep)))
                continue

            if proto == 17:
                sport, dport, ulen = struct.unpack("!HHH", l4[0:6])
                payload = l4[8:ulen]
                if dport == 67:
                    climac = payload[28:34]; mt = 0; i = 240
                    while i < len(payload) and payload[i] != 255:
                        if payload[i] == 53: mt = payload[i+2]
                        i += 2 + payload[i+1] if payload[i] != 0 else 1
                    if mt in (1, 3):
                        kind = 2 if mt == 1 else 5
                        print("[dhcp] %s -> %s %s" % ("discover" if mt==1 else "request",
                              "offer" if mt==1 else "ack", CLIENT_IP))
                        r = dhcp_reply(payload, climac, kind)
                        self.send(eth(climac, GW_MAC, 0x0800,
                                  ipv4(GW_IP,"255.255.255.255",17, udp(GW_IP,"255.255.255.255",67,68,r))))
                    continue
                if dport == 53:
                    qd,_ = dns_name(payload, 12)
                    tgt = INSIGNIA_IP if any(qd.lower().endswith(s) for s in LIVE_SUFFIXES) else "(nxdomain)"
                    print("[dns] %s -> %s" % (qd.decode("latin1"), tgt))
                    self.send(eth(src, GW_MAC, 0x0800, ipv4(GW_IP, sip, 17, udp(GW_IP, sip, 53, sport, dns_reply(payload)))))
                    continue
                k = ("udp", dip, dport)
                if k not in self.seen:
                    self.seen.add(k); print("[udp] %s:%d -> %s:%d (not relayed)" % (sip,sport,dip,dport))
                continue

            if proto == 6:
                self.handle_tcp(src, sip, dip, l4)
                continue


if __name__ == "__main__":
    Router().run()
