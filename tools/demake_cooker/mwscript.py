"""MWScript compiler: Morrowind script source -> compact stack bytecode.

The language is line oriented and famously loose; the scanner and parser
follow the behaviour the original content relies on:

* ``;`` starts a comment, ``,`` and ``:`` are whitespace, ``[ ]`` alias
  ``( )``, ``->`` is the explicit-reference operator and ``.`` member
  access. Names may contain letters, digits, ``_``, `````, ``'`` and a
  ``-`` that is followed by another name character; a number glued to name
  characters is a name (``1stGuard``). Quoted names are never keywords.
* Comparison operators are scanned greedily over ``= < > !`` and blanks and
  normalised (``=`` and ``= =`` mean ``==``, ``=<`` means ``==``...).
* Priorities: comparisons 1, ``+ -`` 2, ``* /`` 3, unary minus 4. Inside
  an *unparenthesised* function argument, ``+``, ``-`` and comparisons end
  the argument (``Position 10 -20 30 0`` has four arguments); ``*`` and
  ``/`` continue it.
* Names in expressions resolve to a local, then a global, then a function
  (unless a local shadows it), then an object/script ID (``ID->Func``,
  ``ID.var``) and finally - for legacy content - to the number the name
  starts with (usually 0), with a warning.
* Recoverable problems produce warnings and skip the offending line; a
  broken ``if``/``while`` condition compiles to "false" so block structure
  survives.

Bytecode (little endian, see ``MWBC_OP_*`` in ``src/fmt.h``): integers and
floats are typed statically and converted explicitly, string arguments are
offsets into a per-script string pool, locals are slots
``[shorts..., longs..., floats...]``. Dialogue result snippets are compiled
in *dynamic* mode: unknown names that are a local in some script become
by-name accesses resolved against the speaker's script at run time.
"""

import re
import struct
import unicodedata
from typing import Dict, Iterable, List, Optional, Set, Tuple

import dmcommon
import mwsig

# ----------------------------------------------------------------- opcodes
OP_RET = 0x00
OP_PUSHI8 = 0x01
OP_PUSHI16 = 0x02
OP_PUSHI32 = 0x03
OP_PUSHF = 0x04
OP_PUSHS = 0x05
OP_LOADL = 0x06
OP_STOREL_S = 0x07
OP_STOREL_L = 0x08
OP_STOREL_F = 0x09
OP_LOADG = 0x0A
OP_STOREG_S = 0x0B
OP_STOREG_L = 0x0C
OP_STOREG_F = 0x0D
OP_LOADM = 0x0E
OP_STOREM = 0x0F
OP_LOADD = 0x10
OP_STORED = 0x11
OP_I2F = 0x12
OP_F2I = 0x13
OP_NEGI = 0x14
OP_NEGF = 0x15
OP_ADDI = 0x16
OP_SUBI = 0x17
OP_MULI = 0x18
OP_DIVI = 0x19
OP_ADDF = 0x1A
OP_SUBF = 0x1B
OP_MULF = 0x1C
OP_DIVF = 0x1D
OP_CMPI = 0x1E  # + cmp code (EQ NE LT LE GT GE) -> 0x1E..0x23
OP_CMPF = 0x24  # 0x24..0x29
OP_JMP = 0x2A
OP_JZ = 0x2B
OP_POP = 0x2C
OP_CALL = 0x2D
OP_CALLX = 0x2E
OP_MSGBOX = 0x2F
OP_COUNT = 0x30

CMP_CODES = {"==": 0, "!=": 1, "<": 2, "<=": 3, ">": 4, ">=": 5}

# member/store type bits (STOREM operand byte): bit0 = target is a global
# script, bits 1-2 = variable type
VT_SHORT, VT_LONG, VT_FLOAT = 0, 1, 2
VT_CODES = {"s": VT_SHORT, "l": VT_LONG, "f": VT_FLOAT}

# dialogue result script blob header: flags, reserved, code size, strings size
SNIPPET_HEADER_FMT = "<HHII"

# MSGBOX argument types
MA_INT, MA_FLOAT, MA_STRING = 0, 1, 2

# script flags
SF_ONACTIVATE = 0x0001   # script handles activation itself (OnActivate)
SF_DYNAMIC = 0x0002      # snippet with by-name local accesses

KEYWORDS = {"begin", "end", "short", "long", "float", "if", "endif", "else", "elseif", "while",
            "endwhile", "return", "messagebox", "set", "to"}

PREC = {"==": 1, "!=": 1, "<": 1, "<=": 1, ">": 1, ">=": 1, "+": 2, "-": 2, "*": 3, "/": 3}


class CompileError(Exception):
    pass


class LineError(Exception):
    """A problem confined to one statement: warn and skip the line."""


# ------------------------------------------------------------- text helpers
_DISPLAY_MAP = {
    "‘": "'", "’": "'", "‚": ",", "“": '"', "”": '"', "„": '"',
    "–": "-", "—": "-", "…": "...", " ": " ", "´": "'", "`": "'",
    "«": '"', "»": '"', "•": "*", "·": "*", "Æ": "AE", "æ": "ae",
    "Œ": "OE", "œ": "oe", "ß": "ss", "Ø": "O", "ø": "o",
}


