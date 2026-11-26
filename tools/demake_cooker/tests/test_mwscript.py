"""mwscript.py: scanner quirks, expressions, statements and bytecode."""

import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import mwscript as M  # noqa: E402
import mwsig  # noqa: E402


def ctx():
    other = M.collect_locals("Begin other_s\nshort greeted\nfloat mood\nEnd")
    glob = M.collect_locals("Begin glob_s\nlong counter\nEnd")
    return M.Context({"gamehour": "f", "tw_flag": "s", "tw_long": "l"},
                     ["tw_npc", "gold_001", "Some Thing"],
                     {"other_s": other, "glob_s": glob}, {"tw_npc": "other_s"})


def body(src, c=None):
    full = "Begin t\nshort s\nlong l\nfloat f\n" + src + "\nEnd t\n"
    comp = M.compile_script(full, c or ctx())
    return comp, M.disassemble(comp.code, comp.strings)


def ops(listing):
    return [line.split(": ", 1)[1] for line in listing]


class ScannerTests(unittest.TestCase):
    def scan(self, line):
        return [(t.kind, t.value) for t in M.scan_line(line, lambda m: None)]

    def test_separators_and_comments(self):
        self.assertEqual(self.scan('AddItem, "Gold_001": 5 ; comment'),
                         [("name", "AddItem"), ("name", "Gold_001"), ("int", 5)])

    def test_names_with_dashes_and_digits(self):
        self.assertEqual(self.scan("1stguard->Disable a-b x - y"),
                         [("name", "1stguard"), ("op", "->"), ("name", "Disable"), ("name", "a-b"),
                          ("name", "x"), ("op", "-"), ("name", "y")])

    def test_comparison_operators_are_normalised(self):
        warnings = []
        toks = M.scan_line("if ( x = 1 ) if ( y > = 2 ) if (z =< 3)", warnings.append)
        self.assertEqual([t.value for t in toks if t.kind == "op" and t.value not in "()"],
                         ["==", ">", "=="])  # "> =" is ">" (with a warning), as in the original
        self.assertEqual(len(warnings), 3)

    def test_floats_and_brackets(self):
        self.assertEqual(self.scan("[.5 2.25]"), [("op", "("), ("float", 0.5), ("float", 2.25), ("op", ")")])


class ExpressionTests(unittest.TestCase):
    def test_precedence_and_types(self):
        _, lst = body("set f to s + l * 2 - f / 4")
        self.assertEqual(ops(lst)[:12], ["loadl 0", "loadl 1", "pushi 2", "muli", "addi", "i2f",
                                         "loadl 2", "pushi 4", "i2f", "divf", "subf", "storel.f 2"])

    def test_unary_minus_binds_tightest(self):
        _, lst = body("set l to -s * 3")
        self.assertEqual(ops(lst)[:4], ["loadl 0", "negi", "pushi 3", "muli"])

    def test_argument_mode_splits_on_minus(self):
        _, lst = body("Position 10 -20 30 0")
        self.assertIn("call position/4", ops(lst))
        self.assertEqual(ops(lst)[2:4], ["pushi 20", "negi"])

    def test_parenthesised_argument_keeps_minus(self):
        _, lst = body("ModPCFacRep (s - 1) \"guild\"")
        self.assertEqual(ops(lst)[:4], ["loadl 0", "pushi 1", "subi", "pushs 'guild'"])

    def test_function_comparison_in_if(self):
        _, lst = body('if ( GetJournalIndex "Quest_A" >= 10 )\nset s to 1\nendif')
        o = ops(lst)
        self.assertEqual(o[:4], ["pushs 'Quest_A'", "call getjournalindex/1", "pushi 10", "cmpi.ge"])
        self.assertTrue(o[4].startswith("jz "))

    def test_float_condition_compared_with_zero(self):
        _, lst = body("if f\nset s to 1\nendif")
        self.assertEqual(ops(lst)[:3], ["loadl 2", "pushf 0", "cmpf.ne"])

    def test_globals_and_members(self):
        c = ctx()
        _, lst = body("set tw_flag to gamehour\nset tw_npc.greeted to 1\nset glob_s.counter to tw_npc.mood", c)
        o = ops(lst)
        gi = c.global_index
        self.assertEqual(o[0], f"loadg {gi['gamehour']}")
        self.assertEqual(o[1], "f2i")
        self.assertEqual(o[2], f"storeg.s {gi['tw_flag']}")
        self.assertTrue(o[4].startswith("storem ") and o[4].endswith("[0] flags=0"))
        self.assertTrue(o[5].startswith("loadm ") and o[5].endswith("[1] flags=4"))
        self.assertTrue(o[7].startswith("storem ") and o[7].endswith("[0] flags=3"))

    def test_explicit_reference_function(self):
        _, lst = body('if ( "Some Thing"->GetDistance player < 200 )\nendif')
        self.assertTrue(ops(lst)[1].endswith("->getdistance/1"))
        self.assertEqual(ops(lst)[1].split()[1].split("->")[0], f"{M.id_hash('some thing'):08x}")

    def test_unknown_name_is_a_number(self):
        comp, lst = body("set s to 5abc + nothing")
        self.assertEqual(ops(lst)[:2], ["pushf 5", "pushf 0"])
        self.assertEqual(len([w for w in comp.warnings if "non-variable" in w]), 2)

    def test_local_shadows_function(self):
        comp, lst = body("short random\nset random to random + 1")
        self.assertNotIn("call random/1", " ".join(ops(lst)))


