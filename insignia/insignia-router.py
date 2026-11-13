#!/usr/bin/env python3
"""
insignia-router.py - EXPERIMENTAL host-side virtual router for putting the
Xbox-app emulator's original-Xbox network onto Insignia.

Status: milestone 1 of 3. This is a research tool, not a finished product. It
does NOT by itself connect TimeSplitters to Xbox Live - two other pieces are
still needed (a registered console identity injected into the emulator, and a
saved Live account for the game to select). See insignia/NOTES.md.

What it does now
----------------
tsfp-syslink's bridge already tunnels every raw Ethernet frame the emulated
Xbox sends as one UDP datagram to  host:port  (the same format XLink Kai and
xemu's UDP Tunnel use), and injects any datagram it receives back as a frame.

Point that tunnel at this script instead of XLink Kai, and it plays the part of
the LAN the console is plugged into:

  * answers ARP for the gateway,
  * runs a tiny DHCP server so the console gets a real IP (instead of the
    169.254.x.x "no DHCP" fallback),
  * runs a DNS server that resolves the Xbox Live names
    (MACS/AS/TGS.XBOXLIVE.COM and friends) to Insignia's address,
  * replies to ICMP echo (gateway reachability checks),
  * logs every other packet - in particular the TCP connections the console
    tries to open to Insignia - so we can see exactly how the emulated console
    behaves and what the next milestone (TCP NAT) has to carry.

What it does NOT do yet
-----------------------
  * TCP NAT: it logs the console's SYNs to Insignia but does not yet relay them
    to the real server. That is milestone 2 (a small userspace TCP relay).
  * Nothing here provisions an account or a console registration; that is the
    Insignia-side piece (milestone 3).

Usage
-----
  1. Close XLink Kai (this replaces it for Insignia testing).
  2. In tsfp-syslink.ini set:
         [relay]
         host=127.0.0.1
         port=40000          ; this script's LISTEN_PORT below
         local_port=9369     ; must match REPLY_PORT below
  3. python3 insignia-router.py
  4. Run tsfp-syslink-build4.exe and start the game.

Everything is printed; nothing is written to disk.
"""
import socket, struct, sys, time

# --- configuration ---------------------------------------------------------
LISTEN_HOST = "127.0.0.1"
LISTEN_PORT = 40000          # tsfp-syslink.ini [relay] port  -> frames arrive here
REPLY_HOST  = "127.0.0.1"
REPLY_PORT  = 9369           # tsfp-syslink.ini [relay] local_port -> frames go back here

GW_IP       = "10.13.37.1"   # the virtual gateway / DNS the console will use
CLIENT_IP   = "10.13.37.37"  # the address we lease to the console
NETMASK     = "255.255.255.0"
GW_MAC      = bytes([0x02, 0x00, 0x00, 0x13, 0x37, 0x01])  # locally-administered

INSIGNIA_IP = "46.101.64.175"  # where the Xbox Live names resolve to
LIVE_SUFFIXES = (b"xboxlive.com", b"xbox.com")

# --- helpers ---------------------------------------------------------------
def ipb(s):     return socket.inet_aton(s)
def ips(b):     return socket.inet_ntoa(b)
def mac(b):     return ":".join("%02x" % x for x in b)

def csum(data):
    if len(data) & 1:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return (~s) & 0xFFFF

def eth(dst, src, etype, payload):
    return dst + src + struct.pack("!H", etype) + payload

def ipv4(src, dst, proto, payload):
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload),
                      0, 0x4000, 64, proto, 0, ipb(src), ipb(dst))
    hdr = hdr[:10] + struct.pack("!H", csum(hdr)) + hdr[12:]
    return hdr + payload

def udp(src, dst, sport, dport, payload):
    length = 8 + len(payload)
    u = struct.pack("!HHHH", sport, dport, length, 0)
    pseudo = ipb(src) + ipb(dst) + struct.pack("!BBH", 0, 17, length)
    c = csum(pseudo + u + payload) or 0xFFFF
    u = struct.pack("!HHHH", sport, dport, length, c)
    return u + payload

# --- DHCP ------------------------------------------------------------------
def dhcp_reply(req, climac, msg_type):
    # req is the DHCP payload (BOOTP). Build OFFER(2)/ACK(5).
    xid = req[4:8]
    opts = b"\x63\x82\x53\x63"
    opts += bytes([53, 1, msg_type])
    opts += bytes([54, 4]) + ipb(GW_IP)          # server id
    opts += bytes([51, 4]) + struct.pack("!I", 86400)
    opts += bytes([1, 4]) + ipb(NETMASK)
    opts += bytes([3, 4]) + ipb(GW_IP)           # router
    opts += bytes([6, 4]) + ipb(GW_IP)           # DNS = us
    opts += bytes([255])
    bootp = struct.pack("!BBBB4sHH4s4s4s4s16s64s128s",
                        2, 1, 6, 0, xid, 0, 0x8000,
                        b"\0\0\0\0", ipb(CLIENT_IP), ipb(GW_IP), b"\0\0\0\0",
                        climac + b"\0" * 10, b"\0" * 64, b"\0" * 128)
    return bootp + opts