def display_text(raw: str) -> str:
    """Game text (Windows-1252 decoded as latin-1) -> printable ASCII for the
    8x8 font. Line breaks are kept as ``\\n``."""
    try:
        s = raw.encode("latin-1").decode("cp1252", errors="replace")
    except UnicodeEncodeError:
        s = raw
    out = []
    for ch in s.replace("\r\n", "\n").replace("\r", "\n"):
        if ch == "\n" or ch == "\t":
            out.append("\n" if ch == "\n" else " ")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        elif ch in _DISPLAY_MAP:
            out.append(_DISPLAY_MAP[ch])
        else:
            base = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode("ascii")
            out.append(base if base else "?")
    return "".join(out)


def id_hash(s: str) -> int:
    return dmcommon.fnv1a32(s)


_NUM_PREFIX = re.compile(r"\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?")


def legacy_number(name: str) -> float:
    m = _NUM_PREFIX.match(name)
    if not m:
        return 0.0
    try:
        return float(m.group(0))
    except ValueError:
        return 0.0


# ----------------------------------------------------------------- scanner
class Tok:
    __slots__ = ("kind", "value", "quoted", "col")

    def __init__(self, kind: str, value, quoted: bool = False, col: int = 0):
        self.kind = kind      # name int float op
        self.value = value
        self.quoted = quoted
        self.col = col

    def __repr__(self) -> str:
        return f"Tok({self.kind},{self.value!r}{',q' if self.quoted else ''})"


def _is_name_char(c: str) -> bool:
    return c.isalnum() or c in "_`'"


def scan_line(line: str, warn) -> List[Tok]:
    toks: List[Tok] = []
    i = 0
    n = len(line)
    at_start = True
    while i < n:
        c = line[i]
        if c == ";":
            break
        if c in " \t,:\r\x00\x1a":
            i += 1
            continue
        if c == '"':
            j = line.find('"', i + 1)
            if j < 0:
                j = n
            toks.append(Tok("name", line[i + 1:j], True, i))
            i = j + 1
            at_start = False
            continue
        if c.isalpha() or c == "_" or (ord(c) >= 128 and c.isalnum()):
            j = i + 1
            while j < n:
                d = line[j]
                if _is_name_char(d):
                    j += 1
                elif d == "-" and j + 1 < n and _is_name_char(line[j + 1]):
                    j += 1
                else:
                    break
            toks.append(Tok("name", line[i:j], False, i))
            i = j
            at_start = False
            continue
        if c.isdigit():
            j = i
            while j < n and line[j].isdigit():
                j += 1
            if j < n and line[j] == ".":
                k = j + 1
                while k < n and line[k].isdigit():
                    k += 1
                if k < n and (line[k].isalpha() or line[k] == "_"):
                    warn(f"malformed number {line[i:k + 1]!r}")
                    k += 1
                    while k < n and _is_name_char(line[k]):
                        k += 1
                    i = k
                    continue
                toks.append(Tok("float", float(line[i:k]), False, i))
                i = k
            elif j < n and (_is_name_char(line[j]) or (line[j] == "-" and j + 1 < n
                                                        and _is_name_char(line[j + 1])
                                                        and not line[j + 1].isdigit())):
                k = j
                while k < n:
                    d = line[k]
                    if _is_name_char(d):
                        k += 1
                    elif d == "-" and k + 1 < n and _is_name_char(line[k + 1]):
                        k += 1
                    else:
                        break
                toks.append(Tok("name", line[i:k], False, i))
                i = k
            else:
                v = int(line[i:j])
                if v > 0x7FFFFFFF:
                    v = 0x7FFFFFFF
                toks.append(Tok("int", v, False, i))
                i = j
            at_start = False
            continue
        if c == "." and i + 1 < n and line[i + 1].isdigit():
            k = i + 1
            while k < n and line[k].isdigit():
                k += 1
            toks.append(Tok("float", float(line[i:k]), False, i))
            i = k
            at_start = False
            continue
        if c in "=<>!":
            j = i
            while j < n and line[j] in "=<>! \t":
                j += 1
            op = line[i:j].rstrip(" \t")
            i = i + len(op)
            first = op[0]
            if first == "=":
                if op != "==":
                    warn(f"invalid operator {op}, treating it as ==")
                op = "=="
            elif first == "<":
                if len(op) > 1 and op[1] == "=":
                    if op != "<=":
                        warn(f"invalid operator {op}, treating it as <=")
                    op = "<="
                else:
                    if op != "<":
                        warn(f"invalid operator {op}, treating it as <")
                    op = "<"
            elif first == ">":
                if len(op) > 1 and op[1] == "=":
                    if op != ">=":
                        warn(f"invalid operator {op}, treating it as >=")
                    op = ">="
                else:
                    if op != ">":
                        warn(f"invalid operator {op}, treating it as >")
                    op = ">"
            else:
                if op != "!=":
                    warn(f"invalid operator {op}, treating it as !=")
                op = "!="
            toks.append(Tok("op", op, False, i))
            at_start = False
            continue
        if c == "-" and i + 1 < n and line[i + 1] == ">":
            toks.append(Tok("op", "->", False, i))
            i += 2
            continue
        if c in "+-*/().[]":
            if at_start and c in "+*/.":
                warn(f"stray special character {c!r} at start of line")
                i += 1
                continue
            v = {"[": "(", "]": ")"}.get(c, c)
            toks.append(Tok("op", v, False, i))
            i += 1
            at_start = False
            continue
        warn(f"ignoring stray character {c!r}")
        i += 1
    return toks


