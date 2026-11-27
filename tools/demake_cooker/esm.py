"""TES3 (Morrowind) ESM/ESP reader + minimal writer.

Loads the records the world and RPG cookers need from a load-ordered list of
content files and merges them the way the engine does:

* object records (STAT, DOOR, LIGH, ...) are keyed by lower-cased id; later
  files override earlier ones, DELE removes
* CELL records merge per cell (interior by name, exterior by grid); refs are
  keyed by (content file, ref index) using the FRMR top byte as a 1-based
  index into the plugin's master list (OpenMW adjustRefNum)
* MVRF moves a ref into the exterior cell named by CNDT
* LAND keeps the latest version; LTEX indices are per content file
* LAND heights are decoded from VHGT row/column deltas (x8 height scale) and
  VTEX is de-swizzled from its 4x4-blocks-of-4x4 order (OpenMW loadland.cpp)
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

LAND_SIZE = 65
LAND_NUM_VERTS = LAND_SIZE * LAND_SIZE
LAND_TEX_SIZE = 16
CELL_SIZE = 8192.0
HEIGHT_SCALE = 8.0

CELL_INTERIOR = 0x01
CELL_HAS_WATER = 0x02
CELL_NO_SLEEP = 0x04
CELL_QUASI_EX = 0x80

# Records whose instances carry a model and can be placed in cells.
OBJECT_TYPES = ("STAT", "ACTI", "DOOR", "CONT", "LIGH", "MISC", "WEAP", "ARMO", "CLOT",
                "BOOK", "ALCH", "INGR", "APPA", "LOCK", "PROB", "REPA", "NPC_", "CREA",
                "LEVC", "LEVI", "BODY")


class EsmError(Exception):
    pass


def zstr(b: bytes) -> str:
    n = b.find(b"\0")
    if n >= 0:
        b = b[:n]
    return b.decode("latin-1")


@dataclass
class Record:
    type: str
    flags: int
    subs: List[Tuple[str, bytes]]

    def get(self, name: str) -> Optional[bytes]:
        for n, d in self.subs:
            if n == name:
                return d
        return None

    def has(self, name: str) -> bool:
        return any(n == name for n, _ in self.subs)


def iter_records(data: bytes, wanted: Optional[set] = None) -> Iterator[Record]:
    pos = 0
    end = len(data)
    while pos < end:
        if pos + 16 > end:
            raise EsmError(f"truncated record header at 0x{pos:x}")
        rtype = data[pos:pos + 4].decode("latin-1")
        size, _unused, flags = struct.unpack_from("<III", data, pos + 4)
        body = pos + 16
        if body + size > end:
            raise EsmError(f"record {rtype} at 0x{pos:x} runs past end of file")
        if wanted is None or rtype in wanted:
            subs = []
            p = body
            stop = body + size
            while p < stop:
                if p + 8 > stop:
                    raise EsmError(f"truncated subrecord in {rtype} at 0x{p:x}")
                sname = data[p:p + 4].decode("latin-1")
                (ssize,) = struct.unpack_from("<I", data, p + 4)
                if p + 8 + ssize > stop:
                    raise EsmError(f"subrecord {rtype}.{sname} runs past record end")
                subs.append((sname, data[p + 8:p + 8 + ssize]))
                p += 8 + ssize
            yield Record(rtype, flags, subs)
        pos = body + size


@dataclass
class ObjectDef:
    type: str
    id: str
    model: str = ""
    name: str = ""
    script: str = ""
    content: int = 0
    light: Optional[dict] = None  # LIGH: radius, color, flags, time
    race: str = ""        # NPC_ RNAM / BODY FNAM
    head: str = ""        # NPC_ BNAM (BODY id)
    hair: str = ""        # NPC_ KNAM (BODY id)
    npc_flags: int = 0    # NPC_ FLAG (bit 0 = female)
    body: Optional[Tuple[int, int, int, int]] = None  # BODY BYDT: part, vampire, flags, type


@dataclass
class CellRef:
    content: int
    index: int
    id: str
    pos: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    rot: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    scale: float = 1.0
    door_dest: Optional[Tuple[float, ...]] = None  # x y z rx ry rz
    dest_cell: str = ""
    lock_level: int = 0
    key: str = ""
    trap: str = ""
    owner: str = ""
    owner_faction: str = ""
    count: int = 1
    deleted: bool = False
    persistent: bool = False


@dataclass
class Cell:
    name: str
    flags: int
    x: int = 0
    y: int = 0
    region: str = ""
    water: Optional[float] = None
    ambient: Optional[Tuple[int, int, int, float]] = None  # ambient, sun, fog (RGBA u32), density
    refs: Dict[Tuple[int, int], CellRef] = field(default_factory=dict)

    @property
    def interior(self) -> bool:
        return bool(self.flags & CELL_INTERIOR)

    @property
    def key(self):
        return ("int", self.name.lower()) if self.interior else ("ext", self.x, self.y)


@dataclass
class Land:
    x: int
    y: int
    content: int
    heights: Optional[List[float]] = None  # 65*65, row-major, y north
    normals: Optional[bytes] = None  # 65*65*3 signed
    colors: Optional[bytes] = None  # 65*65*3
    textures: Optional[List[int]] = None  # 16*16, row-major, 0 = default


def decode_heights(vhgt: bytes) -> List[float]:
    if len(vhgt) < 4 + LAND_NUM_VERTS:
        raise EsmError("short VHGT")
    (offset,) = struct.unpack_from("<f", vhgt, 0)
    deltas = struct.unpack_from(f"<{LAND_NUM_VERTS}b", vhgt, 4)
    out = [0.0] * LAND_NUM_VERTS
    row = offset
    for y in range(LAND_SIZE):
        row += deltas[y * LAND_SIZE]
        out[y * LAND_SIZE] = row * HEIGHT_SCALE
        col = row
        for x in range(1, LAND_SIZE):
            col += deltas[y * LAND_SIZE + x]
            out[y * LAND_SIZE + x] = col * HEIGHT_SCALE
    return out


def encode_heights(heights: Sequence[float]) -> bytes:
    """Inverse of decode_heights (OpenMW Land::save rounding). Test fixtures."""
    def q(v: float) -> int:
        return int(v + 0.5) if v >= 0 else int(v - 0.5)

    offset = heights[0] / HEIGHT_SCALE
    deltas = []
    row_prev = offset
    for y in range(LAND_SIZE):
        first = heights[y * LAND_SIZE] / HEIGHT_SCALE
        d = q(first - row_prev)
        d = max(-128, min(127, d))
        deltas.append(d)
        row_prev = row_prev + d
        col_prev = row_prev
        for x in range(1, LAND_SIZE):
            v = heights[y * LAND_SIZE + x] / HEIGHT_SCALE
            d = max(-128, min(127, q(v - col_prev)))
            deltas.append(d)
            col_prev += d
    return struct.pack("<f", offset) + struct.pack(f"<{LAND_NUM_VERTS}b", *deltas) + b"\0\0\0"


def detranspose_vtex(raw: Sequence[int]) -> List[int]:
    out = [0] * 256
    i = 0
    for y1 in range(4):
        for x1 in range(4):
            for y2 in range(4):
                for x2 in range(4):
                    out[(y1 * 4 + y2) * 16 + (x1 * 4 + x2)] = raw[i]
                    i += 1
    return out


def transpose_vtex(grid: Sequence[int]) -> List[int]:
    out = []
    for y1 in range(4):
        for x1 in range(4):
            for y2 in range(4):
                for x2 in range(4):
                    out.append(grid[(y1 * 4 + y2) * 16 + (x1 * 4 + x2)])
    return out


class World:
    """Merged view over a load-ordered list of ESM/ESP files."""

    def __init__(self) -> None:
        self.files: List[str] = []
        self.objects: Dict[str, ObjectDef] = {}
        self.cells: Dict[tuple, Cell] = {}
        self.lands: Dict[Tuple[int, int], Land] = {}
        self.ltex: Dict[Tuple[int, int], str] = {}  # (content, index) -> texture path
        self.regions: Dict[str, dict] = {}
        self.races: Dict[str, dict] = {}
        self.warnings: List[str] = []

    # ------------------------------------------------------------ loading
    def load(self, paths: Sequence[str]) -> "World":
        name_to_index: Dict[str, int] = {}
        for path in paths:
            with open(path, "rb") as f:
                data = f.read()
            self.load_bytes(data, os.path.basename(path), name_to_index)
        return self

    def load_bytes(self, data: bytes, filename: str, name_to_index: Optional[Dict[str, int]] = None
                   ) -> None:
        if name_to_index is None:
            name_to_index = {os.path.basename(f).lower(): i for i, f in enumerate(self.files)}
        content = len(self.files)
        self.files.append(filename)
        name_to_index[filename.lower()] = content
        masters: List[int] = []
        first = True
        for rec in iter_records(data):
            if first:
                if rec.type != "TES3":
                    raise EsmError(f"{filename}: missing TES3 header")
                for n, d in rec.subs:
                    if n == "MAST":
                        m = zstr(d).lower()
                        if m not in name_to_index:
                            self.warnings.append(f"{filename}: master {m} not loaded")
                            masters.append(content)
                        else:
                            masters.append(name_to_index[m])
                first = False
                continue
            t = rec.type
            if t in OBJECT_TYPES:
                self._load_object(rec, content)
            elif t == "CELL":
                self._load_cell(rec, content, masters)
            elif t == "LAND":
                self._load_land(rec, content)
            elif t == "LTEX":
                self._load_ltex(rec, content)
            elif t == "REGN":
                self._load_region(rec)
            elif t == "RACE":
                self._load_race(rec)

    def _load_object(self, rec: Record, content: int) -> None:
        name = rec.get("NAME")
        if name is None:
            return
        oid = zstr(name)
        key = oid.lower()
        if rec.has("DELE"):
            self.objects.pop(key, None)
            return
        obj = ObjectDef(rec.type, oid, content=content)
        for n, d in rec.subs:
            if n == "MODL":
                obj.model = zstr(d)
            elif n == "FNAM" and rec.type != "BODY":
                obj.name = zstr(d)
            elif n == "SCRI":
                obj.script = zstr(d)
            elif n == "RNAM":
                obj.race = zstr(d)
            elif n == "BNAM" and rec.type == "NPC_":
                obj.head = zstr(d)
            elif n == "KNAM" and rec.type == "NPC_":
                obj.hair = zstr(d)
            elif n == "FLAG" and rec.type == "NPC_" and len(d) >= 4:
                obj.npc_flags = struct.unpack_from("<i", d, 0)[0] & 0xFF
            elif n == "BYDT" and len(d) >= 4:
                obj.body = tuple(d[:4])  # type: ignore[assignment]
            elif n == "FNAM" and rec.type == "BODY":
                obj.race = zstr(d)
            elif n == "LHDT" and len(d) >= 24:
                weight, value, time, radius, color, flags = struct.unpack_from("<fiiIII", d, 0)
                obj.light = {"radius": radius, "color": color, "flags": flags, "time": time}
        self.objects[key] = obj

    def _load_ltex(self, rec: Record, content: int) -> None:
        intv = rec.get("INTV")
        tex = rec.get("DATA")
        if intv is None or tex is None:
            return
        (index,) = struct.unpack_from("<I", intv, 0)
        self.ltex[(content, index)] = zstr(tex)

    def _load_region(self, rec: Record) -> None:
        name = rec.get("NAME")
        if name is None:
            return
        reg = {"id": zstr(name), "name": zstr(rec.get("FNAM") or b""), "color": 0}
        cnam = rec.get("CNAM")
        if cnam and len(cnam) >= 4:
            (reg["color"],) = struct.unpack_from("<I", cnam, 0)
        self.regions[reg["id"].lower()] = reg

    def _load_race(self, rec: Record) -> None:
        name = rec.get("NAME")
        if name is None:
            return
        race = {"id": zstr(name), "name": zstr(rec.get("FNAM") or b""), "flags": 0}
        radt = rec.get("RADT")
        if radt is not None and len(radt) >= 140:
            (race["flags"],) = struct.unpack_from("<i", radt, 136)  # 1 playable, 2 beast
        self.races[race["id"].lower()] = race

    def _load_land(self, rec: Record, content: int) -> None:
        intv = rec.get("INTV")
        if intv is None or len(intv) < 8:
            return
        x, y = struct.unpack_from("<ii", intv, 0)
        land = Land(x, y, content)
        vhgt = rec.get("VHGT")
        if vhgt is not None:
            land.heights = decode_heights(vhgt)
        vnml = rec.get("VNML")
        if vnml is not None and len(vnml) >= LAND_NUM_VERTS * 3:
            land.normals = vnml[:LAND_NUM_VERTS * 3]
        vclr = rec.get("VCLR")
        if vclr is not None and len(vclr) >= LAND_NUM_VERTS * 3:
            land.colors = vclr[:LAND_NUM_VERTS * 3]
        vtex = rec.get("VTEX")
        if vtex is not None and len(vtex) >= 512:
            land.textures = detranspose_vtex(struct.unpack_from("<256H", vtex, 0))
        self.lands[(x, y)] = land

    def _ref_key(self, raw_refnum: int, content: int, masters: Sequence[int]) -> Tuple[int, int]:
        local = (raw_refnum >> 24) & 0xFF
        if local and local <= len(masters):
            return masters[local - 1], raw_refnum & 0x00FFFFFF
        return content, raw_refnum

    def _load_cell(self, rec: Record, content: int, masters: Sequence[int]) -> None:
        subs = rec.subs
        i = 0
        name = ""
        flags = 0
        gx = gy = 0
        have_data = False
        # header subrecords until the first reference
        while i < len(subs) and subs[i][0] not in ("FRMR", "MVRF"):
            n, d = subs[i]
            if n == "NAME":
                name = zstr(d)
            elif n == "DATA" and len(d) >= 12:
                flags, gx, gy = struct.unpack_from("<iii", d, 0)
                have_data = True
            i += 1
        if not have_data:
            self.warnings.append(f"cell {name!r} without DATA")
            return
        key = ("int", name.lower()) if flags & CELL_INTERIOR else ("ext", gx, gy)
        cell = self.cells.get(key)
        if cell is None:
            cell = Cell(name, flags, gx, gy)
            self.cells[key] = cell
        else:
            cell.flags = flags
            if name:
                cell.name = name
        for n, d in subs[:i]:
            if n == "RGNN":
                cell.region = zstr(d)
            elif n == "WHGT" and len(d) >= 4:
                cell.water = struct.unpack_from("<f", d, 0)[0]
            elif n == "INTV" and len(d) >= 4:
                cell.water = float(struct.unpack_from("<i", d, 0)[0])
            elif n == "AMBI" and len(d) >= 16:
                cell.ambient = struct.unpack_from("<IIIf", d, 0)
            elif n == "DELE":
                self.cells.pop(key, None)
                return

        persistent = True
        moved_target: Optional[Tuple[int, int]] = None
        ref: Optional[CellRef] = None
        target_cell = cell

        def commit():
            if ref is None:
                return
            dest = target_cell
            if ref.deleted:
                dest.refs.pop((ref.content, ref.index), None)
            else:
                dest.refs[(ref.content, ref.index)] = ref

        while i < len(subs):
            n, d = subs[i]
            i += 1
            if n == "MVRF":
                commit()
                ref = None
                moved_target = None
                if i < len(subs) and subs[i][0] == "CNDT" and len(subs[i][1]) >= 8:
                    moved_target = struct.unpack_from("<ii", subs[i][1], 0)
                    i += 1
                elif i < len(subs) and subs[i][0] == "CNAM":
                    i += 1  # moved to a named (interior) cell: data follows in that cell
                continue
            if n == "FRMR":
                commit()
                (raw,) = struct.unpack_from("<I", d, 0)
                c, idx = self._ref_key(raw, content, masters)
                ref = CellRef(c, idx, "", persistent=persistent)
                target_cell = cell
                if moved_target is not None:
                    tkey = ("ext", moved_target[0], moved_target[1])
                    target_cell = self.cells.get(tkey)
                    if target_cell is None:
                        target_cell = Cell("", 0, moved_target[0], moved_target[1])
                        self.cells[tkey] = target_cell
                    cell.refs.pop((c, idx), None)
                    moved_target = None
                continue
            if n == "NAM0":
                persistent = False
                continue
            if ref is None:
                continue
            if n == "NAME":
                ref.id = zstr(d)
            elif n == "XSCL":
                ref.scale = min(2.0, max(0.5, struct.unpack_from("<f", d, 0)[0]))
            elif n == "DATA" and len(d) >= 24:
                v = struct.unpack_from("<6f", d, 0)
                ref.pos, ref.rot = v[:3], v[3:]
            elif n == "DODT" and len(d) >= 24:
                ref.door_dest = struct.unpack_from("<6f", d, 0)
            elif n == "DNAM":
                ref.dest_cell = zstr(d)
            elif n == "FLTV":
                ref.lock_level = struct.unpack_from("<i", d, 0)[0]
            elif n == "KNAM":
                ref.key = zstr(d)
            elif n == "TNAM":
                ref.trap = zstr(d)
            elif n == "ANAM":
                ref.owner = zstr(d)
            elif n == "CNAM":
                ref.owner_faction = zstr(d)
            elif n == "NAM9" and len(d) >= 4:
                ref.count = struct.unpack_from("<i", d, 0)[0]
            elif n == "DELE":
                ref.deleted = True
        commit()

    # ------------------------------------------------------------ helpers
    def ltex_path(self, land: Land, value: int) -> Optional[str]:
        """VTEX value -> texture path (0 = default land texture)."""
        if value == 0:
            return "_land_default.dds"
        path = self.ltex.get((land.content, value - 1))
        if path is None:
            for (c, idx), p in self.ltex.items():  # fall back to any file's index
                if idx == value - 1:
                    return p
        return path

    def exterior_cells(self) -> List[Cell]:
        return [c for c in self.cells.values() if not c.interior]

    def interior_cells(self) -> List[Cell]:
        return [c for c in self.cells.values() if c.interior]


# --------------------------------------------------------------------------
# writer (synthetic test worlds)
# --------------------------------------------------------------------------


def sub(name: str, data: bytes) -> bytes:
    return name.encode("latin-1") + struct.pack("<I", len(data)) + data


def zs(s: str) -> bytes:
    return s.encode("latin-1") + b"\0"


def record(rtype: str, subs: Sequence[bytes], flags: int = 0) -> bytes:
    body = b"".join(subs)
    return rtype.encode("latin-1") + struct.pack("<III", len(body), 0, flags) + body


def tes3_header(nrecords: int, masters: Sequence[Tuple[str, int]] = (), author: str = "",
                desc: str = "") -> bytes:
    hedr = struct.pack("<fI32s256sI", 1.3, 0, author.encode()[:32], desc.encode()[:256],
                       nrecords)
    subs = [sub("HEDR", hedr)]
    for m, size in masters:
        subs.append(sub("MAST", zs(m)))
        subs.append(sub("DATA", struct.pack("<Q", size)))
    return record("TES3", subs)


def write_esm(path: str, records: Sequence[bytes], masters: Sequence[Tuple[str, int]] = ()) -> None:
    with open(path, "wb") as f:
        f.write(tes3_header(len(records), masters))
        for r in records:
            f.write(r)
