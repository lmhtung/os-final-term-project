# Adaptive Multi-Heap + Memory QoS + OOM Fault Injection

Đồ án chạy trực tiếp trên **ESP32-S3 / ESP-IDF v6.1**, giao tiếp UART. Bản mặc định mới dùng **allocator kích thước biến đổi**, thay cho demo pool khối cố định. Demo cũ vẫn còn trong `main/legacy_pool_demo.c` và component `demo_pool`.

## Phạm vi bản nộp

- First-fit, căn chỉnh 16 byte, tách khối, gộp hai phía; một free list và một allocated list cho mỗi vùng.
- Cùng arena RAM nội 8192 byte: SHARED một vùng 8192; STATIC hai vùng 4096; ADAPTIVE hai vùng riêng 2048 và một vùng chung 4096.
- Theo dõi chủ sở hữu, lỗi con trỏ, số byte đang dùng, mức cao nhất, phần mượn và hạn mức.
- ADAPTIVE tự mượn khối từ vùng chung khi phần riêng không đáp ứng. Khi free, trả đúng vùng nguồn. Giới hạn tối đa mỗi nhóm 6144 byte **tính cả header và căn chỉnh**.
- Memory QoS: bảo vệ phần riêng của B trước A, trần sử dụng theo nhóm, API thay đổi trần có kiểm tra. B có ưu tiên CPU cao hơn, nhưng không có cơ chế thu hồi buffer đang dùng hoặc hàng chờ cấp phát theo ưu tiên.
- OOM fault injection có thể tái hiện: mỗi lời gọi hợp lệ thứ N của nhóm được chọn bị từ chối. Phân biệt `QUOTA`, `NO_MEMORY`, `INJECTED`.
- Kiểm thử API, con trỏ, tách/gộp, phân mảnh; chuỗi ngẫu nhiên 4000 thao tác/chế độ; kiểm thử hai lõi 2000 cấp phát/chế độ.
- 4 kịch bản × 3 chính sách × 3 lần lặp mặc định = **36 lượt so sánh**, có log CSV và thời gian lời gọi.

**Chưa được coi là kết quả phần cứng cho đến khi bạn nạp firmware và thu đủ log `SUITE_PASS`.** Các kiểm thử firmware chạy trên bo, không trên mô phỏng. Kiểm thử Python chỉ kiểm tra chương trình đọc log bằng dữ liệu fixture trong bộ nhớ.

## Chạy nhanh bằng VS Code

1. Mở folder `B:\OS\os-btl`.
2. Giữ cáp ở cổng UART; chọn cổng COM của bo (trước đây COM7).
3. Đóng monitor cũ bằng Ctrl+] để giải phóng COM.
4. Build → Flash bằng UART → Monitor.
5. Khi monitor đã mở, nhấn EN **một lần** nếu bỏ lỡ log.
6. Đợi `SUITE_PASS,cases=36`. Nếu có `SUITE_FAIL` hoặc `AH_TEST: FAIL`, lưu toàn bộ log để kiểm tra.

Nạp firmware thay thế chương trình trên bo. Không cần đổi partition table, erase-flash hoặc bật PSRAM.

Trong ESP-IDF Terminal:

```powershell
Set-Location -LiteralPath 'B:\OS\os-btl'
idf.py build
idf.py -p COM7 flash monitor
```

Nếu terminal thường không nhận `idf.py`, dùng **ESP-IDF: Open ESP-IDF Terminal**. Script `tools/build_demo.ps1` hỗ trợ build bằng bản cài v6.1 hiện có trên C và không nạp firmware.

## Thu log để nộp

Sau khi nạp, đóng IDF Monitor. Trong terminal đã kích hoạt ESP-IDF:

```powershell
python tools/capture_serial.py --port COM7 --out results/board_run_01.log
```

Khi thấy hướng dẫn, nhấn EN một lần. Script chờ tối đa 90 giây, không ghi đè file có sẵn. Nếu cần, dùng `--seconds 180`. Nếu COM thay đổi, sửa tham số. Không mở hai monitor cùng lúc.

Phân tích log:

```powershell
python tools/analyze_results.py results/board_run_01.log --out results/run_01
```

Đầu ra: `results.csv`, `timings.csv`, `summary.md`. Chương trình từ chối đánh dấu đầy đủ nếu thiếu ca, thiếu self-test, thiếu timing, trùng dòng hoặc phiên khởi động cuối bị ngắt. Exit code 2 nghĩa là không đủ dữ liệu hoặc có lỗi. Không lấy log dự kiến làm số liệu nộp.

## Các kịch bản

Mỗi request trong bảng yêu cầu **256 byte payload**, nhưng allocator còn tính header/căn chỉnh vào ngân sách. A giữ các buffer đến khi B thử xong. B yêu cầu 4 buffer và giữ chúng đến khi hoàn tất các yêu cầu của mình. Sau đó B và A kiểm tra dữ liệu, giải phóng tất cả; kiểm tra heap trở lại trạng thái ban đầu.

| Kịch bản | A yêu cầu | B yêu cầu | Lỗi chủ động | Mục đích |
|---|---:|---:|---|---|
| NORMAL | 4 | 4 | Không | Nhu cầu thấp, cả hai nhóm được phục vụ |
| BURST | 20 | 4 | Không | STATIC giới hạn A, ADAPTIVE mượn thêm để phục vụ A |
| PRESSURE | 40 | 4 | Không | SHARED có thể bị A chiếm hết các khối đủ lớn; B còn phần riêng ở STATIC/ADAPTIVE |
| INJECTED | 20 | 4 | Mỗi request thứ 3, từng nhóm | Xử lý thất bại có chủ đích, không dùng NULL, không làm hỏng heap |