def split_lines(source: str) -> List[str]:
    return source.replace("\r\n", "\n").replace("\r", "\n").split("\n")


# ------------------------------------------------------------------ locals
class Locals:
    """Declared variables of one script, in declaration order per type."""

    def __init__(self) -> None:
        self.shorts: List[str] = []
        self.longs: List[str] = []
        self.floats: List[str] = []
        self._type: Dict[str, str] = {}

    def declare(self, name: str, vtype: str) -> bool:
        key = name.lower()
        if key in self._type:
            return False
        self._type[key] = vtype
        {"s": self.shorts, "l": self.longs, "f": self.floats}[vtype].append(key)
        return True

    def type_of(self, name: str) -> Optional[str]:
        return self._type.get(name.lower())

    def slot(self, name: str) -> int:
        key = name.lower()
        t = self._type[key]
        if t == "s":
            return self.shorts.index(key)
        if t == "l":
            return len(self.shorts) + self.longs.index(key)
        return len(self.shorts) + len(self.longs) + self.floats.index(key)

    def count(self) -> int:
        return len(self._type)

    def table(self) -> List[Tuple[str, str, int]]:
        """(name, type, slot) for every variable."""
        return [(k, t, self.slot(k)) for k, t in self._type.items()]


def collect_locals(source: str) -> Locals:
    """Quick declaration pass (declarations may appear anywhere)."""
    loc = Locals()
    for line in split_lines(source):
        toks = scan_line(line, lambda _m: None)
        if len(toks) >= 2 and toks[0].kind == "name" and not toks[0].quoted:
            kw = toks[0].value.lower()
            if kw in ("short", "long", "float") and toks[1].kind == "name":
                loc.declare(toks[1].value, kw[0])
    return loc


def script_name(source: str) -> Optional[str]:
    for line in split_lines(source):
        toks = scan_line(line, lambda _m: None)
        if toks and toks[0].kind == "name" and not toks[0].quoted and toks[0].value.lower() == "begin":
            if len(toks) > 1 and toks[1].kind == "name":
                return toks[1].value
            return None
    return None


# ----------------------------------------------------------------- context
class Context:
    """What the compiler knows about the world."""

    def __init__(self, globals_: Optional[Dict[str, str]] = None, ids: Iterable[str] = (),
                 scripts: Optional[Dict[str, Locals]] = None,
                 object_scripts: Optional[Dict[str, str]] = None) -> None:
        self.globals: Dict[str, str] = {k.lower(): v for k, v in (globals_ or {}).items()}
        self.global_index: Dict[str, int] = {k: i for i, k in enumerate(sorted(self.globals, key=id_hash))}
        self.ids: Set[str] = {i.lower() for i in ids}
        self.ids.add("player")
        self.scripts: Dict[str, Locals] = {k.lower(): v for k, v in (scripts or {}).items()}
        self.ids.update(self.scripts)
        self.object_scripts = {k.lower(): v.lower() for k, v in (object_scripts or {}).items() if v}
        self.dynamic_names: Set[str] = set()
        for loc in self.scripts.values():
            self.dynamic_names.update(loc._type)

    def member_type(self, var: str, ref: str) -> Optional[Tuple[str, int, bool]]:
        """(type, slot, is_global_script) of ref.var, or None."""
        ref = ref.lower()
        if ref in self.scripts:
            loc, glob = self.scripts[ref], True
        else:
            sname = self.object_scripts.get(ref)
            if not sname or sname not in self.scripts:
                return None
            loc, glob = self.scripts[sname], False
        t = loc.type_of(var)
        if t is None:
            return None
        return t, loc.slot(var), glob


# -------------------------------------------------------------- the result
class Compiled:
    def __init__(self, name: str, locals_: Locals, code: bytes, strings: bytes, flags: int,
                 warnings: List[str]) -> None:
        self.name = name
        self.locals = locals_
        self.code = code
        self.strings = strings
        self.flags = flags
        self.warnings = warnings

    def snippet_blob(self) -> bytes:
        """Self-contained form used for dialogue result scripts."""
        return (struct.pack(SNIPPET_HEADER_FMT, self.flags, 0, len(self.code), len(self.strings))
                + self.code + self.strings)


# ----------------------------------------------------------------- parser
class _Line:
    def __init__(self, toks: List[Tok]):
        self.toks = toks
        self.i = 0

    def peek(self, k: int = 0) -> Optional[Tok]:
        j = self.i + k
        return self.toks[j] if j < len(self.toks) else None

    def next(self) -> Optional[Tok]:
        t = self.peek()
        if t is not None:
            self.i += 1
        return t

    def at_end(self) -> bool:
        return self.i >= len(self.toks)

    def is_op(self, value: str, k: int = 0) -> bool:
        t = self.peek(k)
        return t is not None and t.kind == "op" and t.value == value


