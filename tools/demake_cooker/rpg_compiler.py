#!/usr/bin/env python3
"""rpg_compiler - Morrowind game data -> rpg.dpak (dialogue, quests, scripts...).

Extracts every record the RPG layer needs from the content files in load
order and writes one pack:

    rpg/core        resident database (DRpgHeader + tables + name pool)
    text/<n>        raw-DEFLATE blocks (~32 KB) of INFO/BOOK text and the
                    compiled dialogue result scripts, streamed on demand
    script/<id>     compiled MWScript bytecode (code + string pool)

Records: GLOB, GMST, SCPT (+SSCR start scripts), DIAL/INFO (load-order
merge via PNAM/NNAM, SCVR conditions, journal flags, result scripts),
FACT, SPEL, ENCH, CLAS, RACE, REGN and every object type (NPC_, CREA,
WEAP, ARMO, CLOT, BOOK, ALCH, INGR, MISC, APPA, LOCK, PROB, REPA, LIGH,
CONT, LEVI, LEVC, DOOR, ACTI, STAT). See docs/FORMATS.md (DRPG).

    python3 rpg_compiler.py --data "/path/Morrowind/Data Files" --out build/cooked
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import struct
import sys
import time
import zlib
from typing import Dict, List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
import esm  # noqa: E402
import mwscript  # noqa: E402
from dmcommon import LOG, fnv1a32  # noqa: E402

NONE32 = 0xFFFFFFFF
NONE16 = 0xFFFF
DRPG_MAGIC = b"DRPG"
DRPG_VERSION = 1
TEXT_BLOCK = 32 * 1024

# ------------------------------------------------------------------ layout
HEADER_FMT = "<4sHHIII" + "II" * 16 + "II4x"   # see DRpgHeader in src/fmt.h
OBJECT_FMT = "<IIBBHIIIfi3IHHII6i"
ACTOR_FMT = "<hHHHii8H6H27sBBbBBBBxxI"
LIST_FMT = "<Ii"
EFFECT_FMT = "<hbbBxHHHHxx"
SPELL_FMT = "<IIBBHiiIHxxI"
FACTION_FMT = "<II10I2b7bBHI50h"
REACTION_FMT = "<Ii"
GLOBAL_FMT = "<IBxxxI"
GMST_FMT = "<IBxxxI"
SCRIPT_FMT = "<IIHHHHIHxxII"
VAR_FMT = "<IBxH"
TOPIC_FMT = "<IIBBxxII"
INFO_FMT = "<IIIIHBbi"
COND_FMT = "<BBBBIi"
BLOCK_FMT = "<IIII"

SIZES = {name: struct.calcsize(fmt) for name, fmt in (
    ("DRpgHeader", HEADER_FMT), ("DRpgObject", OBJECT_FMT), ("DRpgActor", ACTOR_FMT),
    ("DRpgListItem", LIST_FMT), ("DRpgEffect", EFFECT_FMT), ("DRpgSpell", SPELL_FMT),
    ("DRpgFaction", FACTION_FMT), ("DRpgReaction", REACTION_FMT), ("DRpgGlobal", GLOBAL_FMT),
    ("DRpgGmst", GMST_FMT), ("DRpgScript", SCRIPT_FMT), ("DRpgVar", VAR_FMT),
    ("DRpgTopic", TOPIC_FMT), ("DRpgInfo", INFO_FMT), ("DRpgCond", COND_FMT),
    ("DRpgBlock", BLOCK_FMT))}

# object types (DRpgObject.type)
OBJ_TYPES = ("NPC_", "CREA", "WEAP", "ARMO", "CLOT", "BOOK", "ALCH", "INGR", "MISC", "APPA",
             "LOCK", "PROB", "REPA", "LIGH", "CONT", "LEVI", "LEVC", "DOOR", "ACTI", "STAT",
             "CLAS", "RACE", "REGN")
OT = {t: i for i, t in enumerate(OBJ_TYPES)}
ITEM_TYPES = {"WEAP", "ARMO", "CLOT", "BOOK", "ALCH", "INGR", "MISC", "APPA", "LOCK", "PROB",
              "REPA", "LIGH"}

# DRpgObject.flags
OF_UNIQUE = 1 << 0      # exactly one placed reference (scripts key it by object id)
OF_FEMALE = 1 << 1
OF_ESSENTIAL = 1 << 2
OF_AUTOCALC = 1 << 3
OF_KEY = 1 << 4         # MISC key
OF_SCROLL = 1 << 5      # BOOK scroll
OF_CARRY = 1 << 6       # LIGH can be carried
OF_ORGANIC = 1 << 7     # CONT organic
OF_RESPAWN = 1 << 8
OF_PLAYABLE = 1 << 9    # CLAS/RACE playable
OF_BEAST = 1 << 10      # RACE beast
OF_ALL_LEVELS = 1 << 11  # LEVI/LEVC calculate from all levels <= PC
OF_EACH = 1 << 12       # LEVI calculate for each item
OF_AUTOCALC_ITEM = 1 << 13  # ALCH/ENCH autocalc

# topic types (DIAL DATA)
TT_TOPIC, TT_VOICE, TT_GREETING, TT_PERSUASION, TT_JOURNAL = range(5)

# INFO flags
IF_QUEST_NAME = 1 << 0
IF_QUEST_FINISHED = 1 << 1
IF_QUEST_RESTART = 1 << 2
IF_HAS_ACTOR = 1 << 3

# condition functions: 0..73 = SCVR function codes, then the variable kinds,
# then the INFO header filters (all tested in this order at run time)
CF_GLOBAL, CF_LOCAL, CF_JOURNAL, CF_ITEM, CF_DEAD, CF_NOT_ID, CF_NOT_FACTION, CF_NOT_CLASS, \
    CF_NOT_RACE, CF_NOT_CELL, CF_NOT_LOCAL = range(74, 85)
CF_ACTOR, CF_RACE, CF_CLASS, CF_FACTION, CF_FACTIONLESS, CF_RANK, CF_GENDER, CF_PCFACTION, \
    CF_PCRANK, CF_CELL = range(100, 110)
_VAR_KINDS = {"2": CF_GLOBAL, "3": CF_LOCAL, "4": CF_JOURNAL, "5": CF_ITEM, "6": CF_DEAD,
              "7": CF_NOT_ID, "8": CF_NOT_FACTION, "9": CF_NOT_CLASS, "A": CF_NOT_RACE,
              "B": CF_NOT_CELL, "C": CF_NOT_LOCAL}
# conditions whose 'hash' field is a name-pool offset (prefix compared)
_NAME_CONDS = {CF_NOT_CELL, CF_CELL}

GMST_INT, GMST_FLOAT, GMST_STRING = 0, 1, 2
VAR_TYPE = {"s": 0, "l": 1, "f": 2}


def zs(b: Optional[bytes]) -> str:
    return esm.zstr(b) if b is not None else ""


def _i32(d: Optional[bytes], ofs: int = 0, default: int = 0) -> int:
    if d is None or len(d) < ofs + 4:
        return default
    return struct.unpack_from("<i", d, ofs)[0]


def _f32(d: Optional[bytes], ofs: int = 0, default: float = 0.0) -> float:
    if d is None or len(d) < ofs + 4:
        return default
    return struct.unpack_from("<f", d, ofs)[0]


def fbits(v: float) -> int:
    return struct.unpack("<i", struct.pack("<f", v))[0]


# ------------------------------------------------------------- text helpers
_LINK = re.compile(r"@([^#]*)#")


def dialogue_text(raw: str) -> str:
    """INFO response -> display text (explicit @topic# links flattened)."""
    return mwscript.display_text(_LINK.sub(r"\1", raw)).strip()


def book_text(raw: str) -> str:
    """BOOK TEXT (Morrowind's HTML subset) -> plain text with line breaks."""
    s = raw.replace("\r", "").replace("\n", "")
    s = re.sub(r"(?i)<br\s*/?>", "\n", s)
    s = re.sub(r"(?i)<p\b[^>]*>", "\n\n", s)
    s = re.sub(r"(?i)<div\b[^>]*>", "\n", s)
    s = re.sub(r"(?i)<img\b[^>]*>", "", s)
    s = re.sub(r"<[^>]*>", "", s)
    s = html.unescape(s)
    s = mwscript.display_text(s)
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r" *\n *", "\n", s)
    s = re.sub(r"\n{3,}", "\n\n", s)
    return s.strip("\n ")


