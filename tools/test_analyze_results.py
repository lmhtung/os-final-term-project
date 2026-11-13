"""Validate log parsing with in-memory fixtures independent of board measurements."""
import unittest
from analyze_results import parse_log, validate_mode, POLICIES, SCENARIOS, TESTS

def fixture():
    lines = ["SUITE_BEGIN,adaptive-v1,repeats=1"]
    lines += [f"TEST,{name},PASS" for name in sorted(TESTS)]
    for p in POLICIES:
        for s in SCENARIOS:
            lines.append(f"RESULT,{p},{s},1,0,0,0,0,0,0,0,0,0,0,0,0,16,PASS")
            for g in ("A", "B"):
                lines.append(f"TIMING,{p},{s},1,{g},0,0,0,0,0,0")
    return "\n".join(lines + ["SUITE_PASS,cases=12"])

class LogParserTests(unittest.TestCase):
    def test_complete(self):
        results, timings, tests, errors = parse_log(fixture())
        self.assertEqual(len(results), 12)
        self.assertEqual(len(timings), 24)
        self.assertEqual(set(tests), TESTS)
        self.assertEqual(errors, [])

    def test_incomplete(self):
        self.assertTrue(parse_log(fixture().replace("SUITE_PASS,cases=12", ""))[3])

    def test_last_boot_only(self):
        self.assertTrue(parse_log(fixture() + "\nSUITE_BEGIN,adaptive-v1,repeats=1")[3])
        self.assertEqual(len(parse_log(fixture() + "\n" + fixture())[0]), 12)

    def test_duplicate(self):
        extra = next(line for line in fixture().splitlines() if line.startswith("RESULT,"))
        self.assertTrue(parse_log(fixture() + "\n" + extra)[3])

    def test_failed_selftest(self):
        self.assertTrue(parse_log(fixture().replace("api_pointer_alignment_exact_fit,PASS", "api_pointer_alignment_exact_fit,FAIL"))[3])

    def test_malformed_numbers(self):
        broken = fixture().replace("NORMAL,1,0", "NORMAL,not-an-integer,0", 1)
        self.assertTrue(parse_log(broken)[3])

    def test_validate_mode(self):
        self.assertEqual(validate_mode(fixture()), "each_call")  # log cu khong co truong
        boundary = fixture().replace("repeats=1", "repeats=1,validate=boundary")
        self.assertEqual(validate_mode(boundary), "boundary")
        self.assertEqual(parse_log(boundary)[3], [])  # parse_log khong bi anh huong
        self.assertEqual(validate_mode(boundary + "\n" + fixture()), "each_call")  # phien cuoi
        self.assertIsNone(validate_mode("no session here"))

if __name__ == "__main__":
    unittest.main()