Giới hạn 2 KiB của B ở ADAPTIVE đủ cho 4 request 256 byte trong cấu hình này. STATIC dành 4 KiB cho B, nên không được nói hai phương án có cùng dung lượng riêng của B; chúng chỉ có cùng tổng arena và cùng nhu cầu B đang được thử. Cần bổ sung tải B vượt 2 KiB nếu muốn nghiên cứu vượt mức dự trữ.

Trong INJECTED, B được cố ý cho thất bại ở request thứ 3. Đây không phải thất bại của cơ chế bảo vệ dung lượng. Bộ đếm injected phải được phân tích riêng.

## Đọc các dòng log

- `TEST,<tên>,PASS`: ca kiểm thử trên board vừa thành công.
- `RESULT,...`: số liệu từng kịch bản; thứ tự trường nằm ở `CSV_HEADER`.
- `TIMING,...`: tổng và max thời gian cấp phát/giải phóng; đơn vị microsecond.
- `SUITE_PASS`: đủ các vòng và các điều kiện trong chương trình đều đạt.
- `SUITE_FAIL`: dừng; không reset một arena còn đang dùng để che lỗi.

Thời gian bao gồm mutex, kiểm tra bất biến và ảnh hưởng lập lịch. Đây là số đo quan sát, không phải WCET. Không dùng số đo này để khẳng định bảo đảm deadline. Header thực tế được log ở `header_bytes`, vì vậy số lần cấp phát thành công sẽ khác demo pool cũ 32/16.

## Chọn lại demo cũ / số lần lặp

SDK Configuration Editor → **OS project demonstrations**:

- `Run the original fixed-block pool demo`: bật để chạy demo cũ, tắt để chạy suite adaptive.
- `Repetitions per policy and scenario`: mặc định 3, có thể chọn 1–20.

Sau khi thay đổi, Save → Build → Flash. Hướng dẫn demo cũ ở `README_DEMO.md` chỉ áp dụng khi chọn chế độ legacy.

## Cấu trúc mã nguồn

| File | Vai trò |
|---|---|
| `components/adaptive_heap/include/adaptive_heap.h` | API, trạng thái, thống kê |
| `components/adaptive_heap/adaptive_heap.c` | Thuật toán cấp phát, phân vùng, mượn/trả và kiểm tra |
| `main/allocator_tests.c` | Kiểm thử API, phân mảnh, quota, injection và ngẫu nhiên |
| `main/experiments.c` | Hai task, kiểm thử SMP, workload và log |
| `main/main.c` | Chọn suite mới hoặc demo cũ |
| `tools/capture_serial.py` | Thu log UART thật |
| `tools/analyze_results.py` | Chuyển log thành CSV và bảng báo cáo |

## Giới hạn phải ghi trong báo cáo

1. Chính sách thích nghi điều chỉnh phần bộ nhớ mà nhóm được dùng bằng các khoản mượn đang sống. Các vùng vật lý vẫn cố định; không resize hoặc di chuyển buffer đang dùng.
2. Một request phải nằm trọn trong một vùng; không ghép các vùng rời thành một buffer liên tục. Tổng trống đủ chưa chắc cấp phát được.
3. Không thay thế heap hệ thống. Task có thể bỏ qua API hoặc ghi RAM sai; đây không phải cách ly phần cứng.
4. Chỉ phát hiện double-free trước khi địa chỉ được cấp lại; chưa có generation handle.
5. Một mutex chung giữ phép so sánh đơn giản và bảo vệ toàn bộ metadata. Không tuyên bố hiệu năng của khóa riêng từng heap.
6. Dữ liệu thử thay đổi nhu cầu buffer trên board thật; chưa có camera, ảnh thực, driver camera hoặc bài đo deadline liên tục.
7. Xử lý OOM bằng trả mã lỗi và bỏ yêu cầu đó; chưa có giảm độ phân giải ảnh, thu hồi cưỡng bức hoặc tự sửa rò rỉ.
8. Arena 8 KiB không tính stack, RTOS objects, metadata ngoài arena, logger và phần SDK. Header block nằm trong arena và được tính vào quota.

## Kế hoạch hai ngày

**Ngày 1:** nạp firmware, chạy đủ self-test và 36 ca, lưu log nguyên bản, phân tích, xử lý mọi FAIL trước khi diễn giải hiệu quả. Chạy lại vài lần nếu còn thời gian, giữ riêng từng log.

**Ngày 2:** cập nhật báo cáo theo `DESIGN_ADAPTIVE.md`, chèn bảng từ log thật, ghi phiên bản SDK/cấu hình/firmware, kiểm tra lại demo trực tiếp và chuẩn bị phần giới hạn. Không thay kết quả chưa có bằng dự đoán.

## Đóng gói mã nguồn

```powershell
python tools/package_source.py
```

Tạo ZIP trong `B:\OS\deliverables`, gồm code, cấu hình, tài liệu và danh sách SHA256. Không gồm build cache, báo cáo `hdh` hoặc kết quả phần cứng; bổ sung báo cáo và log thật riêng khi nộp. Không ghi đè ZIP có sẵn.

Trên máy khác, cài ESP-IDF v6.1, giải nén rồi mở thư mục `os-btl` và build bằng terminal ESP-IDF. Script `build_demo.ps1` chỉ hỗ trợ đường dẫn bản cài hiện có của máy này; không cần dùng script đó trên máy khác.
