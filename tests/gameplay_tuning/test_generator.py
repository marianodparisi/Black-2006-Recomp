"""Authored guards for the combat hook boundaries, without retail code."""
import ast
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import generate


def sites():
    tree = ast.parse((ROOT / "scripts/generate.py").read_text(encoding="utf-8"))
    function = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                    and n.name == "gameplay_tuning_hooks")
    assignment = next(n for n in function.body if isinstance(n, ast.Assign)
                      and any(isinstance(t, ast.Name) and t.id == "sites" for t in n.targets))
    return ast.literal_eval(assignment.value)


def fixture(name, entries):
    body = "void " + name + "(void)\n{\n"
    for label, native, _ in entries:
        body += "loc_" + label + ": ;\n    " + native + "\n    untouched();\n"
    return body + "loc_end: ;\n    return;\n}\n"


class CombatGeneratorTests(unittest.TestCase):
    def test_supported_boundaries_and_idempotence(self):
        table = sites()
        self.assertEqual(len(table), 7)
        self.assertEqual(sum(map(len, table.values())), 9)
        for name, entries in table.items():
            original = fixture(name, entries)
            hooked = generate.gameplay_tuning_hooks(name, original)
            self.assertEqual(generate.gameplay_tuning_hooks(name, hooked), hooked)
            self.assertEqual(original.count("untouched();"), hooked.count("untouched();"))
            for _, _, replacement in entries:
                self.assertEqual(hooked.count(replacement), 1)

    def test_changed_missing_duplicate_moved_boundaries_fail(self):
        for name, entries in sites().items():
            original = fixture(name, entries)
            for label, native, replacement in entries:
                marker = "loc_" + label + ": ;\n"
                for invalid in [original.replace(marker, "", 1),
                                original.replace(marker, marker + marker, 1),
                                original.replace(native, "changed();", 1),
                                original.replace(native, native + "\n    " + native, 1),
                                original.replace(native, "", 1).replace("loc_end: ;", "loc_end: ;\n" + native),
                                original.replace(native, replacement + "\n    " + replacement, 1)]:
                    with self.subTest(name=name, label=label):
                        with self.assertRaises(ValueError):
                            generate.gameplay_tuning_hooks(name, invalid)

    def test_other_functions_unchanged(self):
        original = "void unrelated(void) { untouched(); }"
        self.assertEqual(generate.gameplay_tuning_hooks("unrelated", original), original)


if __name__ == "__main__":
    unittest.main()