def _stringable(t: Optional[Tok]) -> bool:
    return t is not None and t.kind in ("name", "int", "float")


def _tok_text(t: Tok) -> str:
    if t.kind == "int":
        return str(t.value)
    if t.kind == "float":
        return repr(t.value)
    return t.value


def _can_start_expr(t: Optional[Tok]) -> bool:
    if t is None:
        return False
    if t.kind in ("name", "int", "float"):
        return True
    return t.kind == "op" and t.value in ("(", "-", "+")


def message_format_args(fmt: str) -> List[str]:
    """Argument types ('S', 'l', 'f') a MessageBox format string consumes."""
    out = []
    i = 0
    n = len(fmt)
    while i < n:
        if fmt[i] != "%":
            i += 1
            continue
        i += 1
        if i < n and fmt[i] == "%":
            i += 1
            continue
        while i < n and fmt[i] in "-+ #0":
            i += 1
        while i < n and fmt[i].isdigit():
            i += 1
        if i < n and fmt[i] == ".":
            i += 1
            while i < n and fmt[i].isdigit():
                i += 1
        if i < n:
            c = fmt[i]
            if c in "Ss":
                out.append("S")
            elif c in "di":
                out.append("l")
            elif c in "feEgGF":
                out.append("f")
            i += 1
    return out


