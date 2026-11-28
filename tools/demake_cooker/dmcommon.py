"""Shared helpers for the demake cooker.

* asset key normalisation + FNV-1a hashing (mirrors pw_hash_name in src/common.h)
* Morrowind (TES3) BSA archive reader and writer
* DataSource: layered "Data Files" view (loose files override BSAs, later
  BSAs override earlier ones) - the same rule the original engine uses
* DPAK pack writer/reader (layout defined in src/fmt.h)
* BinWriter: little-endian binary builder with alignment and patching
* parallel map helper built on multiprocessing
"""

from __future__ import annotations

import io
import logging
import multiprocessing
import os
import struct
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, Iterator, List, Optional, Sequence, Tuple

LOG = logging.getLogger("demake")

# --------------------------------------------------------------------------
# hashing / keys
# --------------------------------------------------------------------------

FNV_OFFSET = 2166136261
FNV_PRIME = 16777619


def fnv1a32(key: str) -> int:
    """FNV-1a over a key, lower-cased with '\\' -> '/' (same as pw_hash_name)."""
    h = FNV_OFFSET
    for ch in key.encode("latin-1", errors="replace"):
        if 65 <= ch <= 90:
            ch += 32
        elif ch == 0x5C:
            ch = 0x2F
        h ^= ch
        h = (h * FNV_PRIME) & 0xFFFFFFFF
    return h


def normalize_path(path: str) -> str:
    """Lower case, forward slashes, no leading './' or '/'."""
    p = path.replace("\\", "/").lower().strip()
    while p.startswith("./"):
        p = p[2:]
    p = p.lstrip("/")
    if p.startswith("data files/"):
        p = p[len("data files/"):]
    return p


TEXTURE_EXTS = (".dds", ".tga", ".bmp", ".png")


def texture_key(path: str) -> str:
    """'Textures\\Tx_Wood_01.TGA' -> 'tx_wood_01'. NIFs reference .tga while
    the BSA ships .dds, so the key drops the extension and the folder."""
    p = normalize_path(path)
    if p.startswith("textures/"):
        p = p[len("textures/"):]
    base, ext = os.path.splitext(p)
    if ext in TEXTURE_EXTS:
        p = base
    return p


def sound_key(path: str, folder: str = "sound") -> str:
    """Sound file key: 'Fx\\FOOT\\walk.wav' -> 'sound/fx/foot/walk'; with
    folder='music', 'Explore/mx_1.mp3' -> 'music/explore/mx_1'. Paths that
    already start with Sound/ or Music/ keep that folder."""
    p = normalize_path(path)
    for pre in ("sound/", "music/"):
        if p.startswith(pre):
            folder, p = pre[:-1], p[len(pre):]
            break
    base, ext = os.path.splitext(p)
    if ext in (".wav", ".mp3"):
        p = base
    return folder + "/" + p


def mesh_key(path: str) -> str:
    """'Meshes\\x\\Ex_Hlaalu_B_01.NIF' -> 'x/ex_hlaalu_b_01'."""
    p = normalize_path(path)
    if p.startswith("meshes/"):
        p = p[len("meshes/"):]
    base, ext = os.path.splitext(p)
    if ext in (".nif", ".kf"):
        p = base
    return p


# --------------------------------------------------------------------------
# binary writer
# --------------------------------------------------------------------------


class BinWriter:
    """Little-endian byte builder."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def __len__(self) -> int:
        return len(self.buf)

    def tell(self) -> int:
        return len(self.buf)

    def pack(self, fmt: str, *values) -> int:
        ofs = len(self.buf)
        self.buf += struct.pack("<" + fmt, *values)
        return ofs

    def bytes(self, data: bytes) -> int:
        ofs = len(self.buf)
        self.buf += data
        return ofs

    def align(self, n: int, fill: int = 0) -> int:
        pad = (-len(self.buf)) % n
        if pad:
            self.buf += bytes([fill]) * pad
        return len(self.buf)

    def reserve(self, size: int) -> int:
        ofs = len(self.buf)
        self.buf += b"\0" * size
        return ofs

    def patch(self, ofs: int, fmt: str, *values) -> None:
        struct.pack_into("<" + fmt, self.buf, ofs, *values)

    def getvalue(self) -> bytes:
        return bytes(self.buf)


class StringTable:
    """De-duplicated NUL-terminated string block."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.index: Dict[str, int] = {}

    def add(self, s: str) -> int:
        if s in self.index:
            return self.index[s]
        ofs = len(self.data)
        self.data += s.encode("latin-1", errors="replace") + b"\0"
        self.index[s] = ofs
        return ofs