class NamePool(dmcommon.StringTable):
    def __init__(self) -> None:
        super().__init__()
        self.add("")

    def opt(self, s: str) -> int:
        return self.add(s) if s else NONE32


class BlobStore:
    """Packs byte strings into ~32 KB raw-DEFLATE blocks; id = block<<16 | ofs."""

    def __init__(self, block_size: int = TEXT_BLOCK) -> None:
        self.block_size = block_size
        self.blocks: List[bytearray] = [bytearray()]
        self.dedupe: Dict[bytes, int] = {}

    def add(self, data: bytes) -> int:
        if data in self.dedupe:
            return self.dedupe[data]
        cur = self.blocks[-1]
        if cur and (len(cur) + len(data) > self.block_size or len(cur) >= 0xFFFF):
            cur = bytearray()
            self.blocks.append(cur)
        if len(self.blocks) > 0xFFFF:
            raise ValueError("too many text blocks")
        sid = ((len(self.blocks) - 1) << 16) | len(cur)
        cur += data
        self.dedupe[data] = sid
        return sid

    def add_text(self, text: str) -> int:
        return self.add(text.encode("latin-1", errors="replace") + b"\0")

    @staticmethod
    def compress(raw: bytes) -> bytes:
        c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
        return c.compress(raw) + c.flush()


# ------------------------------------------------------------ INFO ordering
class InfoOrder:
    """Load-order merge of a topic's INFO list (PNAM/NNAM linked list)."""

    def __init__(self) -> None:
        self.order: List[str] = []
        self.items: Dict[str, Tuple[dict, bool]] = {}

    def insert(self, info: dict, deleted: bool) -> None:
        iid = info["id"]
        old = self.items.get(iid)
        if old is not None and old[0]["prev"] == info["prev"]:
            self.items[iid] = (info, deleted)
            return
        if old is not None:
            self.order.remove(iid)
        self.items[iid] = (info, deleted)
        if not info["prev"]:
            self.order.insert(0, iid)
        elif info["prev"] in self.items and info["prev"] in self.order:
            self.order.insert(self.order.index(info["prev"]) + 1, iid)
        elif info["next"] in self.items and info["next"] in self.order:
            self.order.insert(self.order.index(info["next"]), iid)
        else:
            self.order.append(iid)

    def infos(self) -> List[dict]:
        return [self.items[i][0] for i in self.order if not self.items[i][1]]


def parse_info(rec: esm.Record) -> Tuple[dict, bool]:
    info = {"id": zs(rec.get("INAM")), "prev": zs(rec.get("PNAM")), "next": zs(rec.get("NNAM")),
            "disp": 0, "rank": -1, "gender": -1, "pcrank": -1, "actor": "", "race": "",
            "class": "", "faction": "", "cell": "", "pcfaction": "", "sound": "", "text": "",
            "script": "", "flags": 0, "selects": []}
    pending = None
    for n, d in rec.subs:
        if n == "DATA" and len(d) >= 12:
            info["disp"] = struct.unpack_from("<i", d, 4)[0]
            info["rank"], info["gender"], info["pcrank"] = struct.unpack_from("<bbb", d, 8)
        elif n == "ONAM":
            info["actor"] = zs(d)
        elif n == "RNAM":
            info["race"] = zs(d)
        elif n == "CNAM":
            info["class"] = zs(d)
        elif n == "FNAM":
            info["faction"] = zs(d)
        elif n == "ANAM":
            info["cell"] = zs(d)
        elif n == "DNAM":
            info["pcfaction"] = zs(d)
        elif n == "SNAM":
            info["sound"] = zs(d)
        elif n == "NAME":
            info["text"] = zs(d)
        elif n == "SCVR":
            pending = zs(d)
            info["selects"].append([pending, 0])
        elif n == "INTV" and pending is not None:
            info["selects"][-1][1] = struct.unpack_from("<i", d, 0)[0] if len(d) >= 4 else 0
            pending = None
        elif n == "FLTV" and pending is not None:
            info["selects"][-1][1] = struct.unpack_from("<f", d, 0)[0] if len(d) >= 4 else 0.0
            pending = None
        elif n == "BNAM":
            info["script"] = zs(d)
        elif n == "QSTN":
            info["flags"] |= IF_QUEST_NAME
        elif n == "QSTF":
            info["flags"] |= IF_QUEST_FINISHED
        elif n == "QSTR":
            info["flags"] |= IF_QUEST_RESTART
    return info, rec.has("DELE")