class Compiler:
    def __init__(self, ctx: Context, locals_: Locals, dynamic: bool = False) -> None:
        self.ctx = ctx
        self.locals = locals_
        self.dynamic = dynamic
        self.code = bytearray()
        self.strings = bytearray()
        self.string_ofs: Dict[str, int] = {}
        self.warnings: List[str] = []
        self.flags = SF_DYNAMIC if dynamic else 0
        self.lineno = 0
        self.blocks: List[dict] = []

    # ------------------------------------------------------------- helpers
    def warn(self, msg: str) -> None:
        self.warnings.append(f"line {self.lineno}: {msg}")

    def string(self, s: str, display: bool) -> int:
        text = display_text(s) if display else s
        if text in self.string_ofs:
            return self.string_ofs[text]
        ofs = len(self.strings)
        if ofs > 0xFFFF:
            raise CompileError("string pool exceeds 64 KB")
        self.strings += text.encode("latin-1", errors="replace") + b"\0"
        self.string_ofs[text] = ofs
        return ofs

    @staticmethod
    def push_int(out: bytearray, v: int) -> None:
        v = max(-0x80000000, min(0x7FFFFFFF, int(v)))
        if -128 <= v <= 127:
            out += struct.pack("<Bb", OP_PUSHI8, v)
        elif -32768 <= v <= 32767:
            out += struct.pack("<Bh", OP_PUSHI16, v)
        else:
            out += struct.pack("<Bi", OP_PUSHI32, v)

    @staticmethod
    def push_float(out: bytearray, v: float) -> None:
        out += struct.pack("<Bf", OP_PUSHF, v)

    @staticmethod
    def convert(out: bytearray, have: str, want: str) -> None:
        if have == want:
            return
        out.append(OP_I2F if want == "f" else OP_F2I)

    # ---------------------------------------------------------- expressions
    def expr(self, ln: _Line, out: bytearray, arg: bool) -> str:
        return self._binary(ln, out, 1, arg)

    def _binary(self, ln: _Line, out: bytearray, minprec: int, arg: bool) -> str:
        lt = self._unary(ln, out, arg)
        while True:
            t = ln.peek()
            if t is None or t.kind != "op" or t.value not in PREC:
                return lt
            op = t.value
            prec = PREC[op]
            if prec < minprec:
                return lt
            if arg and op not in ("*", "/"):
                return lt
            ln.next()
            rbuf = bytearray()
            rt = self._binary(ln, rbuf, prec + 1, arg)
            if lt != rt:
                if lt == "l":
                    out.append(OP_I2F)
                    lt = "f"
                else:
                    rbuf.append(OP_I2F)
            out += rbuf
            if op in CMP_CODES:
                out.append((OP_CMPF if lt == "f" else OP_CMPI) + CMP_CODES[op])
                lt = "l"
            else:
                base = {"+": 0, "-": 1, "*": 2, "/": 3}[op]
                out.append((OP_ADDF if lt == "f" else OP_ADDI) + base)

    def _unary(self, ln: _Line, out: bytearray, arg: bool) -> str:
        t = ln.peek()
        if t is not None and t.kind == "op" and t.value == "-":
            ln.next()
            ty = self._unary(ln, out, arg)
            out.append(OP_NEGF if ty == "f" else OP_NEGI)
            return ty
        if t is not None and t.kind == "op" and t.value == "+":
            ln.next()
            return self._unary(ln, out, arg)
        return self._primary(ln, out)

    def _primary(self, ln: _Line, out: bytearray) -> str:
        t = ln.next()
        if t is None:
            raise LineError("missing expression")
        if t.kind == "int":
            self.push_int(out, t.value)
            return "l"
        if t.kind == "float":
            self.push_float(out, t.value)
            return "f"
        if t.kind == "op":
            if t.value == "(":
                ty = self.expr(ln, out, False)
                if not ln.is_op(")"):
                    raise LineError("missing ')'")
                ln.next()
                return ty
            raise LineError(f"unexpected {t.value!r} in expression")
        name = t.value
        low = name.lower()
        sig = mwsig.SIGS.get(low)
        if (not t.quoted and sig is not None and sig.kind == "f"
                and self.locals.type_of(low) is None):
            return self.call(ln, out, sig, None, as_value=True)
        lt = self.locals.type_of(low)
        if lt is not None:
            out += struct.pack("<BH", OP_LOADL, self.locals.slot(low))
            return "f" if lt == "f" else "l"
        gt = self.ctx.globals.get(low)
        if gt is not None:
            out += struct.pack("<BH", OP_LOADG, self.ctx.global_index[low])
            return "f" if gt == "f" else "l"
        if ln.is_op("->"):
            ln.next()
            f = ln.next()
            if f is None or f.kind != "name":
                raise LineError("expected a function after '->'")
            fsig = mwsig.SIGS.get(f.value.lower())
            if fsig is None or fsig.kind != "f":
                raise LineError(f"{f.value!r} is not a function")
            return self.call(ln, out, fsig, name, as_value=True)
        if ln.is_op(".") and ln.peek(1) is not None and ln.peek(1).kind == "name":
            ln.next()
            var = ln.next().value
            mt = self.ctx.member_type(var, name)
            if mt is None:
                raise LineError(f"unknown member {name}.{var}")
            vt, slot, glob = mt
            out += struct.pack("<BIHB", OP_LOADM, id_hash(name), slot,
                               (1 if glob else 0) | (VT_CODES[vt] << 1))
            return "f" if vt == "f" else "l"
        if self.dynamic and low in self.ctx.dynamic_names:
            out += struct.pack("<BI", OP_LOADD, id_hash(low))
            return "f"
        value = legacy_number(name)
        self.warn(f"parsing a non-variable string as a number: {name!r} -> {value:g}")
        self.push_float(out, value)
        return "f"

    # ------------------------------------------------------------ calls
    def args(self, ln: _Line, out: bytearray, sig: mwsig.Sig) -> int:
        optional = False
        argc = 0
        spec = sig.args
        for a in spec:
            if a == "/":
                optional = True
                continue
            t = ln.peek()
            if a in "Sc":
                if _stringable(t):
                    ln.next()
                    out += struct.pack("<BH", OP_PUSHS, self.string(_tok_text(t), a == "S"))
                    argc += 1
                elif optional:
                    break
                else:
                    raise LineError(f"{sig.name}: missing string argument")
            elif a == "x":
                if _stringable(t):
                    ln.next()
                    self.warn(f"{sig.name}: extra argument {_tok_text(t)!r} ignored")
                else:
                    break
            elif a == "X":
                if _can_start_expr(t):
                    self.expr(ln, bytearray(), True)
                    self.warn(f"{sig.name}: extra argument ignored")
                else:
                    break
            elif a == "z":
                if _stringable(t):
                    ln.next()
                    self.warn(f"{sig.name}: extra argument {_tok_text(t)!r} ignored")
                else:
                    break
            elif a == "j":
                continue
            else:
                want = "f" if a == "f" else "l"
                if _can_start_expr(t):
                    have = self.expr(ln, out, True)
                    self.convert(out, have, want)
                    argc += 1
                elif optional:
                    break
                else:
                    raise LineError(f"{sig.name}: missing numeric argument")
        if argc > 255:
            raise LineError("too many arguments")
        return argc

    def call(self, ln: _Line, out: bytearray, sig: mwsig.Sig, ref: Optional[str],
             as_value: bool) -> str:
        if sig.name == "onactivate":
            self.flags |= SF_ONACTIVATE
        if ref is not None and not sig.explicit:
            self.warn(f"stray explicit reference {ref!r} on {sig.name}")
            ref = None
        argc = self.args(ln, out, sig)
        if ref is None:
            out += struct.pack("<BHB", OP_CALL, sig.id, argc)
        else:
            out += struct.pack("<BHBI", OP_CALLX, sig.id, argc, id_hash(ref))
        if not as_value:
            if sig.kind == "f":
                out.append(OP_POP)
            return ""
        return sig.ret if sig.ret in ("f", "l") else "l"

    # ------------------------------------------------------------ blocks
    def _jump(self, op: int) -> int:
        self.code += struct.pack("<Bh", op, 0)
        return len(self.code) - 2

    def _patch(self, at: int, target: Optional[int] = None) -> None:
        target = len(self.code) if target is None else target
        rel = target - (at + 2)
        if not -32768 <= rel <= 32767:
            raise CompileError("jump distance exceeds 32 KB")
        struct.pack_into("<h", self.code, at, rel)

    def _condition(self, ln: _Line) -> None:
        buf = bytearray()
        try:
            ty = self.expr(ln, buf, False)
            if not ln.at_end():
                self.warn("extra tokens after condition ignored")
        except LineError as e:
            self.warn(f"bad condition ({e}); treated as false")
            buf = bytearray()
            self.push_int(buf, 0)
            ty = "l"
        if ty == "f":
            self.push_float(buf, 0.0)
            buf.append(OP_CMPF + CMP_CODES["!="])
        self.code += buf

    # --------------------------------------------------------- statements
    def statement(self, toks: List[Tok]) -> bool:
        """Compiles one line. Returns False when the script body ended."""
        ln = _Line(toks)
        t = ln.peek()
        if t is None:
            return True
        word = t.value.lower() if (t.kind == "name" and not t.quoted) else None
        if word in ("short", "long", "float"):
            return True  # collected by the declaration pass
        if word == "end":
            return False
        if word == "begin":
            self.warn("nested 'begin' ignored")
            return True
        if word == "if":
            ln.next()
            self._condition(ln)
            self.blocks.append({"kind": "if", "jz": self._jump(OP_JZ), "ends": [], "else": False})
            return True
        if word == "elseif":
            ln.next()
            b = self.blocks[-1] if self.blocks else None
            if b is None or b["kind"] != "if" or b["else"]:
                self.warn("'elseif' without 'if' ignored")
                return True
            b["ends"].append(self._jump(OP_JMP))
            self._patch(b["jz"])
            self._condition(ln)
            b["jz"] = self._jump(OP_JZ)
            return True
        if word == "else":
            b = self.blocks[-1] if self.blocks else None
            if b is None or b["kind"] != "if" or b["else"]:
                self.warn("'else' without 'if' ignored")
                return True
            b["ends"].append(self._jump(OP_JMP))
            self._patch(b["jz"])
            b["jz"] = None
            b["else"] = True
            return True
        if word == "endif":
            b = self.blocks[-1] if self.blocks else None
            if b is None or b["kind"] != "if":
                self.warn("'endif' without 'if' ignored")
                return True
            self._close_if(self.blocks.pop())
            return True
        if word == "while":
            ln.next()
            start = len(self.code)
            self._condition(ln)
            self.blocks.append({"kind": "while", "start": start, "jz": self._jump(OP_JZ)})
            return True
        if word == "endwhile":
            b = self.blocks[-1] if self.blocks else None
            if b is None or b["kind"] != "while":
                self.warn("'endwhile' without 'while' ignored")
                return True
            self._close_while(self.blocks.pop())
            return True
        try:
            if word == "return":
                self.code.append(OP_RET)
            elif word == "set":
                ln.next()
                self._set(ln)
            elif word == "messagebox":
                ln.next()
                self._messagebox(ln)
            else:
                self._command(ln)
            if not ln.at_end():
                self.warn(f"extra tokens ignored: {' '.join(_tok_text(x) if x.kind != 'op' else x.value for x in ln.toks[ln.i:])}")
        except LineError as e:
            self.warn(f"{e}; line skipped")
        return True

    def _close_if(self, b: dict) -> None:
        if b["jz"] is not None:
            self._patch(b["jz"])
        for at in b["ends"]:
            self._patch(at)

    def _close_while(self, b: dict) -> None:
        at = self._jump(OP_JMP)
        self._patch(at, b["start"])
        self._patch(b["jz"])

    def _set(self, ln: _Line) -> None:
        t = ln.next()
        if t is None or t.kind != "name":
            raise LineError("set: missing variable")
        name = t.value
        low = name.lower()
        target: Tuple
        if ln.is_op(".") and ln.peek(1) is not None and ln.peek(1).kind == "name":
            ln.next()
            var = ln.next().value
            mt = self.ctx.member_type(var, name)
            if mt is None:
                raise LineError(f"set: unknown member {name}.{var}")
            vt, slot, glob = mt
            target = ("m", vt, (id_hash(name), slot, (1 if glob else 0) | (VT_CODES[vt] << 1)))
        elif self.locals.type_of(low) is not None:
            target = ("l", self.locals.type_of(low), self.locals.slot(low))
        elif low in self.ctx.globals:
            target = ("g", self.ctx.globals[low], self.ctx.global_index[low])
        elif self.dynamic and low in self.ctx.dynamic_names:
            target = ("d", "f", id_hash(low))
        else:
            raise LineError(f"set: unknown variable {name!r}")
        to = ln.peek()
        if to is not None and to.kind == "name" and not to.quoted and to.value.lower() == "to":
            ln.next()
        else:
            self.warn("set: missing 'to'")
        have = self.expr(ln, self.code, False)
        kind, vt, operand = target
        want = "f" if vt == "f" else "l"
        self.convert(self.code, have, want)
        if kind == "l":
            op = {"s": OP_STOREL_S, "l": OP_STOREL_L, "f": OP_STOREL_F}[vt]
            self.code += struct.pack("<BH", op, operand)
        elif kind == "g":
            op = {"s": OP_STOREG_S, "l": OP_STOREG_L, "f": OP_STOREG_F}[vt]
            self.code += struct.pack("<BH", op, operand)
        elif kind == "m":
            h, slot, flags = operand
            self.code += struct.pack("<BIHB", OP_STOREM, h, slot, flags)
        else:
            self.code += struct.pack("<BI", OP_STORED, operand)

    def _messagebox(self, ln: _Line) -> None:
        t = ln.next()
        if not _stringable(t):
            raise LineError("messagebox: missing message")
        fmt = _tok_text(t)
        fmt_ofs = self.string(fmt, True)
        kinds = message_format_args(display_text(fmt))
        types = []
        for k in kinds:
            nt = ln.peek()
            if k == "S":
                if not _stringable(nt):
                    raise LineError("messagebox: missing string argument")
                ln.next()
                self.code += struct.pack("<BH", OP_PUSHS, self.string(_tok_text(nt), True))
                types.append(MA_STRING)
            else:
                if not _can_start_expr(nt):
                    raise LineError("messagebox: missing argument")
                have = self.expr(ln, self.code, True)
                self.convert(self.code, have, k)
                types.append(MA_FLOAT if k == "f" else MA_INT)
        buttons = []
        while _stringable(ln.peek()):
            buttons.append(self.string(_tok_text(ln.next()), True))
        if len(types) > 255 or len(buttons) > 255:
            raise LineError("messagebox: too many arguments")
        self.code += struct.pack("<BHBB", OP_MSGBOX, fmt_ofs, len(types), len(buttons))
        self.code += bytes(types)
        for b in buttons:
            self.code += struct.pack("<H", b)

    def _command(self, ln: _Line) -> None:
        t = ln.next()
        if t.kind != "name":
            raise LineError(f"unexpected {_tok_text(t) if t.kind != 'op' else t.value!r}")
        low = t.value.lower()
        if ln.is_op("->"):
            ln.next()
            f = ln.next()
            if f is None or f.kind != "name" or f.value.lower() not in mwsig.SIGS:
                raise LineError(f"unknown instruction after {t.value}->")
            self.call(ln, self.code, mwsig.SIGS[f.value.lower()], t.value, as_value=False)
            return
        sig = mwsig.SIGS.get(low)
        if sig is not None and not t.quoted:
            self.call(ln, self.code, sig, None, as_value=False)
            return
        raise LineError(f"unknown statement {t.value!r}")

    # ------------------------------------------------------------- driver
    def finish(self) -> None:
        while self.blocks:
            b = self.blocks.pop()
            self.warn(f"missing 'end{b['kind']}' added")
            if b["kind"] == "if":
                self._close_if(b)
            else:
                self._close_while(b)
        self.code.append(OP_RET)