# --- DNS -------------------------------------------------------------------
def dns_name(data, off):
    parts = []
    while True:
        l = data[off]
        if l == 0:
            off += 1
            break
        parts.append(data[off + 1:off + 1 + l])
        off += 1 + l
    return b".".join(parts), off

def dns_reply(query):
    tid = query[:2]
    qd, _ = dns_name(query, 12)
    off = 12 + len(qd) + 2
    qtype, qclass = struct.unpack("!HH", query[off:off + 4])
    off += 4
    low = qd.lower()
    if any(low.endswith(s) for s in LIVE_SUFFIXES):
        ip = INSIGNIA_IP
    else:
        print("    [dns] passthrough/unknown name:", qd.decode("latin1"))
        ip = None
    header = tid + struct.pack("!HHHHH", 0x8180, 1, 1 if ip else 0, 0, 0)
    body = query[12:off]
    if ip:
        body += struct.pack("!HHHIH", 0xC00C, 1, 1, 60, 4) + ipb(ip)
    return header + body

# --- main loop -------------------------------------------------------------
def main():
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind((LISTEN_HOST, LISTEN_PORT))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print("insignia-router: listening on %s:%d, replying to %s:%d" %
          (LISTEN_HOST, LISTEN_PORT, REPLY_HOST, REPLY_PORT))
    print("  gateway %s  client %s  Xbox Live -> %s" % (GW_IP, CLIENT_IP, INSIGNIA_IP))
    seen = set()

    def send(frame):
        tx.sendto(frame, (REPLY_HOST, REPLY_PORT))

    while True:
        try:
            frame, _ = rx.recvfrom(2048)
        except KeyboardInterrupt:
            print("\nbye")
            return
        if len(frame) < 14:
            continue
        dst, src, etype = frame[0:6], frame[6:12], struct.unpack("!H", frame[12:14])[0]

        if etype == 0x0806:  # ARP
            (htype, ptype, hlen, plen, op) = struct.unpack("!HHBBH", frame[14:22])
            sha = frame[22:28]; spa = frame[28:32]; tpa = frame[38:42]
            if op == 1 and ips(tpa) == GW_IP:
                arp = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) + GW_MAC + ipb(GW_IP) + sha + spa
                send(eth(sha, GW_MAC, 0x0806, arp))
            continue

        if etype != 0x0800:
            continue
        ihl = (frame[14] & 0x0F) * 4
        proto = frame[23]
        sip, dip = ips(frame[26:30]), ips(frame[30:34])
        l4 = frame[14 + ihl:]

        if proto == 1:  # ICMP
            if l4 and l4[0] == 8:  # echo request
                rep = bytes([0, 0]) + l4[2:]
                rep = rep[:2] + struct.pack("!H", csum(rep)) + rep[4:]
                send(eth(src, GW_MAC, 0x0800, ipv4(GW_IP, sip, 1, rep)))
            continue

        if proto == 17:  # UDP
            sport, dport, ulen = struct.unpack("!HHH", l4[0:6])
            payload = l4[8:ulen]
            if dport == 67:  # DHCP
                climac = payload[28:34]
                mt = 0
                i = 240
                while i < len(payload) and payload[i] != 255:
                    if payload[i] == 53:
                        mt = payload[i + 2]
                    i += 2 + payload[i + 1] if payload[i] != 0 else 1
                if mt == 1:   # DISCOVER -> OFFER
                    print("[dhcp] discover from", mac(climac))
                    r = dhcp_reply(payload, climac, 2)
                    send(eth(climac, GW_MAC, 0x0800,
                             ipv4(GW_IP, "255.255.255.255", 17, udp(GW_IP, "255.255.255.255", 67, 68, r))))
                elif mt == 3: # REQUEST -> ACK
                    print("[dhcp] request -> ack, leased %s to %s" % (CLIENT_IP, mac(climac)))
                    r = dhcp_reply(payload, climac, 5)
                    send(eth(climac, GW_MAC, 0x0800,
                             ipv4(GW_IP, "255.255.255.255", 17, udp(GW_IP, "255.255.255.255", 67, 68, r))))
                continue
            if dport == 53:  # DNS
                qd, _ = dns_name(payload, 12)
                print("[dns] %s ? -> %s" % (qd.decode("latin1"),
                      INSIGNIA_IP if any(qd.lower().endswith(s) for s in LIVE_SUFFIXES) else "(nxdomain)"))
                rep = dns_reply(payload)
                send(eth(src, GW_MAC, 0x0800, ipv4(GW_IP, sip, 17, udp(GW_IP, sip, 53, sport, rep))))
                continue
            key = ("udp", dip, dport)
            if key not in seen:
                seen.add(key); print("[udp] %s:%d -> %s:%d (not relayed yet)" % (sip, sport, dip, dport))
            continue

        if proto == 6:  # TCP
            dport = struct.unpack("!H", l4[2:4])[0]
            flags = l4[13]
            if flags & 0x02:  # SYN
                key = ("tcp", dip, dport)
                if key not in seen:
                    seen.add(key)
                    tag = " <-- Insignia" if dip == INSIGNIA_IP else ""
                    print("[tcp] SYN %s -> %s:%d  (NAT not built yet, milestone 2)%s" %
                          (sip, dip, dport, tag))
            continue


if __name__ == "__main__":
    main()
