"""Validate and summarize ADAPTIVE firmware logs using the Python standard library."""
import argparse
import csv
import io
from pathlib import Path
import re
import statistics

RESULT_FIELDS = "policy scenario run a_ok b_ok a_no_memory b_no_memory a_quota b_quota a_injected b_injected a_peak_charged b_peak_charged a_peak_borrowed free_payload_after header_bytes result".split()
TIMING_FIELDS = "policy scenario run group alloc_calls alloc_us_sum alloc_us_max free_calls free_us_sum free_us_max".split()
POLICIES = ("SHARED", "STATIC", "ADAPTIVE")        # allocator policies (self-tests, SMP stress)
CONFIGS = POLICIES + ("SHARED_QUOTA",)             # RESULT/TIMING rows: policy plus optional quota
SCENARIOS = ("NORMAL", "BURST", "PRESSURE", "INJECTED", "B_OVER_RESERVE")
TESTS_V1 = {"api_pointer_alignment_exact_fit", "fragmentation_and_coalescing", "borrow_return_quota_injection"}
TESTS_V1 |= {f"random_{p}_4000_seed_1234ABCD" for p in POLICIES}
TESTS_V1 |= {f"smp_{p}_2000_allocations" for p in POLICIES}
TESTS = TESTS_V1 | {"region_isolation_and_group_reset"}
# What each suite version must print. adaptive-v1 is the firmware before SHARED_QUOTA and B_OVER_RESERVE.
SUITES = {
    "adaptive-v1": (POLICIES, SCENARIOS[:4], TESTS_V1),
    "adaptive-v2": (CONFIGS, SCENARIOS, TESTS),
}

def parse_log(text):
    """Select the final boot session to keep results from separate resets isolated."""
    results, timings, tests, errors = [], [], {}, []
    repeats, finished, started, version = 0, False, False, None
    for line in re.sub(r"\x1b\[[0-9;]*m", "", text).splitlines():
        if "SUITE_BEGIN," in line:
            results, timings, tests, errors = [], [], {}, []
            started, finished = True, False
            match = re.search(r"repeats=(\d+)", line)
            repeats = int(match.group(1)) if match else 0
            version = line.split("SUITE_BEGIN,", 1)[1].split(",", 1)[0].strip()
        if not started:
            continue
        for marker, fields, destination in (("RESULT,", RESULT_FIELDS, results), ("TIMING,", TIMING_FIELDS, timings)):
            if marker in line:
                values = next(csv.reader(io.StringIO(line.split(marker, 1)[1])))
                if len(values) != len(fields):
                    errors.append(f"Malformed {marker[:-1]} row: {line}")
                    continue
                row = dict(zip(fields, values))
                try:
                    for field in fields:
                        if field not in ("policy", "scenario", "group", "result"):
                            row[field] = int(row[field])
                            if row[field] < 0:
                                raise ValueError("negative number")
                except ValueError:
                    errors.append(f"Invalid numeric data: {line}")
                    continue
                destination.append(row)
        if "TEST," in line:
            parts = line.split("TEST,", 1)[1].split(",")
            if len(parts) == 2:
                tests[parts[0]] = parts[1].strip()
        if "SUITE_FAIL," in line:
            errors.append(line.strip())
        if "SUITE_PASS," in line:
            finished = True
    if not started or not finished or repeats < 1:
        errors.append("Final boot session is incomplete (need SUITE_BEGIN and SUITE_PASS).")
    if started and version not in SUITES:
        errors.append(f"Unknown suite version: {version}")
    configs, scenarios, required_tests = SUITES.get(version, SUITES["adaptive-v2"])
    expected = {(p, s, r) for p in configs for s in scenarios for r in range(1, repeats + 1)}
    keys = [(r["policy"], r["scenario"], r["run"]) for r in results]
    if set(keys) != expected or len(keys) != len(set(keys)):
        errors.append("Missing, unexpected or duplicate RESULT rows.")
    timing_keys = [(r["policy"], r["scenario"], r["run"], r["group"]) for r in timings]
    expected_timings = {(*key, g) for key in expected for g in ("A", "B")}
    if set(timing_keys) != expected_timings or len(timing_keys) != len(set(timing_keys)):
        errors.append("Missing, unexpected or duplicate TIMING rows.")
    if any(r["result"] != "PASS" for r in results):
        errors.append("Firmware reported a failed scenario.")
    if any(tests.get(name) != "PASS" for name in required_tests) or any(v != "PASS" for v in tests.values()):
        errors.append("Required on-board self-tests are missing or failed.")
    return results, timings, tests, errors

VALIDATE_MODES = ("each_call", "boundary")

def validate_mode(text):
    """Return the validate= mode of the final boot session.

    Logs written before the option existed carry no field; that firmware always
    validated inside every call, so they map to each_call. None means no session.
    """
    mode = None
    for line in re.sub(r"\x1b\[[0-9;]*m", "", text).splitlines():
        if "SUITE_BEGIN," in line:
            match = re.search(r"validate=([A-Za-z_]+)", line)
            mode = match.group(1) if match else "each_call"
    return mode