def parse_select(rule: str, value) -> Optional[Tuple[int, int, int, str, object]]:
    """SCVR rule -> (index, function, comparison, variable, value) or None."""
    if len(rule) < 5 or not "0" <= rule[4] <= "5":
        return None
    index = ord(rule[0]) - 48 if rule[0].isdigit() else 0
    kind = rule[1]
    if kind == "1":
        try:
            func = int(rule[2:4])
        except ValueError:
            return None
        if not 0 <= func <= 73:
            return None
    elif kind in _VAR_KINDS:
        if len(rule) == 5:
            return None
        func = _VAR_KINDS[kind]
    else:
        return None
    return index, func, ord(rule[4]) - 48, rule[5:], value


# ----------------------------------------------------------------- loading
class GameData:
    """All RPG records of a load order, merged."""

    def __init__(self) -> None:
        self.objects: Dict[str, Tuple[str, esm.Record]] = {}
        self.globals: Dict[str, Tuple[str, str, float]] = {}
        self.gmsts: Dict[str, Tuple[str, object]] = {}
        self.scripts: Dict[str, Tuple[str, str]] = {}
        self.start_scripts: List[str] = []
        self.dialogues: Dict[str, dict] = {}
        self.factions: Dict[str, esm.Record] = {}
        self.spells: Dict[str, esm.Record] = {}
        self.enchants: Dict[str, esm.Record] = {}
        self.ref_counts: Dict[str, int] = {}
        self.files: List[str] = []

    def load(self, paths: Sequence[str]) -> "GameData":
        for p in paths:
            self.load_bytes(dmcommon.read_file(p), os.path.basename(p))
        return self

    def load_bytes(self, data: bytes, filename: str) -> None:
        self.files.append(filename)
        current: Optional[dict] = None
        for rec in esm.iter_records(data):
            t = rec.type
            if t != "INFO":
                current = None
            if t in OBJ_TYPES:
                oid = zs(rec.get("NAME"))
                if not oid:
                    continue
                if rec.has("DELE"):
                    self.objects.pop(oid.lower(), None)
                else:
                    self.objects[oid.lower()] = (t, rec)
            elif t == "GLOB":
                gid = zs(rec.get("NAME"))
                if rec.has("DELE"):
                    self.globals.pop(gid.lower(), None)
                    continue
                gtype = chr(rec.get("FNAM")[0]) if rec.get("FNAM") else "f"
                if gtype not in "slf":
                    gtype = "f"
                self.globals[gid.lower()] = (gid, gtype, _f32(rec.get("FLTV")))
            elif t == "GMST":
                gid = zs(rec.get("NAME"))
                if not gid:
                    continue
                if rec.get("STRV") is not None:
                    v: object = zs(rec.get("STRV"))
                    kind = "s"
                elif rec.get("INTV") is not None:
                    v, kind = _i32(rec.get("INTV")), "i"
                elif rec.get("FLTV") is not None:
                    v, kind = _f32(rec.get("FLTV")), "f"
                else:
                    kind = gid[:1].lower()
                    v = "" if kind == "s" else 0
                self.gmsts[gid.lower()] = (gid, v if kind != "s" else str(v))
            elif t == "SCPT":
                hdr = rec.get("SCHD")
                sid = esm.zstr(hdr[:32]) if hdr else ""
                if not sid:
                    continue
                if rec.has("DELE"):
                    self.scripts.pop(sid.lower(), None)
                    continue
                self.scripts[sid.lower()] = (sid, zs(rec.get("SCTX")))
            elif t == "SSCR":
                name = zs(rec.get("NAME"))
                if name and name.lower() not in (s.lower() for s in self.start_scripts):
                    self.start_scripts.append(name)
            elif t == "DIAL":
                did = zs(rec.get("NAME"))
                if not did:
                    continue
                if rec.has("DELE"):
                    self.dialogues.pop(did.lower(), None)
                    continue
                dtype = rec.get("DATA")[0] if rec.get("DATA") else TT_TOPIC
                dial = self.dialogues.get(did.lower())
                if dial is None or dial["type"] != dtype:
                    dial = {"id": did, "type": dtype, "order": InfoOrder()}
                    self.dialogues[did.lower()] = dial
                current = dial
            elif t == "INFO":
                if current is None:
                    continue
                info, deleted = parse_info(rec)
                current["order"].insert(info, deleted)
            elif t == "FACT":
                self._keyed(self.factions, rec)
            elif t == "SPEL":
                self._keyed(self.spells, rec)
            elif t == "ENCH":
                self._keyed(self.enchants, rec)
            elif t == "CELL":
                for n, d in rec.subs:
                    if n == "NAME" and self._in_ref:
                        key = zs(d).lower()
                        self.ref_counts[key] = self.ref_counts.get(key, 0) + 1
                        self._in_ref = False
                    elif n == "FRMR":
                        self._in_ref = True
                self._in_ref = False

    _in_ref = False

    @staticmethod
    def _keyed(table: Dict[str, esm.Record], rec: esm.Record) -> None:
        rid = zs(rec.get("NAME"))
        if not rid:
            return
        if rec.has("DELE"):
            table.pop(rid.lower(), None)
        else:
            table[rid.lower()] = rec


