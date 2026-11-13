# Nhật ký commit của Minh

File này ghi lại **mọi commit Minh thêm vào trên code của Tùng**, để Tùng biết đã đổi gì, vì sao, và cần làm gì tiếp. Mỗi commit mới sẽ được ghi thêm vào đây; không xoá dòng cũ.

## Mốc xuất phát

| | |
|---|---|
| Code gốc của Tùng | commit `6ddac91` "Adaptive Multi-Heap + Memory QoS + OOM Fault Injection Instruction" (13/11/2026, 11:12) |
| Nhánh của Minh | `minh/d1-validate-flag`, tách từ `6ddac91` |
| Cách xem toàn bộ thay đổi | `git diff 6ddac91..minh/d1-validate-flag` |
| Nhánh `main` của Tùng | **Chưa bị đụng tới.** Mọi thay đổi chỉ nằm trên nhánh của Minh |

## Danh sách commit

| # | Commit | Ngày | Nội dung | Đổi hành vi mặc định? |
|---|---|---|---|---|
| 1 | `8263bb4` | 13/11/2026 | Tuỳ chọn bỏ validate trong `ah_alloc`/`ah_free` khi đo độ trễ (Đ1) | **Không.** Tuỳ chọn mặc định tắt |
| 2 | `8b97de8` | 13/11/2026 | Thêm mục "Đề xuất cải tiến" vào cuối `DESIGN_ADAPTIVE.md` | Không, chỉ là tài liệu |
| 3 | (commit thêm file này) | 13/11/2026 | Thêm `CHANGES_MINH.md` | Không, chỉ là tài liệu |

---

## Commit 1 — `8263bb4`: tuỳ chọn bỏ validate khi đo (Đ1)

**Vấn đề ở `6ddac91`.** `ah_alloc` và `ah_free` gọi `validate_locked()` ở **mọi** lời gọi, trong lúc giữ mutex. Hàm này duyệt toàn bộ arena và cả hai danh sách, nên mỗi lời gọi tốn O(arena). Mọi dòng `TIMING` vì vậy đo cả lượt kiểm tra chứ không chỉ allocator.

**Đã sửa:**

| File | Thay đổi |
|---|---|
| `components/adaptive_heap/adaptive_heap.c` | Macro `AH_VALIDATE_EACH_CALL`; `ah_alloc`/`ah_free` chỉ gọi `validate_locked()` khi macro bằng 1; thêm `ah_validate_each_call()` |
| `components/adaptive_heap/include/adaptive_heap.h` | Khai báo `bool ah_validate_each_call(void)` |
| `main/Kconfig.projbuild` | Tuỳ chọn `CONFIG_AH_SKIP_PER_CALL_VALIDATION`, mặc định `n` |
| `main/experiments.c` | `SUITE_BEGIN` in thêm `validate=each_call` hoặc `validate=boundary`; dòng mô tả chế độ in cho đúng |
| `tools/analyze_results.py` | Hàm `validate_mode()`; `summary.md` ghi chế độ và chú thích thời gian theo chế độ; log cũ không có trường này được hiểu là `each_call` |
| `tools/test_analyze_results.py` | Thêm `test_validate_mode` |

**Giữ an toàn mặc định.** Tuỳ chọn đặt tên theo hướng "bỏ" (skip), nên khi không bật gì thì allocator kiểm tra như cũ. `ah_validate`, `ah_get_stats` và `ah_reset` luôn kiểm tra. Ở chế độ đo, `finish_is_clean()` vẫn gọi `ah_validate()` cuối mỗi lượt.

**Cách dùng trên bo.** `idf.py menuconfig` → **OS project demonstrations** → *Skip invariant validation inside each ah_alloc/ah_free (latency measurement)*. Chạy suite hai lần, mỗi chế độ một lần, lưu hai log riêng. Không trộn số TIMING của hai chế độ trong cùng một bảng.

**Đã kiểm chứng (native trên PC, GCC 5.3 x86-64, không phải bo):**

- Chế độ mặc định: mã máy của 15 hàm có sẵn giống hệt bản `6ddac91`, từng lệnh, ở `-O1` và `-O2`.
- Self-test `allocator_tests.c`: PASS ở cả hai chế độ.
- Fuzz 200 seed × 3000 bước × 3 policy: PASS ở cả hai chế độ.
- Test cố ý làm hỏng metadata: chế độ mặc định trả `AH_CORRUPT` như cũ; chế độ đo chạy tiếp nhưng `ah_validate()` vẫn phát hiện.
- 7/7 test Python của `tools/` PASS.

**Chưa kiểm chứng, nhờ Tùng:**

- [ ] `idf.py build` với tuỳ chọn tắt và bật (máy Minh chưa có ESP-IDF).
- [ ] Chạy suite trên bo ở cả hai chế độ, gửi hai log.

**Lưu ý.** Khi bật chế độ đo, các vòng lặp trong `take_block` và `ah_free` không còn được `validate_locked()` bảo vệ khỏi danh sách có chu trình. Chỉ dùng chế độ này để đo. Sẽ có commit riêng thêm bộ đếm bước cho các vòng lặp đó.

---

## Commit 2 — `8b97de8`: mục "Đề xuất cải tiến" trong `DESIGN_ADAPTIVE.md`

Chỉ **thêm** vào cuối file, không sửa dòng nào của Tùng. Đây là **đề xuất, chưa phải quyết định của nhóm**. Tóm tắt:

1. **Lỗi ở một vùng làm tê liệt vùng khác** (ưu tiên cao nhất). A ghi tràn 1 byte sang header vùng B, sau đó mọi thao tác của B và cả `ah_reset` đều trả `AH_CORRUPT`.
2. **Kịch bản hiện tại không phân biệt ADAPTIVE với SHARED + quota.** Khi B cần vượt 2048 byte, ADAPTIVE còn thua STATIC.
3. Validate trong mọi lời gọi làm sai số đo thời gian (commit 1 đã xử lý phần này).
4. Đo lường: in `TIMING` cho lượt stress SMP, khoá theo vùng, dùng bộ đếm chu kỳ CPU.
5. Sửa mô tả: ưu tiên CPU của B, kích thước block lớn nhất.

Mục 1, 2, 4, 5 **chưa có code**, cần thống nhất ai làm.

---

## Cần Tùng trả lời

- Còn code nào chưa push lên GitHub không? (GitHub hiện chỉ có `6ddac91`.)
- Muốn nhận code bằng pull request, hay tự kéo nhánh từ repo của Minh?
- Ai làm mục 1 (cô lập lỗi theo vùng) và phần criticality + thu hồi theo áp lực?