def write_csv(path, fields, rows):
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--out", type=Path, default=Path("results/analysis"))
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    results, timings, tests, errors = parse_log(text)
    mode = validate_mode(text)
    if mode is not None and mode not in VALIDATE_MODES:
        errors.append(f"Unknown validate mode: {mode}")
    args.out.mkdir(parents=True, exist_ok=True)
    write_csv(args.out / "results.csv", RESULT_FIELDS, results)
    write_csv(args.out / "timings.csv", TIMING_FIELDS, timings)
    lines = ["# Báo cáo kết quả từ log phần cứng", "", f"Nguồn: `{args.log.resolve()}`", "",
             "Trạng thái: **" + ("KHÔNG ĐỦ / CÓ LỖI" if errors else "ĐẦY ĐỦ, CÁC CA ĐỀU PASS") + "**", "",
             "Chỉ dùng phiên khởi động cuối cùng trong log. Không tự điền số liệu thiếu.", "",
             f"Chế độ kiểm tra bất biến: `{mode}`" + (" (kiểm tra trong mọi lời gọi ah_alloc/ah_free)" if mode == "each_call" else " (chỉ kiểm tra ở ranh giới lượt)" if mode == "boundary" else ""), ""]
    if errors:
        lines += ["Các vấn đề:", ""] + [f"- {e}" for e in errors] + [""]
    lines += ["## Cấp phát theo kịch bản", "",
              "SHARED_QUOTA là heap chung 8 KiB, mỗi nhóm giữ tối đa 6144 byte (cùng trần với ADAPTIVE). B_OVER_RESERVE: B cần nhiều hơn phần dự trữ 2048 byte của ADAPTIVE trong lúc A giữ tối đa.", "",
              "| Chế độ | Kịch bản | Lượt | A thành công TB | B yêu cầu | B thành công TB | B OOM/Quota tổng | B lỗi chèn tổng | Mượn A cao nhất (byte tính cả header) |",
              "|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for scenario in SCENARIOS:
        for policy in CONFIGS:
            rows = [r for r in results if r["policy"] == policy and r["scenario"] == scenario]
            if rows:
                b_asked = statistics.mean(r['b_ok'] + r['b_no_memory'] + r['b_quota'] + r['b_injected'] for r in rows)
                lines.append(f"| {policy} | {scenario} | {len(rows)} | {statistics.mean(r['a_ok'] for r in rows):.2f} | {b_asked:.0f} | {statistics.mean(r['b_ok'] for r in rows):.2f} | {sum(r['b_no_memory']+r['b_quota'] for r in rows)} | {sum(r['b_injected'] for r in rows)} | {max(r['a_peak_borrowed'] for r in rows)} |")
    timing_note = ("Các phép đo bao gồm chờ mutex và ảnh hưởng lập lịch, không gồm lượt kiểm tra bất biến (chỉ chạy ở ranh giới lượt)"
                   if mode == "boundary" else "Các phép đo bao gồm kiểm tra bất biến, chờ mutex và ảnh hưởng lập lịch")
    lines += ["", "## Thời gian lời gọi quan sát được", "", timing_note + "; không phải WCET hoặc bảo đảm deadline. Trung bình cấp phát gồm cả lời gọi thất bại. Không lấy timestamp dòng log làm thời gian cấp phát. Không trộn số đo của hai chế độ kiểm tra bất biến trong cùng một bảng.", "",
              "| Chế độ | Nhóm | Cấp phát TB (us) | Cấp phát max quan sát (us) | Giải phóng TB (us) |", "|---|---|---:|---:|---:|"]
    for policy in CONFIGS:
        for group in ("A", "B"):
            rows = [r for r in timings if r["policy"] == policy and r["group"] == group]
            calls = sum(r["alloc_calls"] for r in rows)
            frees = sum(r["free_calls"] for r in rows)
            if calls:
                avg_free = sum(r['free_us_sum'] for r in rows)/frees if frees else 0
                lines.append(f"| {policy} | {group} | {sum(r['alloc_us_sum'] for r in rows)/calls:.2f} | {max(r['alloc_us_max'] for r in rows)} | {avg_free:.2f} |")
    lines += ["", "Giới hạn: tải bộ đệm tổng hợp chạy trên ESP32-S3, chưa tích hợp camera thật. SHARED/STATIC/ADAPTIVE/SHARED_QUOTA cùng arena thô 8 KiB; payload khả dụng khác nhau do số header và guard cuối mỗi vùng. Các kịch bản hiện chỉ đo dung lượng; lợi thế cô lập lỗi của ADAPTIVE so với SHARED_QUOTA chưa có kịch bản đo. Không suy diễn PASS thành miễn nhiễm mọi lỗi bộ nhớ.", ""]
    (args.out / "summary.md").write_text("\n".join(lines), encoding="utf-8")
    print(args.out / "summary.md")
    return 2 if errors else 0

if __name__ == "__main__":
    raise SystemExit(main())