class StatementTests(unittest.TestCase):
    def test_if_elseif_else_jumps(self):
        comp, lst = body("if ( s == 1 )\nset l to 1\nelseif ( s == 2 )\nset l to 2\nelse\nset l to 3\nendif")
        # every jump lands on an instruction boundary inside the program
        offsets = {int(x.split(":")[0]) for x in lst}
        for line in lst:
            op = line.split(": ", 1)[1]
            if op.startswith(("jmp", "jz")):
                self.assertIn(int(op.split()[1]), offsets | {len(comp.code)})

    def test_while_loop_jumps_back(self):
        _, lst = body("while ( l < 3 )\nset l to l + 1\nendwhile")
        jmp = [x for x in lst if ": jmp " in x][0]
        self.assertEqual(int(jmp.split()[-1]), 0)

    def test_messagebox_arguments_and_buttons(self):
        _, lst = body('MessageBox "Hi %s, %.1f gold %d%%" "Bob" f l "Yes" "No"')
        mb = [x for x in lst if "msgbox" in x][0]
        self.assertIn("args=[2, 1, 0]", mb)
        self.assertIn("buttons=['Yes', 'No']", mb)

    def test_choice_pairs(self):
        _, lst = body('Choice "Yes" 1 "No" 2')
        self.assertIn("call choice/4", ops(lst))

    def test_optional_arguments(self):
        _, lst = body("Disable\nplayer->AddItem gold_001 25\nGetDisabled")
        o = ops(lst)
        self.assertIn("call disable/0", o)
        self.assertTrue(any(x.endswith("->additem/2") for x in o))
        self.assertIn("call getdisabled/0", o)
        self.assertIn("pop", o)  # function result discarded in statement position

    def test_errors_skip_lines_and_close_blocks(self):
        comp, lst = body("set nothere to 1\nif ( s == 1\nbogus thing\nwhile ( 1 )")
        text = " ".join(comp.warnings)
        self.assertIn("unknown variable", text)
        self.assertIn("unknown statement", text)
        self.assertIn("missing 'endwhile'", text)
        self.assertIn("missing 'endif'", text)
        self.assertEqual(lst[-1].split(": ")[1], "ret")

    def test_onactivate_flag_and_locals(self):
        comp, _ = body("if ( OnActivate )\nActivate\nendif")
        self.assertTrue(comp.flags & M.SF_ONACTIVATE)
        self.assertEqual(comp.locals.table(), [("s", "s", 0), ("l", "l", 1), ("f", "f", 2)])

    def test_text_before_begin_and_no_begin(self):
        comp = M.compile_script("junk here\nBegin x\nEnd", ctx())
        self.assertEqual(comp.name, "x")
        with self.assertRaises(M.CompileError):
            M.compile_script("short x", ctx())


class SnippetTests(unittest.TestCase):
    def test_dynamic_locals(self):
        c = ctx()
        comp = M.compile_snippet("set greeted to 1\nif ( mood > 2 )\nendif\nset unknownvar to 1", c)
        lst = ops(M.disassemble(comp.code, comp.strings))
        self.assertEqual(lst[2], f"stored {M.id_hash('greeted'):08x}")
        self.assertEqual(lst[3], f"loadd {M.id_hash('mood'):08x}")
        self.assertTrue(comp.flags & M.SF_DYNAMIC)
        self.assertTrue(any("unknown variable" in w for w in comp.warnings))

    def test_blob_layout(self):
        comp = M.compile_snippet('Journal "q" 10', ctx())
        blob = comp.snippet_blob()
        flags, _pad, code, strings = struct.unpack_from(M.SNIPPET_HEADER_FMT, blob)
        self.assertEqual(len(blob), struct.calcsize(M.SNIPPET_HEADER_FMT) + code + strings)
        self.assertIsNone(M.snippet_or_none("  ; only a comment\n", ctx()))


class TextTests(unittest.TestCase):
    def test_display_text(self):
        self.assertEqual(M.display_text("It\x92s \x93fine\x94\x85 caf\xe9\r\nok"),
                         "It's \"fine\"... cafe\nok")

    def test_format_args(self):
        self.assertEqual(M.message_format_args("%s %S %d %i %5.2f %g %% %x"), ["S", "S", "l", "l", "f", "f"])

    def test_signature_table(self):
        self.assertGreater(len(mwsig.SIGS), 500)
        self.assertEqual(mwsig.SIGS["getjournalindex"].ret, "l")
        self.assertEqual(mwsig.SIGS["choice"].args[:2], "j/")
        ids = [s.id for s in mwsig.BY_ID]
        self.assertEqual(ids, list(range(len(ids))))


if __name__ == "__main__":
    unittest.main()
