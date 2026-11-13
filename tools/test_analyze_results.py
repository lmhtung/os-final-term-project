"""Validate log parsing with in-memory fixtures independent of board measurements."""
import unittest
from analyze_results import parse_log, validate_mode, SUITES, TESTS

def fixture(version="adaptive-v2"):
    configs, scenarios, tests = SUITES[version]
    lines = [f"SUITE_BEGIN,{version},repeats=1"]
    lines += [f"TEST,{name},PASS" for name in sorted(tests)]
    for p in configs:
        for s in scenarios:
            lines.append(f"RESULT,{p},{s},1,0,0,0,0,0,0,0,0,0,0,0,0,16,PASS")
            for g in ("A", "B"):
                lines.append(f"TIMING,{p},{s},1,{g},0,0,0,0,0,0")
    return "\n".join(lines + [f"SUITE_PASS,cases={len(configs) * len(scenarios)}"])

class LogParserTests(unittest.TestCase):
    def test_complete(self):
        results, timings, tests, errors = parse_log(fixture())
        self.assertEqual(len(results), 20)
        self.assertEqual(len(timings), 40)
        self.assertEqual(set(tests), TESTS)
        self.assertEqual(errors, [])

    def test_v1_log_still_parses(self):
        results, timings, tests, errors = parse_log(fixture("adaptive-v1"))
        self.assertEqual((len(results), len(timings), errors), (12, 24, []))
        # A v1 log does not satisfy v2: rows for SHARED_QUOTA and B_OVER_RESERVE are missing.
        self.assertTrue(parse_log(fixture("adaptive-v1").replace("adaptive-v1", "adaptive-v2"))[3])

    def test_unknown_version(self):
        self.assertTrue(parse_log(fixture().replace("adaptive-v2", "adaptive-v9"))[3])

    def test_any_failed_selftest_is_an_error(self):
        self.assertTrue(parse_log(fixture() + "\nTEST,some_extra_test,FAIL")[3])

    def test_incomplete(self):
        self.assertTrue(parse_log(fixture().replace("SUITE_PASS,cases=20", ""))[3])

    def test_last_boot_only(self):
        self.assertTrue(parse_log(fixture() + "\nSUITE_BEGIN,adaptive-v2,repeats=1")[3])
        self.assertEqual(len(parse_log(fixture() + "\n" + fixture())[0]), 20)

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