# ------------------------------------------------------------------ build
class RpgBuilder:
    def __init__(self, gd: GameData) -> None:
        self.gd = gd
        self.names = NamePool()
        self.blobs = BlobStore()
        self.report: Dict[str, object] = {}
        self.warnings: List[str] = []

    # ---------------------------------------------------------- scripts
    def compile_scripts(self) -> None:
        gd = self.gd
        sources = {sid: src for sid, src in gd.scripts.values()}
        object_scripts = {}
        for key, (_t, rec) in gd.objects.items():
            s = zs(rec.get("SCRI"))
            if s:
                object_scripts[key] = s
        globals_ = {gid: gt for gid, gt, _v in gd.globals.values()}
        self.ctx = mwscript.make_context(sources, globals_, list(gd.objects), object_scripts)
        self.compiled, errors = mwscript.compile_all(sources, self.ctx)
        for key, err in sorted(errors.items()):
            self.warnings.append(f"script {key}: {err}")
        nwarn = sum(len(c.warnings) for c in self.compiled.values())
        self.report["scripts"] = len(sources)
        self.report["scripts_compiled"] = len(self.compiled)
        self.report["script_errors"] = errors
        self.report["script_warnings"] = nwarn
        # keep the failing ones as empty programs so ids still resolve
        for key in errors:
            loc = self.ctx.scripts.get(key, mwscript.Locals())
            self.compiled[key] = mwscript.Compiled(gd.scripts[key][0], loc, bytes([mwscript.OP_RET]),
                                                   b"", 0, [])
        self.script_order = sorted(self.compiled, key=fnv1a32)
        self.script_index = {k: i for i, k in enumerate(self.script_order)}
        if len(self.script_order) >= NONE16:
            raise ValueError("too many scripts")

    def script_ref(self, name: str) -> int:
        return self.script_index.get(name.lower(), NONE16) if name else NONE16

    # ---------------------------------------------------------- objects
    def build_objects(self) -> None:
        gd = self.gd
        self.lists: List[Tuple[int, int]] = []
        self.effects: List[tuple] = []
        self.actors: List[tuple] = []
        objs = []
        for key, (t, rec) in gd.objects.items():
            objs.append(self._object(key, t, rec))
        player = gd.objects.get("player")
        if player is None:
            self.warnings.append("no 'player' NPC record; using defaults")
        self.objects = sorted(objs, key=lambda o: o[0])
        for a, b in zip(self.objects, self.objects[1:]):
            if a[0] == b[0]:
                raise ValueError(f"object id hash collision ({a[0]:08x})")
        self.object_index = {o[0]: i for i, o in enumerate(self.objects)}
        self.report["objects"] = len(self.objects)

    def _list(self, items: Sequence[Tuple[str, int]]) -> Tuple[int, int]:
        first = len(self.lists)
        for iid, count in items:
            self.lists.append((fnv1a32(iid), count))
        return first, len(items)

    def _effects(self, rec: esm.Record) -> Tuple[int, int]:
        first = len(self.effects)
        for n, d in rec.subs:
            if n == "ENAM" and len(d) >= 24:
                eff, skill, attr, rng, area, dur, mn, mx = struct.unpack_from("<hbbiiiii", d, 0)
                self.effects.append((eff, skill, attr, rng & 0xFF, _u16(area), _u16(dur),
                                     _u16(mn), _u16(mx)))
        return first, len(self.effects) - first

    def _inventory(self, rec: esm.Record) -> List[Tuple[str, int]]:
        out = []
        for n, d in rec.subs:
            if n in ("NPCO", "CNTO") and len(d) >= 36:
                out.append((esm.zstr(d[4:36]), struct.unpack_from("<i", d, 0)[0]))
        return out

    def _spell_list(self, rec: esm.Record) -> List[Tuple[str, int]]:
        return [(zs(d), 0) for n, d in rec.subs if n == "NPCS"]

    def _object(self, key: str, t: str, rec: esm.Record) -> tuple:
        name = zs(rec.get("FNAM"))
        icon = zs(rec.get("ITEX"))
        model = zs(rec.get("MODL"))
        flags = OF_UNIQUE if self.gd.ref_counts.get(key, 0) == 1 else 0
        weight, value = 0.0, 0
        link = [0, 0, 0]
        stat = [0] * 6
        l1 = (0, 0)
        l2 = (0, 0)
        script = self.script_ref(zs(rec.get("SCRI")))
        sex = 0
        if t == "NPC_":
            npc_flags = _i32(rec.get("FLAG"))
            if npc_flags & 0x01:
                flags |= OF_FEMALE
                sex = 1
            if npc_flags & 0x02:
                flags |= OF_ESSENTIAL
            if npc_flags & 0x04:
                flags |= OF_RESPAWN
            if npc_flags & 0x10:
                flags |= OF_AUTOCALC
            link = [_h(zs(rec.get("RNAM"))), _h(zs(rec.get("CNAM"))), _h(zs(rec.get("ANAM")))]
            stat[0] = self._npc_actor(rec)
            l1 = self._list(self._inventory(rec))
            l2 = self._list(self._spell_list(rec))
        elif t == "CREA":
            cflags = _i32(rec.get("FLAG"))
            if cflags & 0x02:
                flags |= OF_ESSENTIAL
            if cflags & 0x04:
                flags |= OF_RESPAWN
            stat[0] = self._creature_actor(rec)
            stat[1] = _i32(rec.get("NPDT"), 0)  # creature type
            stat[2] = cflags
            l1 = self._list(self._inventory(rec))
            l2 = self._list(self._spell_list(rec))
        elif t == "WEAP":
            d = rec.get("WPDT") or bytes(32)
            if len(d) >= 32:
                w, v, wtype, health, speed, reach, ench = struct.unpack_from("<fihHffH", d, 0)
                dmg = d[22:28]
                wflags = struct.unpack_from("<i", d, 28)[0]
                weight, value = w, v
                stat[:5] = [wtype, health, fbits(speed), fbits(reach), ench]
                stat[5] = wflags
                link = [_h(zs(rec.get("ENAM"))), dmg[0] | dmg[1] << 8 | dmg[2] << 16 | dmg[3] << 24,
                        dmg[4] | dmg[5] << 8]
        elif t == "ARMO":
            d = rec.get("AODT")
            if d and len(d) >= 24:
                atype, w, v, health, ench, rating = struct.unpack_from("<ifiiii", d, 0)
                weight, value = w, v
                stat[:4] = [atype, health, ench, rating]
            link[0] = _h(zs(rec.get("ENAM")))
        elif t == "CLOT":
            d = rec.get("CTDT")
            if d and len(d) >= 12:
                ctype, w, v, ench = struct.unpack_from("<ifhh", d, 0)
                weight, value = w, v
                stat[0], stat[2] = ctype, ench
            link[0] = _h(zs(rec.get("ENAM")))
        elif t == "BOOK":
            d = rec.get("BKDT")
            if d and len(d) >= 20:
                w, v, scroll, skill, ench = struct.unpack_from("<fiiii", d, 0)
                weight, value = w, v
                if scroll:
                    flags |= OF_SCROLL
                stat[0], stat[2] = skill, ench
            link[0] = _h(zs(rec.get("ENAM")))
            link[1] = self.blobs.add_text(book_text(zs(rec.get("TEXT"))))
        elif t == "ALCH":
            d = rec.get("ALDT")
            if d and len(d) >= 12:
                weight, value, auto = struct.unpack_from("<fii", d, 0)
                if auto:
                    flags |= OF_AUTOCALC_ITEM
            l2 = self._effects(rec)
        elif t == "INGR":
            d = rec.get("IRDT")
            first = len(self.effects)
            if d and len(d) >= 56:
                weight, value = struct.unpack_from("<fi", d, 0)
                effs = struct.unpack_from("<4i", d, 8)
                skills = struct.unpack_from("<4i", d, 24)
                attrs = struct.unpack_from("<4i", d, 40)
                for e, s, a in zip(effs, skills, attrs):
                    if e >= 0:
                        self.effects.append((e, s, a, 0, 0, 0, 0, 0))
            l2 = (first, len(self.effects) - first)
        elif t == "MISC":
            d = rec.get("MCDT")
            if d and len(d) >= 12:
                weight, value, mflags = struct.unpack_from("<fii", d, 0)
                if mflags & 1:
                    flags |= OF_KEY
        elif t in ("LOCK", "PROB", "REPA"):
            d = rec.get({"LOCK": "LKDT", "PROB": "PBDT", "REPA": "RIDT"}[t])
            if d and len(d) >= 16:
                if t == "REPA":
                    weight, value, uses, quality = struct.unpack_from("<fiif", d, 0)
                else:
                    weight, value, quality, uses = struct.unpack_from("<fifi", d, 0)
                stat[0], stat[1] = uses, fbits(quality)
        elif t == "APPA":
            d = rec.get("AADT")
            if d and len(d) >= 16:
                atype, quality, weight, value = struct.unpack_from("<iffi", d, 0)
                stat[0], stat[1] = atype, fbits(quality)
        elif t == "LIGH":
            d = rec.get("LHDT")
            if d and len(d) >= 24:
                weight, value, ltime, radius, color, lflags = struct.unpack_from("<fiiiIi", d, 0)
                stat[:3] = [ltime, radius, color & 0x7FFFFFFF]
                stat[3] = lflags
                if lflags & 0x02:
                    flags |= OF_CARRY
        elif t == "CONT":
            stat[0] = fbits(_f32(rec.get("CNDT")))
            cflags = _i32(rec.get("FLAG"))
            if cflags & 1:
                flags |= OF_ORGANIC
            if cflags & 2:
                flags |= OF_RESPAWN
            stat[1] = cflags
            l1 = self._list(self._inventory(rec))
        elif t in ("LEVI", "LEVC"):
            lflags = _i32(rec.get("DATA"))
            if lflags & 1:
                flags |= OF_ALL_LEVELS if t == "LEVC" else OF_EACH
            if lflags & 2:
                flags |= OF_ALL_LEVELS
            stat[0] = rec.get("NNAM")[0] if rec.get("NNAM") else 0
            entries = []
            ename = None
            for n, d in rec.subs:
                if n in ("INAM", "CNAM"):
                    ename = zs(d)
                elif n == "INTV" and ename is not None:
                    entries.append((ename, struct.unpack_from("<h", d, 0)[0] if len(d) >= 2 else 1))
                    ename = None
            l1 = self._list(entries)
        elif t == "CLAS":
            d = rec.get("CLDT")
            if d and len(d) >= 60:
                vals = struct.unpack_from("<15i", d, 0)
                # attributes[2], specialization, (minor, major) x5, playable, services
                stat[0] = vals[0] | (vals[1] << 8) | (vals[2] << 16)
                skills = vals[3:13]
                stat[1] = sum((s & 0xFF) << (8 * i) for i, s in enumerate(skills[0:4]))
                stat[2] = sum((s & 0xFF) << (8 * i) for i, s in enumerate(skills[4:8]))
                stat[3] = sum((s & 0xFF) << (8 * i) for i, s in enumerate(skills[8:10]))
                if vals[13]:
                    flags |= OF_PLAYABLE
                stat[4] = vals[14]
        elif t == "RACE":
            d = rec.get("RADT")
            if d and len(d) >= 140:
                rflags = struct.unpack_from("<i", d, 136)[0]
                if rflags & 1:
                    flags |= OF_PLAYABLE
                if rflags & 2:
                    flags |= OF_BEAST
                hm, hf, wm, wf = struct.unpack_from("<4f", d, 120)
                stat[:4] = [fbits(hm), fbits(hf), fbits(wm), fbits(wf)]
        elif t == "REGN":
            pass
        return (fnv1a32(key), self.names.opt(mwscript.display_text(name)), OT[t], sex, script, flags,
                _h(icon_key(icon)) if icon else 0, _h(model_key(model)) if model else 0,
                weight, value, link, l1, l2, [s32(v) for v in stat])

    def _npc_actor(self, rec: esm.Record) -> int:
        d = rec.get("NPDT") or b""
        level, health, magicka, fatigue, gold = 1, 0, 0, 0, 0
        attrs = [0] * 8
        skills = b"\0" * 27
        disp = rep = 0
        rank = 0
        if len(d) >= 52:
            level = struct.unpack_from("<h", d, 0)[0]
            attrs = list(d[2:10])
            skills = bytes(d[10:37])
            health, magicka, fatigue = struct.unpack_from("<HHH", d, 38)
            disp, rep, rank = d[44], d[45], struct.unpack_from("<b", d, 46)[0]
            gold = struct.unpack_from("<i", d, 48)[0]
        elif len(d) >= 12:
            level = struct.unpack_from("<h", d, 0)[0]
            disp, rep, rank = d[2], d[3], struct.unpack_from("<b", d, 4)[0]
            gold = struct.unpack_from("<i", d, 8)[0]
        hello, fight, flee, alarm, services = _aidt(rec.get("AIDT"))
        return self._actor(level, health, magicka, fatigue, gold, 0, attrs, [0] * 6, skills,
                           disp, rep, rank, hello, fight, flee, alarm, services)

    def _creature_actor(self, rec: esm.Record) -> int:
        d = rec.get("NPDT") or b""
        if len(d) >= 96:
            v = struct.unpack_from("<24i", d, 0)
            level = v[1]
            attrs = list(v[2:10])
            health, magicka, fatigue, soul = v[10:14]
            combat, magic, stealth = v[14:17]
            attack = list(v[17:23])
            gold = v[23]
        else:
            level, attrs, health, magicka, fatigue, soul = 1, [0] * 8, 0, 0, 0, 0
            combat = magic = stealth = 0
            attack = [0] * 6
            gold = 0
        skills = bytes([min(255, max(0, combat)), min(255, max(0, magic)),
                        min(255, max(0, stealth))]) + b"\0" * 24
        hello, fight, flee, alarm, services = _aidt(rec.get("AIDT"))
        return self._actor(level, health, magicka, fatigue, gold, soul, attrs, attack, skills,
                           0, 0, -1, hello, fight, flee, alarm, services)

    def _actor(self, level, health, magicka, fatigue, gold, soul, attrs, attack, skills, disp, rep,
               rank, hello, fight, flee, alarm, services) -> int:
        self.actors.append((max(-32768, min(32767, level)), _u16(health), _u16(magicka),
                            _u16(fatigue), gold, soul, *[_u16(a) for a in attrs],
                            *[_u16(a) for a in attack], bytes(skills), disp & 0xFF, rep & 0xFF,
                            max(-128, min(127, rank)), hello & 0xFF, fight & 0xFF, flee & 0xFF,
                            alarm & 0xFF, services & 0xFFFFFFFF))
        return len(self.actors) - 1

    # ---------------------------------------------------------- spells
    def build_spells(self) -> None:
        out = []
        for key, rec in self.gd.spells.items():
            d = rec.get("SPDT") or bytes(12)
            stype, cost, sflags = struct.unpack_from("<iii", d, 0) if len(d) >= 12 else (0, 0, 0)
            first, count = self._effects(rec)
            out.append((fnv1a32(key), self.names.opt(mwscript.display_text(zs(rec.get("FNAM")))), 0,
                        stype & 0xFF, sflags & 0xFFFF, cost, 0, first, count, 0))
        for key, rec in self.gd.enchants.items():
            d = rec.get("ENDT") or bytes(16)
            etype, cost, charge, auto = struct.unpack_from("<iiii", d, 0) if len(d) >= 16 else (0, 0, 0, 0)
            first, count = self._effects(rec)
            out.append((fnv1a32(key), NONE32, 1, etype & 0xFF, 1 if auto else 0, cost, charge, first,
                        count, 0))
        self.spells = sorted(out, key=lambda s: s[0])
        self.report["spells"] = len(self.gd.spells)
        self.report["enchantments"] = len(self.gd.enchants)

    # ---------------------------------------------------------- factions
    def build_factions(self) -> None:
        self.reactions: List[Tuple[int, int]] = []
        out = []
        for key, rec in self.gd.factions.items():
            ranks = [zs(d) for n, d in rec.subs if n == "RNAM"][:10]
            rank_ofs = [self.names.opt(mwscript.display_text(r)) for r in ranks]
            rank_ofs += [NONE32] * (10 - len(rank_ofs))
            d = rec.get("FADT") or b""
            attrs = [0, 0]
            skills = [-1] * 7
            hidden = 0
            req = [0] * 50
            if len(d) >= 240:
                attrs = list(struct.unpack_from("<2i", d, 0))
                vals = struct.unpack_from("<50i", d, 8)
                req = [max(-32768, min(32767, v)) for v in vals]
                skills = list(struct.unpack_from("<7i", d, 208))
                hidden = struct.unpack_from("<i", d, 236)[0] & 1
            first = len(self.reactions)
            fid = None
            for n, dd in rec.subs:
                if n == "ANAM":
                    fid = zs(dd)
                elif n == "INTV" and fid is not None:
                    self.reactions.append((fnv1a32(fid), struct.unpack_from("<i", dd, 0)[0]))
                    fid = None
            out.append((fnv1a32(key), self.names.opt(mwscript.display_text(zs(rec.get("FNAM")))),
                        *rank_ofs, *[max(-128, min(127, a)) for a in attrs],
                        *[max(-128, min(127, s)) for s in skills], hidden,
                        len(self.reactions) - first, first, *req))
        self.factions = sorted(out, key=lambda f: f[0])
        self.report["factions"] = len(out)

    # ---------------------------------------------------------- dialogue
    def build_dialogue(self) -> None:
        topics = sorted(self.gd.dialogues.values(), key=lambda d: d["id"].lower())
        self.topics = []
        self.infos = []
        self.conds = []
        n_journal = 0
        snippet_warnings: List[str] = []
        self.snippet_failures = 0
        for dial in topics:
            infos = dial["order"].infos()
            first = len(self.infos)
            for info in infos:
                self.infos.append(self._info(dial, info, snippet_warnings))
                if dial["type"] == TT_JOURNAL and info["flags"] & IF_QUEST_NAME:
                    n_journal += 1
            self.topics.append((fnv1a32(dial["id"]), self.names.add(mwscript.display_text(dial["id"])),
                                dial["type"], 0, first, len(self.infos) - first))
        self.topic_hash_index = sorted(range(len(self.topics)), key=lambda i: self.topics[i][0])
        hashes = [self.topics[i][0] for i in self.topic_hash_index]
        if len(set(hashes)) != len(hashes):
            raise ValueError("dialogue topic hash collision")
        self.report["topics"] = sum(1 for d in topics if d["type"] == TT_TOPIC)
        self.report["dialogues"] = len(topics)
        self.report["infos"] = len(self.infos)
        self.report["conditions"] = len(self.conds)
        self.report["quests"] = n_journal
        self.report["result_script_warnings"] = len(snippet_warnings)
        self.report["result_script_failures"] = self.snippet_failures

    def _info(self, dial: dict, info: dict, warnings: List[str]) -> tuple:
        first = len(self.conds)
        flags = info["flags"] & (IF_QUEST_NAME | IF_QUEST_FINISHED | IF_QUEST_RESTART)
        if info["actor"]:
            flags |= IF_HAS_ACTOR
            self._cond(CF_ACTOR, 0, _h(info["actor"]), 0)
        if info["race"]:
            self._cond(CF_RACE, 0, _h(info["race"]), 0)
        if info["class"]:
            self._cond(CF_CLASS, 0, _h(info["class"]), 0)
        if info["faction"].upper() == "FFFF":
            self._cond(CF_FACTIONLESS, 0, 0, 0)
        elif info["faction"]:
            self._cond(CF_FACTION, 0, _h(info["faction"]), info["rank"])
        elif info["rank"] != -1:
            self._cond(CF_RANK, 0, 0, info["rank"])
        if info["gender"] != -1 and dial["type"] != TT_JOURNAL:
            self._cond(CF_GENDER, 0, 0, info["gender"])
        if info["pcfaction"]:
            self._cond(CF_PCFACTION, 0, _h(info["pcfaction"]), info["pcrank"])
        elif info["pcrank"] != -1:
            self._cond(CF_PCRANK, 0, 0, info["pcrank"])
        if info["cell"]:
            self._cond(CF_CELL, 0, self.names.add(info["cell"].lower()), 0)
        parsed = []
        for rule, value in info["selects"]:
            p = parse_select(rule, value)
            if p is None:
                self.warnings.append(f"INFO {info['id']}: bad SCVR {rule!r} ignored")
                continue
            parsed.append(p)
        parsed.sort(key=lambda p: p[0])
        for _idx, func, comp, var, value in parsed:
            if func in _NAME_CONDS:
                key = self.names.add(var.lower())
            elif func in (CF_LOCAL, CF_NOT_LOCAL, CF_GLOBAL):
                key = fnv1a32(var.lower())
            else:
                key = fnv1a32(var) if var else 0
            self._cond(func, comp, key, value)
        text = dialogue_text(info["text"])
        text_sid = self.blobs.add_text(text)
        script_sid = NONE32
        if info["script"].strip():
            try:
                comp = mwscript.snippet_or_none(info["script"], self.ctx, warnings)
            except mwscript.CompileError as e:
                comp = None
                self.snippet_failures += 1
                self.warnings.append(f"INFO {info['id']}: result script: {e}")
            if comp is not None:
                script_sid = self.blobs.add(comp.snippet_blob())
        sound = _h(info["sound"].lower().replace("\\", "/")) if info["sound"] else 0
        return (text_sid, script_sid, sound, first, len(self.conds) - first, flags, 0, info["disp"])

    def _cond(self, func: int, comp: int, key: int, value) -> None:
        if isinstance(value, float):
            self.conds.append((func, comp, 1, 0, key & 0xFFFFFFFF, fbits(value)))
        else:
            v = max(-0x80000000, min(0x7FFFFFFF, int(value)))
            self.conds.append((func, comp, 0, 0, key & 0xFFFFFFFF, v))

    # ---------------------------------------------------------- globals
    def build_globals(self) -> None:
        rows = []
        for key, (gid, gtype, value) in self.gd.globals.items():
            bits = fbits(value) if gtype == "f" else int(value)
            if gtype == "s":
                bits = max(-32768, min(32767, bits))
            rows.append((fnv1a32(key), VAR_TYPE[gtype], bits & 0xFFFFFFFF))
        rows.sort(key=lambda r: r[0])
        expected = [mwscript.id_hash(k) for k in sorted(self.ctx.globals, key=mwscript.id_hash)]
        if [r[0] for r in rows] != expected:
            raise ValueError("global index mismatch between compiler and table")
        self.globals = rows
        gm = []
        for key, (_gid, value) in self.gd.gmsts.items():
            if isinstance(value, str):
                gm.append((fnv1a32(key), GMST_STRING, self.names.add(mwscript.display_text(value))))
            elif isinstance(value, float):
                gm.append((fnv1a32(key), GMST_FLOAT, fbits(value) & 0xFFFFFFFF))
            else:
                gm.append((fnv1a32(key), GMST_INT, int(value) & 0xFFFFFFFF))
        self.gmsts = sorted(gm, key=lambda r: r[0])
        self.report["globals"] = len(rows)
        self.report["gmsts"] = len(gm)

    # ---------------------------------------------------------- output
    def build(self) -> Dict[str, bytes]:
        self.compile_scripts()
        self.build_globals()
        self.build_objects()
        self.build_spells()
        self.build_factions()
        self.build_dialogue()
        entries: Dict[str, bytes] = {}
        scripts = []
        variables = []
        for key in self.script_order:
            c = self.compiled[key]
            vars_ = sorted(((fnv1a32(n), VAR_TYPE[t], slot) for n, t, slot in c.locals.table()),
                           key=lambda v: v[0])
            scripts.append((fnv1a32(key), self.names.add(c.name or key), len(c.locals.shorts),
                            len(c.locals.longs), len(c.locals.floats), c.flags, len(variables),
                            len(vars_), len(c.code), len(c.strings)))
            variables.extend(vars_)
            entries["script/" + key] = c.code + c.strings
        starts = [self.script_index[s.lower()] for s in self.gd.start_scripts
                  if s.lower() in self.script_index]
        blocks = []
        packed_total = 0
        raw_total = 0
        raw_blocks = self.blobs.blocks if self.blobs.blocks[0] else []
        for i, raw in enumerate(raw_blocks):
            packed = BlobStore.compress(bytes(raw))
            name = f"text/{i}"
            entries[name] = packed
            blocks.append((fnv1a32(name), len(raw), len(packed), 0))
            packed_total += len(packed)
            raw_total += len(raw)
        player_index = self.object_index.get(fnv1a32("player"), NONE32)
        entries["rpg/core"] = self._core(player_index, scripts, variables, starts, blocks)
        self.report["text_raw_bytes"] = raw_total
        self.report["text_packed_bytes"] = packed_total
        self.report["core_bytes"] = len(entries["rpg/core"])
        self.report["script_bytes"] = sum(len(self.compiled[k].code) + len(self.compiled[k].strings)
                                          for k in self.script_order)
        return entries

    def _core(self, player_index, scripts, variables, starts, blocks) -> bytes:
        w = dmcommon.BinWriter()
        w.reserve(SIZES["DRpgHeader"])
        tables = []

        def table(fmt: str, rows) -> Tuple[int, int]:
            w.align(16)
            ofs = w.tell()
            for r in rows:
                w.bytes(struct.pack(fmt, *r))
            return ofs, len(rows)

        obj_rows = [(h, name, t, sex, script, flags, icon, model, weight, value, *link, l1[1], l2[1],
                     l1[0], l2[0], *stat)
                    for (h, name, t, sex, script, flags, icon, model, weight, value, link, l1, l2,
                         stat) in self.objects]
        tables.append(table(OBJECT_FMT, obj_rows))
        tables.append(table(ACTOR_FMT, self.actors))
        tables.append(table(LIST_FMT, self.lists))
        tables.append(table(EFFECT_FMT, self.effects))
        tables.append(table(SPELL_FMT, self.spells))
        tables.append(table(FACTION_FMT, self.factions))
        tables.append(table(REACTION_FMT, self.reactions))
        tables.append(table(GLOBAL_FMT, self.globals))
        tables.append(table(GMST_FMT, self.gmsts))
        tables.append(table(SCRIPT_FMT, scripts))
        tables.append(table(VAR_FMT, variables))
        tables.append(table("<H", [(s,) for s in starts]))
        tables.append(table(TOPIC_FMT, self.topics))
        topic_hash_ofs, _ = table("<I", [(i,) for i in self.topic_hash_index])
        tables.append(table(INFO_FMT, self.infos))
        tables.append(table(COND_FMT, self.conds))
        tables.append(table(BLOCK_FMT, blocks))
        w.align(16)
        names_ofs = w.bytes(bytes(self.names.data))
        w.align(16)
        size = w.tell()
        flat = [v for pair in tables for v in pair]
        w.patch(0, HEADER_FMT[1:], DRPG_MAGIC, DRPG_VERSION, 0, player_index, names_ofs,
                len(self.names.data), *flat, topic_hash_ofs, size)
        return w.getvalue()


