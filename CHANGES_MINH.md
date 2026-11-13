# Nhật ký commit của Minh

File này ghi lại **mọi commit Minh thêm vào trên code của Tùng**, để Tùng biết đã đổi gì, vì sao, và cần làm gì tiếp. Mỗi commit mới sẽ được ghi thêm vào đây; không xoá dòng cũ.

## Mốc xuất phát

| | |
|---|---|
| Code gốc của Tùng | commit `6ddac91` "Adaptive Multi-Heap + Memory QoS + OOM Fault Injection Instruction" (13/11/2026, 11:12) |
| Nhánh của Minh | `minh/d1-validate-flag`, tách từ `6ddac91` |
| Nhánh Đ10 | `minh/d10-region-isolation`, tách từ `minh/d1-validate-flag` (gồm luôn commit 1–3). Xem riêng phần Đ10: `git diff minh/d1-validate-flag..minh/d10-region-isolation` |
| Cách xem toàn bộ thay đổi | `git diff 6ddac91..minh/d1-validate-flag` |
| Nhánh `main` của Tùng | **Chưa bị đụng tới.** Mọi thay đổi chỉ nằm trên nhánh của Minh |

## Danh sách commit

| # | Commit | Ngày | Nội dung | Đổi hành vi mặc định? |
|---|---|---|---|---|
| 1 | `8263bb4` | 13/11/2026 | Tuỳ chọn bỏ validate trong `ah_alloc`/`ah_free` khi đo độ trễ (Đ1) | **Không.** Tuỳ chọn mặc định tắt |
| 2 | `8b97de8` | 13/11/2026 | Thêm mục "Đề xuất cải tiến" vào cuối `DESIGN_ADAPTIVE.md` | Không, chỉ là tài liệu |
| 3 | `d360552` | 13/11/2026 | Thêm `CHANGES_MINH.md` | Không, chỉ là tài liệu |
| 4 | `ecace7a` | 13/11/2026 | Cô lập lỗi theo vùng: guard, cách ly vùng hỏng, `ah_reset_group` (Đ10). Nhánh `minh/d10-region-isolation` | **Có**, xem mục commit 4 |
| 5 | (commit ghi mục này) | 13/11/2026 | Ghi commit 4 vào file này và vào `DESIGN_ADAPTIVE.md` | Không, chỉ là tài liệu |

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

## Commit 4 — `ecace7a`: cô lập lỗi theo vùng (Đ10)

Nhánh `minh/d10-region-isolation`, tách từ `minh/d1-validate-flag`.

**Vấn đề ở `6ddac91`.** `validate_locked()` kiểm tra cả arena, và chỉ cần một chỗ hỏng là mọi API đều trả `AH_CORRUPT`. Ở ADAPTIVE, A ghi tràn đúng 1 byte sang header đầu tiên của vùng B. Sau đó B cấp phát trong vùng riêng của mình cũng `CORRUPT`, và `ah_reset` cũng `CORRUPT`, không còn cách cứu nào ngoài khởi động lại. Như vậy lỗi của A làm B chết theo, trái với mục tiêu cô lập lỗi của đề tài.

**Đã sửa:**

| Thay đổi | Chi tiết |
|---|---|
| Guard cuối mỗi vùng | 16 byte (`AH_GUARD_BYTES`) mẫu cố định ở cuối mỗi vùng, **tính trong** vùng nên tổng RAM vẫn 8192. Ghi tràn ngắn khỏi vùng A sẽ hỏng guard của A, không hỏng header vùng B, nên lỗi được quy cho A |
| Kiểm tra theo vùng | `ah_alloc` chỉ kiểm tra vùng nó cần: vùng riêng, cộng vùng chung khi phải mượn. `ah_free` chỉ kiểm tra vùng chứa con trỏ |
| Cách ly vùng hỏng | Vùng hỏng bị đánh dấu `faulted`. Lời gọi cần vùng đó trả `AH_CORRUPT`, các vùng khác chạy tiếp. Số byte mỗi nhóm giữ trong từng vùng được lưu **ngoài** arena, nên vùng hỏng không làm sai được con số này |
| `ah_reset_group(g)` (API mới) | Vùng lành: gỡ từng block của g, block của nhóm kia giữ nguyên. Vùng bị cách ly: bỏ phần của g mà không duyệt danh sách, và dựng lại vùng khi không nhóm nào còn byte ở đó. Mọi con trỏ của g mất hiệu lực, nên task của g phải dừng hoặc khởi động lại quanh lời gọi này (giống ý "reset heap + restart task" của luận văn) |
| `ah_reset(policy)` | Không còn trả `CORRUPT`. Chỉ trả `BUSY` khi còn byte sống, kể cả byte kẹt trong vùng bị cách ly: phải `ah_reset_group` các nhóm đó trước |
| Thống kê | `ah_stats_t` thêm `guard_bytes`, `faulted_regions` (bit i là vùng i). `ah_group_stats_t` thêm `corrupt_failures` và `group_resets`. `ah_get_stats` điền số liệu cả khi trả `AH_CORRUPT` |
| `main/experiments.c` | Công thức sức chứa trừ guard. Số buffer mỗi kịch bản **không đổi** (đã so trên native: bảng của `scenario_probe` giống hệt trước) |
| `main/allocator_tests.c` | Self-test mới `region_isolation_and_group_reset` cho STATIC và ADAPTIVE: A cố ý ghi tràn 1 byte, B vẫn cấp phát, giải phóng và mượn được, dữ liệu B còn nguyên, `ah_reset_group(A)` phục hồi A |
| `tools/analyze_results.py` | Thêm tên self-test mới vào danh sách bắt buộc |