# --------------------------------------------------------------------------
# BSA (TES3, version 0x100)
# --------------------------------------------------------------------------

BSA_VERSION = 0x100


def bsa_hash(name: str) -> Tuple[int, int]:
    """TES3 BSA name hash (low, high). name: lower case with backslashes."""
    data = name.lower().replace("/", "\\").encode("latin-1", errors="replace")
    length = len(data)
    half = length >> 1
    low = 0
    off = 0
    for i in range(half):
        low ^= (data[i] << (off & 0x1F)) & 0xFFFFFFFF
        off += 8
    high = 0
    off = 0
    for i in range(half, length):
        temp = (data[i] << (off & 0x1F)) & 0xFFFFFFFF
        high ^= temp
        n = temp & 0x1F
        if n:
            high = ((high >> n) | (high << (32 - n))) & 0xFFFFFFFF
        off += 8
    return low, high


@dataclass
class BSAEntry:
    name: str  # normalised (forward slashes, lower case)
    offset: int  # absolute file offset
    size: int


class BSAArchive:
    """Read-only TES3 BSA archive."""

    def __init__(self, path: str) -> None:
        self.path = path
        self.entries: Dict[str, BSAEntry] = {}
        with open(path, "rb") as f:
            header = f.read(12)
            if len(header) != 12:
                raise ValueError(f"{path}: truncated BSA header")
            version, dir_size, count = struct.unpack("<III", header)
            if version != BSA_VERSION:
                raise ValueError(f"{path}: not a TES3 BSA (version 0x{version:x})")
            directory = f.read(dir_size)
            if len(directory) != dir_size:
                raise ValueError(f"{path}: truncated BSA directory")
        records_size = count * 8
        name_ofs_size = count * 4
        if records_size + name_ofs_size > dir_size:
            raise ValueError(f"{path}: corrupt BSA directory")
        data_start = 12 + dir_size + count * 8
        names_base = records_size + name_ofs_size
        file_size = os.path.getsize(path)
        for i in range(count):
            size, offset = struct.unpack_from("<II", directory, i * 8)
            (name_ofs,) = struct.unpack_from("<I", directory, records_size + i * 4)
            start = names_base + name_ofs
            end = directory.index(b"\0", start)
            raw = directory[start:end].decode("latin-1")
            entry = BSAEntry(normalize_path(raw), data_start + offset, size)
            if entry.offset + entry.size > file_size:
                raise ValueError(f"{path}: entry {raw} runs past end of archive")
            self.entries[entry.name] = entry

    def names(self) -> Iterable[str]:
        return self.entries.keys()

    def read(self, name: str) -> bytes:
        e = self.entries[normalize_path(name)]
        with open(self.path, "rb") as f:
            f.seek(e.offset)
            data = f.read(e.size)
        if len(data) != e.size:
            raise IOError(f"{self.path}: short read for {name}")
        return data


def write_bsa(path: str, files: Dict[str, bytes]) -> None:
    """Write a TES3 BSA (used by tests and for repacking cooked loose data)."""
    items = []
    for name, data in files.items():
        stored = name.lower().replace("/", "\\")
        items.append((bsa_hash(stored), stored, data))
    items.sort(key=lambda t: (t[0][0], t[0][1]))
    count = len(items)
    names = bytearray()
    name_offsets = []
    for _, stored, _ in items:
        name_offsets.append(len(names))
        names += stored.encode("latin-1") + b"\0"
    dir_size = count * 8 + count * 4 + len(names)
    out = bytearray(struct.pack("<III", BSA_VERSION, dir_size, count))
    data_ofs = 0
    for _, _, data in items:
        out += struct.pack("<II", len(data), data_ofs)
        data_ofs += len(data)
    for o in name_offsets:
        out += struct.pack("<I", o)
    out += names
    for (low, high), _, _ in items:
        out += struct.pack("<II", low, high)
    for _, _, data in items:
        out += data
    with open(path, "wb") as f:
        f.write(out)


# --------------------------------------------------------------------------
# DataSource
# --------------------------------------------------------------------------