def _u16(v: int) -> int:
    return max(0, min(0xFFFF, int(v)))


def _h(s: str) -> int:
    return fnv1a32(s) if s else 0


def _aidt(d: Optional[bytes]) -> Tuple[int, int, int, int, int]:
    if d is None or len(d) < 12:
        return 30, 0, 30, 0, 0
    hello, _u1, fight, flee, alarm = d[0], d[1], d[2], d[3], d[4]
    services = struct.unpack_from("<I", d, 8)[0]
    return hello, fight, flee, alarm, services


def icon_key(path: str) -> str:
    return dmcommon.texture_key("icons/" + path)


def model_key(path: str) -> str:
    return dmcommon.mesh_key(path)


def s32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >= (1 << 31) else v


# ------------------------------------------------------------------ reader
def read_rpg(core: bytes) -> dict:
    """Parses rpg/core back into Python lists (tests and tools)."""
    vals = struct.unpack_from(HEADER_FMT, core, 0)
    magic, version, _flags, player, names_ofs, names_size = vals[:6]
    if magic != DRPG_MAGIC or version != DRPG_VERSION:
        raise ValueError("not a DRPG core")
    pairs = vals[6:6 + 32]
    topic_hash_ofs, size = vals[38], vals[39]
    if size != len(core):
        raise ValueError("DRPG size mismatch")
    names = core[names_ofs:names_ofs + names_size]

    def name(ofs: int) -> Optional[str]:
        if ofs == NONE32:
            return None
        end = names.find(b"\0", ofs)
        return names[ofs:end].decode("latin-1")

    fmts = [OBJECT_FMT, ACTOR_FMT, LIST_FMT, EFFECT_FMT, SPELL_FMT, FACTION_FMT, REACTION_FMT,
            GLOBAL_FMT, GMST_FMT, SCRIPT_FMT, VAR_FMT, "<H", TOPIC_FMT, INFO_FMT, COND_FMT, BLOCK_FMT]
    keys = ["objects", "actors", "lists", "effects", "spells", "factions", "reactions", "globals",
            "gmsts", "scripts", "vars", "starts", "topics", "infos", "conds", "blocks"]
    out = {"player": player, "name": name}
    for i, (key, fmt) in enumerate(zip(keys, fmts)):
        ofs, count = pairs[2 * i], pairs[2 * i + 1]
        sz = struct.calcsize(fmt)
        out[key] = [struct.unpack_from(fmt, core, ofs + k * sz) for k in range(count)]
    out["topic_hash_index"] = [struct.unpack_from("<I", core, topic_hash_ofs + 4 * k)[0]
                               for k in range(len(out["topics"]))]
    return out


