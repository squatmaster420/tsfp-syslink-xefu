#!/usr/bin/env python3
"""
tsfp_extract.py - read-only extractor for an Xbox 360-style "Games on Demand"
package (DefaultPackage + DefaultPackage.data/Data0000..DataNNNN) that holds an
original-Xbox disc image (XDVDFS).

Layout (verified against Data0000 and Data0024: every SHA-1 matched):
  * Each Data file = 1 master hash block (4 KiB), then up to 203 "sub-parts".
  * Each sub-part = 1 sub-hash block + 204 data blocks (all 4 KiB).
  * Sub-hash block = 204 SHA-1s of its data blocks.
    Master block   = SHA-1s of the 203 sub-hash blocks.
  * Data blocks of all files, concatenated, form the disc image starting at
    the XDVDFS volume descriptor (disc sector 32).

Commands:
  verify  DIR                 check every SHA-1 in the hash tables
  scan    DIR [--extract OUT] find Xbox executables (XBEH) at block starts
  calibrate DIR               show the sector shift for this package
  findgap DIR                 find where the shift starts (ls/extract do this automatically)
  ls      DIR                 list files (needs the chunk holding the root dir)
  extract DIR OUT [--match G] extract files (optionally glob-filtered)

DIR is the folder holding Data0000, Data0001, ... Missing chunks are reported;
anything that lives entirely in present chunks still works.
This tool never writes to DIR.
"""
import argparse, fnmatch, hashlib, os, re, struct, sys

BLK = 0x1000
DATA_PER_SUB = 204
SUBS_PER_FILE = 203
BLOCKS_PER_FILE = DATA_PER_SUB * SUBS_PER_FILE      # 41412 logical blocks
SECTOR = 2048
VOLUME_SECTOR = 32                                  # logical byte 0 == sector 32


class MissingChunk(Exception):
    def __init__(self, n):
        super().__init__(n)
        self.n = n