STOCK_ARCHIVE_ORDER = ("morrowind.bsa", "tribunal.bsa", "bloodmoon.bsa")


@dataclass(frozen=True)
class DataConfig:
    """Picklable description of a DataSource (sent to worker processes)."""

    data_dirs: Tuple[str, ...] = ()
    bsas: Tuple[str, ...] = ()
    scan_bsas_in_data_dirs: bool = True


class DataSource:
    """Layered virtual file system over loose files and BSA archives."""

    def __init__(self, config: DataConfig) -> None:
        self.config = config
        self.archives: List[BSAArchive] = []
        self.loose: Dict[str, str] = {}  # normalised name -> real path
        bsa_paths: List[str] = []
        for d in config.data_dirs:
            if config.scan_bsas_in_data_dirs and os.path.isdir(d):
                found = {f.lower(): os.path.join(d, f) for f in os.listdir(d)
                         if f.lower().endswith(".bsa")}
                for stock in STOCK_ARCHIVE_ORDER:
                    if stock in found:
                        bsa_paths.append(found.pop(stock))
                bsa_paths.extend(found[k] for k in sorted(found))
        bsa_paths.extend(config.bsas)
        for p in bsa_paths:
            self.archives.append(BSAArchive(p))
        for d in config.data_dirs:
            for root, _dirs, files in os.walk(d):
                for fn in files:
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, d)
                    if rel.lower().endswith(".bsa") and os.path.dirname(rel) == "":
                        continue
                    self.loose[normalize_path(rel)] = full

    def _locate(self, name: str):
        n = normalize_path(name)
        if n in self.loose:
            return ("loose", self.loose[n])
        for arc in reversed(self.archives):
            if n in arc.entries:
                return ("bsa", arc)
        return None

    def exists(self, name: str) -> bool:
        return self._locate(name) is not None

    def read(self, name: str) -> bytes:
        loc = self._locate(name)
        if loc is None:
            raise FileNotFoundError(name)
        kind, where = loc
        if kind == "loose":
            with open(where, "rb") as f:
                return f.read()
        return where.read(name)

    def list(self, prefix: str = "", exts: Sequence[str] = ()) -> List[str]:
        """All visible names under prefix with one of exts (normalised)."""
        prefix = normalize_path(prefix)
        names = set(self.loose.keys())
        for arc in self.archives:
            names.update(arc.names())
        out = []
        for n in names:
            if prefix and not n.startswith(prefix):
                continue
            if exts and not n.endswith(tuple(exts)):
                continue
            out.append(n)
        out.sort()
        return out


# --------------------------------------------------------------------------
# DPAK
# --------------------------------------------------------------------------

DPAK_MAGIC = b"DPAK"
DPAK_VERSION = 1
DPAK_ENTRY_ALIGN = 64
DPAK_KIND_GENERIC = 0
DPAK_KIND_TEXTURE = 1
DPAK_KIND_MESH = 2
DPAK_HEADER_FMT = "<4sHHIIII8x"  # 32 bytes
DPAK_ENTRY_FMT = "<IIII"  # 16 bytes


ACTOR_TEMPLATE_SALT = 0x41C7A2E5


def entry_hash(name: str) -> int:
    """Directory hash of a pack entry: pw_hash_name(name), except actor
    templates ("actor/<id>"), keyed pw_hash_name(id) ^ ACTOR_TEMPLATE_SALT so
    the runtime finds them from an object id hash alone."""
    if name.startswith("actor/"):
        return fnv1a32(name[6:]) ^ ACTOR_TEMPLATE_SALT
    return fnv1a32(name)


class DPakWriter:
    def __init__(self, kind: int) -> None:
        self.kind = kind
        self.items: Dict[str, bytes] = {}

    def add(self, name: str, data: bytes) -> None:
        h = entry_hash(name)
        for other in self.items:
            if other != name and entry_hash(other) == h:
                raise ValueError(f"DPAK hash collision: {name!r} vs {other!r}")
        self.items[name] = data

    def build(self) -> bytes:
        entries = sorted(self.items.items(), key=lambda kv: entry_hash(kv[0]))
        names = StringTable()
        name_ofs = [names.add(n) for n, _ in entries]
        w = BinWriter()
        w.reserve(32)
        dir_ofs = w.tell()
        w.reserve(16 * len(entries))
        names_ofs = w.bytes(bytes(names.data))
        records = []
        for (name, data), nofs in zip(entries, name_ofs):
            w.align(DPAK_ENTRY_ALIGN)
            ofs = w.bytes(data)
            records.append((entry_hash(name), ofs, len(data), nofs))
        for i, rec in enumerate(records):
            w.patch(dir_ofs + 16 * i, "IIII", *rec)
        w.patch(0, "4sHHIIII8x", DPAK_MAGIC, DPAK_VERSION, self.kind, len(records),
                dir_ofs, names_ofs, len(names.data))
        return w.getvalue()

    def write(self, path: str) -> int:
        data = self.build()
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        return len(data)