def compile_script(source: str, ctx: Context, name: Optional[str] = None,
                   locals_: Optional[Locals] = None) -> Compiled:
    """Compiles a full script (``Begin name`` ... ``End``)."""
    loc = locals_ if locals_ is not None else collect_locals(source)
    comp = Compiler(ctx, loc)
    started = False
    for lineno, line in enumerate(split_lines(source), 1):
        comp.lineno = lineno
        toks = scan_line(line, comp.warn)
        if not toks:
            continue
        if not started:
            if toks[0].kind == "name" and not toks[0].quoted and toks[0].value.lower() == "begin":
                started = True
                if name is None and len(toks) > 1:
                    name = toks[1].value
            else:
                comp.warn("text before 'begin' ignored")
            continue
        if not comp.statement(toks):
            break
    if not started:
        raise CompileError("script has no 'begin'")
    comp.finish()
    if len(comp.code) > 0xFFFFFF:
        raise CompileError("script too large")
    return Compiled(name or "", loc, bytes(comp.code), bytes(comp.strings), comp.flags, comp.warnings)


def compile_snippet(source: str, ctx: Context, locals_: Optional[Locals] = None) -> Compiled:
    """Compiles a dialogue result script (no Begin/End, speaker locals by name)."""
    comp = Compiler(ctx, locals_ if locals_ is not None else Locals(), dynamic=True)
    for lineno, line in enumerate(split_lines(source), 1):
        comp.lineno = lineno
        toks = scan_line(line, comp.warn)
        if not toks:
            continue
        if not comp.statement(toks):
            break
    comp.finish()
    return Compiled("", comp.locals, bytes(comp.code), bytes(comp.strings), comp.flags, comp.warnings)