class GodImage:
    def __init__(self, folder):
        folder = folder.strip().strip('"').strip("'")
        if os.path.isfile(folder):                  # user passed a file
            folder = os.path.dirname(os.path.abspath(folder))
        sub = os.path.join(folder, "DefaultPackage.data")
        if os.path.isdir(sub):                      # user passed the parent
            folder = sub
        self.paths = {}
        try:
            names = os.listdir(folder)
        except OSError as e:
            sys.exit("Cannot read folder %r: %s" % (folder, e))
        for name in names:
            m = re.match(r"(?i)^data(\d{4})(\..*)?$", name)
            if m and os.path.isfile(os.path.join(folder, name)):
                self.paths[int(m.group(1))] = os.path.join(folder, name)
        if not self.paths:
            sys.exit("No Data0000-style files found in %r.\nFolder contains: %s\n"
                     "Tip: on Windows do not end a quoted path with a backslash."
                     % (folder, ", ".join(sorted(names)[:20]) or "(empty)"))
        self._fh = {}
        self.gaps = []          # (first_claimed_sector, sectors_missing)

    def _open(self, n):
        if n not in self.paths:
            raise MissingChunk(n)
        if n not in self._fh:
            self._fh[n] = open(self.paths[n], "rb")
        return self._fh[n]

    def data_blocks_in(self, n):
        """number of data blocks actually present in chunk n"""
        nb = os.path.getsize(self.paths[n]) // BLK - 1
        subs, rest = divmod(nb, DATA_PER_SUB + 1)
        return subs * DATA_PER_SUB + max(rest - 1, 0)

    @staticmethod
    def file_offset(block_in_file):
        k, j = divmod(block_in_file, DATA_PER_SUB)
        return (1 + k * (DATA_PER_SUB + 1) + 1 + j) * BLK

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            blk, inner = divmod(off, BLK)
            fn, b = divmod(blk, BLOCKS_PER_FILE)
            fh = self._open(fn)
            fh.seek(self.file_offset(b) + inner)
            take = min(n, BLK - inner)
            chunk = fh.read(take)
            if len(chunk) != take:
                raise MissingChunk(fn)           # truncated chunk
            out += chunk
            off += take
            n -= take
        return bytes(out)

    def map_sector(self, sector):
        """claimed XDVDFS sector -> sector as stored in the package"""
        return sector - sum(c for start, c in self.gaps if sector >= start)

    def chunk_of_sector(self, sector):
        sector = self.map_sector(sector)
        return ((sector - VOLUME_SECTOR) * SECTOR // BLK) // BLOCKS_PER_FILE

    def read_sector_range(self, sector, size):
        return self.read((self.map_sector(sector) - VOLUME_SECTOR) * SECTOR, size)


# ---------------------------------------------------------------- verify
def cmd_verify(img, _a):
    bad = 0
    for n in sorted(img.paths):
        with open(img.paths[n], "rb") as f:
            nb = os.path.getsize(img.paths[n]) // BLK
            f.seek(0)
            master = f.read(BLK)
            sub_ok = data_ok = data_tot = 0
            k = 0
            while 1 + k * 205 < nb:
                hb = 1 + k * 205
                f.seek(hb * BLK)
                h = f.read(BLK)
                if hashlib.sha1(h).digest() == master[k * 20:k * 20 + 20]:
                    sub_ok += 1
                else:
                    bad += 1
                for j in range(DATA_PER_SUB):
                    if hb + 1 + j >= nb:
                        break
                    d = f.read(BLK)
                    data_tot += 1
                    if hashlib.sha1(d).digest() == h[j * 20:j * 20 + 20]:
                        data_ok += 1
                    else:
                        bad += 1
                k += 1
        print("Data%04d: sub-hash %d/%d  data %d/%d" % (n, sub_ok, k, data_ok, data_tot))
    print("OK" if not bad else "%d mismatches" % bad)
    return 1 if bad else 0


# ---------------------------------------------------------------- XBE scan
def parse_xbe_header(h):
    if h[:4] != b"XBEH" or len(h) < 0x180:
        return None
    base, = struct.unpack_from("<I", h, 0x104)
    cert, nsec, secaddr = struct.unpack_from("<III", h, 0x118)
    co, so = cert - base, secaddr - base
    if not (0 < co < len(h) - 0x60 and 0 < nsec < 64 and 0 < so < len(h)):
        return None
    tid, = struct.unpack_from("<I", h, co + 8)
    name = h[co + 12:co + 92].decode("utf-16le", "replace").split("\0")[0]
    size = 0
    for i in range(nsec):
        o = so + i * 0x38
        if o + 0x38 > len(h):
            return None
        _, _, _, ra, rs = struct.unpack_from("<IIIII", h, o)
        size = max(size, ra + rs)
    return tid, name, size


def cmd_scan(img, a):
    for n in sorted(img.paths):
        for b in range(img.data_blocks_in(n)):
            fo = img.file_offset(b)
            fh = img._open(n)
            fh.seek(fo)
            if fh.read(4) != b"XBEH":
                continue
            logical = (n * BLOCKS_PER_FILE + b) * BLK
            try:
                info = parse_xbe_header(img.read(logical, 0x2000))
            except MissingChunk:
                info = None
            if not info:
                continue
            tid, name, size = info
            sector = logical // SECTOR + VOLUME_SECTOR
            print("Data%04d+0x%08x  sector %d  title %08X  %r  ~%d bytes"
                  % (n, fo, sector, tid, name, size))
            if a.extract:
                os.makedirs(a.extract, exist_ok=True)
                out = os.path.join(a.extract, "xbe_sector%d.xbe" % sector)
                try:
                    data = img.read(logical, size)
                except MissingChunk as e:
                    print("   (needs missing Data%04d, skipped)" % e.n)
                    continue
                with open(out, "wb") as f:
                    f.write(data)
                print("   wrote", out)
    return 0


def cmd_calibrate(img, _a):
    """Report how far claimed sectors are from where data is actually stored."""
    found = find_total_shift(img)
    if not found:
        print("no directory containing default.xbe in the chunks present")
        return 1
    root, stored, n, st, sz = found
    print("directory block found in Data%04d, stored at sector %d" % (n, stored))
    print("volume descriptor says the root directory is at sector %d" % root)
    print("default.xbe: sector %d, %d bytes" % (st, sz))
    print("shift: %d sectors" % (root - stored))
    return 0


def cmd_findgap(img, _a):
    return 0 if autogap(img) else 1


# ---------------------------------------------------------------- XDVDFS
def read_volume(img):
    d = img.read(0, SECTOR)
    if d[:20] != b"MICROSOFT*XBOX*MEDIA":
        sys.exit("XDVDFS volume descriptor not found at logical offset 0 "
                 "(is Data0000 present?)")
    root_sector, root_size = struct.unpack_from("<II", d, 20)
    return root_sector, root_size


def parse_dir(buf):
    """Parse an XDVDFS directory; return [(name, sector, size, is_dir)] or None
    if it does not look like a directory."""
    todo, done, entries = [0], set(), []
    while todo:
        off = todo.pop()
        if off in done or off + 14 > len(buf):
            continue
        done.add(off)
        l, r, st, sz, at, nl = struct.unpack_from("<HHIIBB", buf, off)
        if l == 0xFFFF and r == 0xFFFF and st == 0xFFFFFFFF:
            continue
        name = buf[off + 14:off + 14 + nl].decode("latin1")
        entries.append((name, st, sz, bool(at & 0x10)))
        if l:
            todo.append(l * 4)
        if r:
            todo.append(r * 4)
    bad = [e for e in entries if not e[0] or not all(32 <= ord(c) < 127 for c in e[0])]
    return None if bad or not entries else entries


def discover_gap(read_dir, root, root_size, total):
    """Find where the missing sectors were removed.

    read_dir(sector, size, shift) returns the bytes of a directory assuming it
    is stored `shift` sectors before its claimed position (or raises).
    Each directory is tried shifted by `total` and unshifted; whichever parses
    tells us which side of the hole it is on. The hole is then an unallocated
    stretch of >= `total` sectors between the last unshifted directory and the
    first shifted one.
    Returns (hole_start or None, allocations, unreadable_dirs, candidates)."""
    allocs, failed, seen = [], [], set()
    lo, hi = VOLUME_SECTOR, None
    todo = [(root, root_size, "/")]
    while todo:
        sec, size, path = todo.pop()
        if sec in seen or size == 0:
            continue
        seen.add(sec)
        n = (size + SECTOR - 1) // SECTOR
        allocs.append((sec, n))
        entries, used = None, None
        for shift in (total, 0):
            try:
                entries = parse_dir(read_dir(sec, size, shift))
            except MissingChunk:
                entries = None
            if entries:
                used = shift
                break
        if not entries:
            failed.append(path)
            continue
        if used == 0:
            lo = max(lo, sec + n)
        else:
            hi = sec if hi is None else min(hi, sec)
        for name, st, sz, isdir in entries:
            if isdir:
                todo.append((st, sz, path + name + "/"))
            elif sz:
                allocs.append((st, (sz + SECTOR - 1) // SECTOR))
    allocs.sort()
    holes, end = [], VOLUME_SECTOR
    for st, n in allocs:
        if st - end >= total:
            holes.append((end, st - end))
        end = max(end, st + n)
    top = hi if hi is not None else 1 << 62
    cands = [(max(h0, lo), ln) for h0, ln in holes if max(h0, lo) <= min(h0 + ln, top)]
    return (cands[0][0] if cands else None), allocs, failed, cands


def find_total_shift(img):
    """Locate the directory sector that lists default.xbe in the chunks present.
    Returns (root_sector, stored_sector) or None."""
    root, _ = read_volume(img)
    needle = b"\x0bdefault.xbe"
    for n in sorted(img.paths):
        for b in range(img.data_blocks_in(n)):
            fh = img._open(n)
            fh.seek(img.file_offset(b))
            blk = fh.read(BLK)
            i = blk.find(needle)
            if i < 14:
                continue
            l, r, st, sz, at, nl = struct.unpack_from("<HHIIBB", blk, i - 13)
            if at != 0x80 or nl != 11:
                continue
            logical = n * BLOCKS_PER_FILE + b
            stored = (logical * BLK + (i - 13) // SECTOR * SECTOR) // SECTOR + VOLUME_SECTOR
            return root, stored, n, st, sz
    return None


def autogap(img, verbose=True):
    found = find_total_shift(img)
    if not found:
        return False
    root, stored, _n, _st, _sz = found
    total = root - stored
    _, root_size = read_volume(img)

    def read_dir(sec, size, shift):
        return img.read((sec - shift - VOLUME_SECTOR) * SECTOR, size)

    hole, allocs, failed, cands = discover_gap(read_dir, root, root_size, total)
    if verbose:
        print("shift %d sectors; %d allocations mapped; candidate holes: %s"
              % (total, len(allocs), cands or "none"))
        for f in failed:
            print("  could not read directory", f, "(missing chunk or bad shift)")
    if hole is None:
        return False
    if len(cands) > 1 or failed:
        print("warning: gap position is a best guess (%d candidates, %d unreadable "
              "directories) - check the 'ls' output" % (len(cands), len(failed)),
              file=sys.stderr)
    img.gaps = [(hole, total)]
    if verbose:
        print("auto gap: --gap %d:%d" % (hole, total))
    return True


def walk(img, sector, size, prefix, seen):
    """yield (path, start_sector, size, is_dir)"""
    if size == 0 or sector in seen:
        return
    seen.add(sector)
    buf = img.read_sector_range(sector, size)
    entries = parse_dir(buf)
    if entries is None:
        print("  [directory at sector %d does not look valid - wrong --gap?]" % sector,
              file=sys.stderr)
        return
    for name, st, sz, isdir in sorted(entries, key=lambda e: e[0].lower()):
        path = prefix + name
        yield path, st, sz, isdir
        if isdir:
            try:
                yield from walk(img, st, sz, path + "/", seen)
            except MissingChunk as e:
                print("  [directory %s needs missing Data%04d]" % (path, e.n),
                      file=sys.stderr)


def iter_files(img):
    root, size = read_volume(img)
    try:
        yield from walk(img, root, size, "", set())
    except MissingChunk:
        need = img.chunk_of_sector(root)
        sys.exit("The root directory (sector %d) lives in Data%04d, which is "
                 "not in this folder. Add that file and run again." % (root, need))


def cmd_ls(img, _a):
    for path, st, sz, isdir in iter_files(img):
        have = ""
        if not isdir and sz:
            c0 = img.chunk_of_sector(st)
            c1 = img.chunk_of_sector(st + (sz - 1) // SECTOR)
            miss = [c for c in range(c0, c1 + 1) if c not in img.paths]
            have = "" if not miss else "  [missing Data%s]" % ",".join("%04d" % m for m in miss)
        print("%10d  sector %-8d %s%s" % (sz, st, path + ("/" if isdir else ""), have))
    return 0


def cmd_extract(img, a):
    done = skipped = 0
    for path, st, sz, isdir in iter_files(img):
        if isdir:
            continue
        if a.match and not fnmatch.fnmatch(path.lower(), a.match.lower()):
            continue
        dest = os.path.join(a.out, *path.split("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        try:
            with open(dest, "wb") as f:
                left, sec = sz, st
                while left > 0:
                    step = min(left, 1 << 20)
                    f.write(img.read_sector_range(sec, step))
                    left -= step
                    sec += step // SECTOR      # steps are 1 MiB until the last one
            done += 1
            print("extracted", path)
        except MissingChunk as e:
            os.remove(dest)
            skipped += 1
            print("skipped  %s (needs missing Data%04d)" % (path, e.n))
    print("%d extracted, %d skipped" % (done, skipped))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    for name in ("verify", "scan", "calibrate", "findgap", "ls", "extract"):
        s = sp.add_parser(name)
        s.add_argument("dir", help="folder containing Data0000, Data0001, ...")
        s.add_argument("--gap", action="append", default=[], metavar="SECTOR:COUNT",
                       help="claimed sectors >= SECTOR are stored COUNT sectors "
                            "earlier (see 'calibrate'); may be repeated")
        if name == "scan":
            s.add_argument("--extract", metavar="OUT")
        if name == "extract":
            s.add_argument("out")
            s.add_argument("--match", help="glob on the full path, e.g. '*.pak'")
    a = p.parse_args()
    img = GodImage(a.dir)
    for g in a.gap:
        st, cnt = g.split(":")
        img.gaps.append((int(st), int(cnt)))
    if a.cmd in ("ls", "extract") and not img.gaps:
        if not autogap(img, verbose=False):
            print("note: could not work out the sector shift automatically; "
                  "run 'findgap' or pass --gap", file=sys.stderr)
        else:
            print("auto gap: --gap %d:%d" % img.gaps[0], file=sys.stderr)
    sys.exit({"verify": cmd_verify, "scan": cmd_scan, "calibrate": cmd_calibrate,
              "findgap": cmd_findgap,
              "ls": cmd_ls, "extract": cmd_extract}[a.cmd](img, a))


if __name__ == "__main__":
    main()