class DPakReader:
    def __init__(self, data: bytes) -> None:
        magic, version, kind, count, dir_ofs, names_ofs, names_size = struct.unpack_from(
            DPAK_HEADER_FMT, data, 0)
        if magic != DPAK_MAGIC or version != DPAK_VERSION:
            raise ValueError("not a DPAK v1")
        self.data = data
        self.kind = kind
        self.entries: Dict[str, Tuple[int, int, int]] = {}
        prev_hash = -1
        for i in range(count):
            h, ofs, size, nofs = struct.unpack_from(DPAK_ENTRY_FMT, data, dir_ofs + 16 * i)
            if h < prev_hash:
                raise ValueError("DPAK directory not sorted")
            prev_hash = h
            start = names_ofs + nofs
            name = data[start:data.index(b"\0", start)].decode("latin-1")
            if entry_hash(name) != h:
                raise ValueError(f"DPAK hash mismatch for {name}")
            if ofs % DPAK_ENTRY_ALIGN:
                raise ValueError("DPAK entry misaligned")
            self.entries[name] = (h, ofs, size)

    def get(self, name: str) -> bytes:
        _h, ofs, size = self.entries[name]
        return self.data[ofs:ofs + size]

    def find_hash(self, h: int) -> Optional[str]:
        for name, (eh, _o, _s) in self.entries.items():
            if eh == h:
                return name
        return None


# --------------------------------------------------------------------------
# parallel map
# --------------------------------------------------------------------------

_WORKER_STATE: Dict[str, object] = {}


def _worker_init(init_fn, init_arg) -> None:
    _WORKER_STATE["ctx"] = init_fn(init_arg)


def _worker_call(args):
    fn, item = args
    return fn(_WORKER_STATE["ctx"], item)


def parallel_map(fn: Callable, items: Sequence, init_fn: Callable, init_arg, jobs: int,
                 progress: Optional[Callable[[int, int], None]] = None) -> Iterator:
    """Yield fn(ctx, item) for each item, ctx = init_fn(init_arg) built once per
    worker. Order of results is not guaranteed (each result should be self
    describing). jobs <= 1 runs in-process."""
    total = len(items)
    if jobs <= 1 or total < 2:
        ctx = init_fn(init_arg)
        for i, item in enumerate(items):
            yield fn(ctx, item)
            if progress:
                progress(i + 1, total)
        return
    with multiprocessing.get_context("spawn").Pool(
            jobs, initializer=_worker_init, initargs=(init_fn, init_arg)) as pool:
        for i, res in enumerate(pool.imap_unordered(_worker_call, [(fn, it) for it in items],
                                                    chunksize=4)):
            yield res
            if progress:
                progress(i + 1, total)


def setup_logging(verbose: bool) -> None:
    logging.basicConfig(level=logging.DEBUG if verbose else logging.INFO,
                        format="%(levelname)s %(message)s")


def progress_printer(label: str) -> Callable[[int, int], None]:
    step = [0]

    def report(done: int, total: int) -> None:
        pct = done * 100 // max(total, 1)
        if pct >= step[0] or done == total:
            LOG.info("%s: %d/%d (%d%%)", label, done, total, pct)
            step[0] = pct + 10

    return report


def image_pixels(img) -> list:
    """Pixel tuples in row-major order (Pillow 13 deprecates getdata())."""
    getter = getattr(img, "get_flattened_data", None)
    return list(getter() if getter is not None else img.getdata())


def read_file(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def read_image_bytes(data: bytes):
    """Decode DDS/TGA/BMP/PNG bytes to an RGBA Pillow image."""
    from PIL import Image  # local import: dmcommon is usable without Pillow

    img = Image.open(io.BytesIO(data))
    img.load()
    return img.convert("RGBA")