# ------------------------------------------------------------- disassembly
_OP_NAMES = {OP_RET: "ret", OP_PUSHI8: "pushi", OP_PUSHI16: "pushi", OP_PUSHI32: "pushi",
             OP_PUSHF: "pushf", OP_PUSHS: "pushs", OP_LOADL: "loadl", OP_STOREL_S: "storel.s",
             OP_STOREL_L: "storel.l", OP_STOREL_F: "storel.f", OP_LOADG: "loadg",
             OP_STOREG_S: "storeg.s", OP_STOREG_L: "storeg.l", OP_STOREG_F: "storeg.f",
             OP_LOADM: "loadm", OP_STOREM: "storem", OP_LOADD: "loadd", OP_STORED: "stored",
             OP_I2F: "i2f", OP_F2I: "f2i", OP_NEGI: "negi", OP_NEGF: "negf", OP_ADDI: "addi",
             OP_SUBI: "subi", OP_MULI: "muli", OP_DIVI: "divi", OP_ADDF: "addf", OP_SUBF: "subf",
             OP_MULF: "mulf", OP_DIVF: "divf", OP_JMP: "jmp", OP_JZ: "jz", OP_POP: "pop",
             OP_CALL: "call", OP_CALLX: "callx", OP_MSGBOX: "msgbox"}
