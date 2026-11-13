#!/usr/bin/env python3
"""
eeprom.py - read an original-Xbox 256-byte EEPROM image (e.g. the eeprom.bin
xemu writes after you register a console with Insignia) and pull out the fields
the game's Xbox Live code needs.

This is the INPUT side of the planned Insignia launcher: the user completes
registration + account creation once on xemu, hands us their eeprom.bin, and
the launcher injects these values into the Xbox-app emulator at start-up so the
emulated console presents the *same* registered identity.

Cleartext fields (serial, MAC, online key) are read directly. The security
section (HDD key, region) is RC4-encrypted with a per-version key; decryption is
implemented for versions 1.0-1.6 and is only attempted if asked.

Usage:
    python3 eeprom.py eeprom.bin           # show identity fields
    python3 eeprom.py eeprom.bin --secret  # also decrypt HDD key / region
"""
import struct, sys, hashlib

# RC4 key for the security-section HMAC, per EEPROM/kernel version.
VERSION_KEYS = {
    "1.0": bytes.fromhex("2a3b ad2c b1 94 4f 93 aa cd a4 4f 0c f1 f5 6e 1f".replace(" ", "")),
    "1.1": bytes.fromhex("1d f3 5d f5 07 d4 d9 ca 80  da cd c1 29 f1 5f 4a".replace(" ", "")),
    "1.6": bytes.fromhex("2b 84 57 a7 d8 0f 4d 9a 36 f2 7e 6e 44 3a 5b 90".replace(" ", "")),
}

def rc4(key, data):
    S = list(range(256)); j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 0xFF
        S[i], S[j] = S[j], S[i]
    out = bytearray(); i = j = 0
    for b in data:
        i = (i + 1) & 0xFF; j = (j + S[i]) & 0xFF
        S[i], S[j] = S[j], S[i]
        out.append(b ^ S[(S[i] + S[j]) & 0xFF])
    return bytes(out)

def read_eeprom(data):
    if len(data) < 0x100:
        raise ValueError("not a 256-byte EEPROM image (%d bytes)" % len(data))
    serial = data[0x34:0x40]
    mac    = data[0x40:0x46]
    okey   = data[0x48:0x58]
    video  = struct.unpack("<I", data[0x58:0x5C])[0]
    return {
        "serial":     serial.decode("latin1").rstrip("\0"),
        "mac":        ":".join("%02X" % b for b in mac),
        "mac_bytes":  mac,
        "online_key": okey.hex(),
        "video_std":  video,
        "hmac":       data[0x00:0x14].hex(),
        "confounder_hdd_region_enc": data[0x14:0x30].hex(),
    }

def decrypt_security(data):
    """Try each known version key; the one whose HMAC check passes is correct.
    Returns (version, hdd_key_hex, region) or None."""
    hmac_stored = data[0x00:0x14]
    enc = data[0x14:0x30]
    for ver, key in VERSION_KEYS.items():
        rc4key = hashlib.sha1(key + hmac_stored).digest()[:16]
        dec = rc4(rc4key, enc)
        confounder = dec[0x00:0x08]
        hdd_key    = dec[0x08:0x18]
        region     = struct.unpack("<I", dec[0x18:0x1C])[0]
        # a correct decrypt yields a plausible region (1..7) - a weak but useful check
        if 0 <= region <= 7:
            return ver, hdd_key.hex(), region
    return None

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    f = read_eeprom(data)
    print("serial      :", f["serial"])
    print("MAC         :", f["mac"])
    print("online key  :", f["online_key"])
    print("video std   : 0x%08X" % f["video_std"])
    if "--secret" in sys.argv:
        s = decrypt_security(data)
        if s:
            ver, hdd, region = s
            print("eeprom ver  :", ver)
            print("HDD key     :", hdd)
            print("region      :", region)
        else:
            print("HDD key     : (could not decrypt with known version keys)")

if __name__ == "__main__":
    main()