def read_blob(pack: dmcommon.DPakReader, core: dict, sid: int) -> bytes:
    block = sid >> 16
    ofs = sid & 0xFFFF
    raw = zlib.decompress(pack.get(f"text/{block}"), -15)
    if len(raw) != core["blocks"][block][1]:
        raise ValueError("text block size mismatch")
    return raw[ofs:]


def read_text(pack: dmcommon.DPakReader, core: dict, sid: int) -> str:
    data = read_blob(pack, core, sid)
    return data[:data.index(b"\0")].decode("latin-1")


# ------------------------------------------------------------------ driver
def cook_rpg(content: Sequence[str], out_dir: str) -> dict:
    t0 = time.time()
    gd = GameData().load(content)
    builder = RpgBuilder(gd)
    entries = builder.build()
    pak = dmcommon.DPakWriter(dmcommon.DPAK_KIND_GENERIC)
    for name, data in entries.items():
        pak.add(name, data)
    os.makedirs(out_dir, exist_ok=True)
    size = pak.write(os.path.join(out_dir, "rpg.dpak"))
    report = dict(builder.report)
    report["pack_bytes"] = size
    report["warnings"] = builder.warnings[:200]
    report["warning_count"] = len(builder.warnings)
    report["seconds"] = round(time.time() - t0, 2)
    with open(os.path.join(out_dir, "rpg_report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    LOG.info("rpg: %d scripts (%d failed), %d topics, %d infos, %d objects -> %d KB "
             "(core %d KB, text %d -> %d KB)", report["scripts"], len(report["script_errors"]),
             report["dialogues"], report["infos"], report["objects"], size // 1024,
             report["core_bytes"] // 1024, report["text_raw_bytes"] // 1024,
             report["text_packed_bytes"] // 1024)
    return report


def main(argv: Optional[Sequence[str]] = None) -> int:
    import world_demaker
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", action="append", default=[], help="Data Files directory")
    ap.add_argument("--esm", action="append", default=[], help="content file in load order")
    ap.add_argument("--out", required=True)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    dmcommon.setup_logging(args.verbose)
    content = world_demaker.discover_content(args.data, args.esm)
    if not content:
        ap.error("no .esm/.esp found; pass --data or --esm")
    cook_rpg(content, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