**Hành vi sau sửa** (cùng phép thử như trước, A ghi tràn 1 byte khỏi vùng riêng):

| Thao tác sau sự cố | `6ddac91` | Sau Đ10 |
|---|---|---|
| B cấp phát trong vùng riêng | `CORRUPT` | **OK** |
| B mượn vùng chung (ADAPTIVE) | `CORRUPT` | **OK** |
| A cấp phát / giải phóng | `CORRUPT` | `CORRUPT` (đúng: A là nhóm gây lỗi) |
| Phục hồi | Không có, phải khởi động lại | `ah_reset_group(A)` |

**Giới hạn còn lại:**

- Ghi tràn **dài hơn 16 byte** vẫn vượt guard và chạm vùng kế bên. Lỗi vẫn bị phát hiện, nhưng cả hai vùng bị cách ly.
- Hỏng **trong vùng chung** (ADAPTIVE) chặn việc mượn của cả hai nhóm cho tới khi cả hai reset phần của mình. Phần dự trữ của mỗi nhóm vẫn dùng được.
- **SHARED** vẫn không cô lập, vì chỉ có một vùng. Đây là cấu hình đối chứng, cố ý giữ nguyên.
- Lỗi chỉ được phát hiện khi có ai kiểm tra vùng đó. Ở chế độ đo của Đ1, `ah_alloc`/`ah_free` không tự kiểm tra, nên phải chờ `ah_validate()` ở cuối lượt.

**Đã kiểm chứng (native trên PC, không phải bo):**

- Self-test của repo, gồm cả test mới: PASS ở cả hai chế độ validate.
- `isolation_test` mới, 5 nhóm tình huống (STATIC, ADAPTIVE, hỏng vùng chung, SHARED đối chứng, ghi tràn dài hơn guard), cùng `ah_reset_group` trên vùng lành ở cả 3 policy: PASS.
- Fuzz 1000 seed × 6000 bước × 3 policy (18 triệu thao tác), có gọi `ah_reset_group` ngẫu nhiên và kiểm tra dữ liệu của nhóm kia sau mỗi lần: PASS.
- Mutation: cho allocator quay lại kiểm tra cả arena như `6ddac91` thì `isolation_test` FAIL ngay. Vậy test thật sự kiểm được tính cô lập.

**Chưa kiểm chứng, nhờ Tùng:**

- [ ] `idf.py build` (máy Minh chưa có ESP-IDF).
- [ ] Chạy suite trên bo: self-test `region_isolation_and_group_reset` phải PASS, 36 case phải PASS như cũ.

---

## Cần Tùng trả lời

- Còn code nào chưa push lên GitHub không? (GitHub hiện chỉ có `6ddac91`.)
- Muốn nhận code bằng pull request, hay tự kéo nhánh từ repo của Minh?
- Ai làm mục 1 (cô lập lỗi theo vùng) và phần criticality + thu hồi theo áp lực?
- *(Bổ sung sau commit 4.)* Mục 1 Minh đã làm ở nhánh `minh/d10-region-isolation`. Tùng xem thiết kế guard + `ah_reset_group` có ổn không, nhất là việc guard lấy 16 byte trong mỗi vùng.