_CMP_NAMES = ["eq", "ne", "lt", "le", "gt", "ge"]


def _cstr(pool: bytes, ofs: int) -> str:
    end = pool.find(b"\0", ofs)
    return pool[ofs:end if end >= 0 else len(pool)].decode("latin-1")


def disassemble(code: bytes, strings: bytes) -> List[str]:
    """Readable listing (``offset: op operands``) for tests and debugging."""
    out = []
    pc = 0
    while pc < len(code):
        op = code[pc]
        at = pc
        pc += 1
        if OP_CMPI <= op < OP_CMPI + 6:
            out.append(f"{at}: cmpi.{_CMP_NAMES[op - OP_CMPI]}")
            continue
        if OP_CMPF <= op < OP_CMPF + 6:
            out.append(f"{at}: cmpf.{_CMP_NAMES[op - OP_CMPF]}")
            continue
        name = _OP_NAMES.get(op)
        if name is None:
            raise ValueError(f"bad opcode {op:#x} at {at}")
        if op == OP_PUSHI8:
            (v,) = struct.unpack_from("<b", code, pc); pc += 1; arg = str(v)
        elif op == OP_PUSHI16:
            (v,) = struct.unpack_from("<h", code, pc); pc += 2; arg = str(v)
        elif op == OP_PUSHI32:
            (v,) = struct.unpack_from("<i", code, pc); pc += 4; arg = str(v)
        elif op == OP_PUSHF:
            (v,) = struct.unpack_from("<f", code, pc); pc += 4; arg = f"{v:g}"
        elif op == OP_PUSHS:
            (v,) = struct.unpack_from("<H", code, pc); pc += 2; arg = repr(_cstr(strings, v))
        elif op in (OP_LOADL, OP_STOREL_S, OP_STOREL_L, OP_STOREL_F, OP_LOADG, OP_STOREG_S,
                    OP_STOREG_L, OP_STOREG_F):
            (v,) = struct.unpack_from("<H", code, pc); pc += 2; arg = str(v)
        elif op in (OP_LOADM, OP_STOREM):
            h, slot, fl = struct.unpack_from("<IHB", code, pc); pc += 7
            arg = f"{h:08x}[{slot}] flags={fl}"
        elif op in (OP_LOADD, OP_STORED):
            (h,) = struct.unpack_from("<I", code, pc); pc += 4; arg = f"{h:08x}"
        elif op in (OP_JMP, OP_JZ):
            (v,) = struct.unpack_from("<h", code, pc); pc += 2; arg = str(pc + v)
        elif op == OP_CALL:
            fid, argc = struct.unpack_from("<HB", code, pc); pc += 3
            arg = f"{mwsig.BY_ID[fid].name}/{argc}"
        elif op == OP_CALLX:
            fid, argc, h = struct.unpack_from("<HBI", code, pc); pc += 7
            arg = f"{h:08x}->{mwsig.BY_ID[fid].name}/{argc}"
        elif op == OP_MSGBOX:
            fmt, na, nb = struct.unpack_from("<HBB", code, pc); pc += 4
            types = list(code[pc:pc + na]); pc += na
            btns = [_cstr(strings, struct.unpack_from("<H", code, pc + 2 * k)[0]) for k in range(nb)]
            pc += 2 * nb
            arg = f"{_cstr(strings, fmt)!r} args={types} buttons={btns}"
        else:
            arg = ""
        out.append(f"{at}: {name} {arg}".rstrip())
    return out


def compile_all(sources: Dict[str, str], ctx: Context) -> Tuple[Dict[str, Compiled], Dict[str, str]]:
    """Compiles every script; returns (compiled by lower-case id, errors)."""
    done: Dict[str, Compiled] = {}
    errors: Dict[str, str] = {}
    for sid, src in sources.items():
        key = sid.lower()
        try:
            done[key] = compile_script(src, ctx, sid, ctx.scripts.get(key))
        except CompileError as e:
            errors[key] = str(e)
    return done, errors


def make_context(sources: Dict[str, str], globals_: Dict[str, str], ids: Iterable[str],
                 object_scripts: Dict[str, str]) -> Context:
    scripts = {sid.lower(): collect_locals(src) for sid, src in sources.items()}
    return Context(globals_, ids, scripts, object_scripts)


def snippet_or_none(source: str, ctx: Context, warnings: Optional[List[str]] = None
                    ) -> Optional[Compiled]:
    """Result scripts that contain only whitespace/comments compile to None."""
    if not any(scan_line(line, lambda _m: None) for line in split_lines(source)):
        return None
    c = compile_snippet(source, ctx)
    if warnings is not None:
        warnings.extend(c.warnings)
    return c


__all__ = ["Context", "Locals", "Compiled", "CompileError", "compile_script", "compile_snippet",
           "compile_all", "make_context", "disassemble", "display_text", "message_format_args",
           "collect_locals", "script_name", "snippet_or_none"]
