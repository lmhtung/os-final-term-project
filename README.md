# Quản lý bộ nhớ đa heap thích nghi trên ESP32-S3 FreeRTOS

> *Bổ sung (Minh, 14/11/2026): các thay đổi của Minh trên nền `6ddac91` được ghi trong [`CHANGES_MINH.md`](CHANGES_MINH.md); đọc mục "Đọc trước" ở đầu file đó. Các thay đổi đã có trên `main` từ 14/11/2026; bản gốc của Tùng là commit `6ddac91`.*

## Bài toán và mục tiêu

Khi nhiều tác vụ dùng chung một heap, một tác vụ có nhu cầu bộ nhớ lớn có thể chiếm hết các khối còn trống, khiến tác vụ quan trọng không cấp phát được. Chia bộ nhớ thành các vùng riêng giúp cô lập tình huống cạn bộ nhớ, nhưng dung lượng dành cho một nhóm có thể bị bỏ trống trong khi nhóm khác cần thêm.

Dự án triển khai **Adaptive Multi-Heap + Memory QoS + OOM Fault Injection** trên ESP32-S3 N16R8 với ESP-IDF v6.1. Mục tiêu là đánh giá khả năng phục vụ nhu cầu tăng đột biến, bảo vệ phần bộ nhớ dự trữ của tác vụ quan trọng và xử lý yêu cầu cấp phát thất bại.

## Thiết kế giải pháp

Allocator quản lý một arena **8192 byte RAM nội**, căn chỉnh 16 byte. Mỗi vùng có danh sách khối trống và danh sách khối đang sử dụng. Cấp phát dùng first-fit, tách phần dư phù hợp; giải phóng gộp các khối trống liền kề.

| Chính sách | Phân vùng | Cơ chế phục vụ |
|---|---|---|
| SHARED | Một vùng 8192 byte | A và B cùng sử dụng toàn vùng |
| STATIC | Hai vùng 4096 byte | Mỗi nhóm chỉ sử dụng vùng riêng |
| ADAPTIVE | A: 2048 byte; B: 2048 byte; chung: 4096 byte | Tìm trong vùng riêng trước, sau đó mượn vùng chung nếu còn hạn mức |

Một block được tính vào ngân sách theo toàn bộ kích thước chiếm dụng, gồm header, payload và phần căn chỉnh. Với ADAPTIVE, trần sử dụng tối đa của mỗi nhóm là 6144 byte. Khi giải phóng, block trở về vùng nguồn và phần mượn của nhóm giảm tương ứng.

Memory QoS gồm vùng dự trữ, hạn mức theo nhóm và kiểm tra chủ sở hữu khi giải phóng. API `ah_set_limit` điều chỉnh trần sử dụng; từ chối trần thấp hơn lượng đang dùng hoặc mức dự trữ của ADAPTIVE. Việc mượn trả làm thay đổi dung lượng nhóm đang sử dụng; ranh giới vật lý các vùng giữ cố định.

Một mutex tĩnh bảo vệ metadata và hạn mức. Các API dùng trong ngữ cảnh task. OOM fault injection từ chối mỗi yêu cầu hợp lệ thứ N của từng nhóm được chọn, phục vụ kiểm tra nhánh xử lý lỗi. Các trạng thái `NO_MEMORY`, `QUOTA` và `INJECTED` được thống kê riêng.

Chi tiết thuật toán, bất biến và giới hạn nằm trong [DESIGN_ADAPTIVE.md](DESIGN_ADAPTIVE.md).

## Môi trường và cách chạy

- Bo ESP32-S3 N16R8, kết nối máy tính bằng cáp dữ liệu qua cổng UART.
- ESP-IDF v6.1, target `esp32s3`, cấu hình flash 16 MB và hai lõi.
- Arena sử dụng RAM nội; bộ thử không yêu cầu PSRAM hoặc camera.
- VS Code với extension ESP-IDF hoặc terminal đã kích hoạt môi trường ESP-IDF.

### VS Code

1. Mở thư mục `B:\OS\os-btl`.
2. Chạy **ESP-IDF: Select Port to Use** và chọn cổng COM của bo.
3. Chọn phương thức nạp **UART**.
4. Đóng ứng dụng đang giữ cổng COM, sau đó chọn **Build → Flash → Monitor**.
5. Nếu bỏ lỡ log khởi động, nhấn EN một lần khi monitor đã mở.
6. Theo dõi đến `SUITE_PASS,cases=36` ở cấu hình mặc định. Lưu toàn bộ log nếu xuất hiện `SUITE_FAIL` hoặc lỗi kiểm thử.
   *Bổ sung (Minh, nhánh `minh/d8-d9-scenarios`): firmware từ nhánh này in `SUITE_BEGIN,adaptive-v2,...` và kết thúc ở `SUITE_PASS,cases=60` (4 cấu hình × 5 kịch bản × 3 lần).*

### Terminal ESP-IDF

```powershell
Set-Location -LiteralPath 'B:\OS\os-btl'
idf.py build
idf.py -p COM7 flash monitor
```

`COM7` là giá trị ví dụ; sử dụng cổng được Windows nhận diện cho bo. Nếu terminal chưa nhận `idf.py`, mở **ESP-IDF: Open ESP-IDF Terminal** trong VS Code. Thoát IDF Monitor bằng `Ctrl+]`.

Script `tools/build_demo.ps1` hỗ trợ build theo đường dẫn bản cài ESP-IDF v6.1 trên máy phát triển. Với máy khác, sử dụng terminal của bản cài ESP-IDF tương ứng.

### Cấu hình chế độ thực nghiệm

Mở SDK Configuration Editor → **OS project demonstrations**, hoặc chạy `idf.py menuconfig`:

- `CONFIG_APP_LEGACY_POOL_DEMO`: tắt để chạy suite adaptive; bật để chạy pool khối cố định theo [README_DEMO.md](README_DEMO.md). Nhãn trong giao diện là `Run the original fixed-block pool demo`.
- `Repetitions per policy and scenario`: số lần lặp, từ 1 đến 20; mặc định 3.

Lưu cấu hình, build và nạp firmware để áp dụng.

## Thiết kế thực nghiệm

Hai worker A và B chạy trên hai lõi; A có ưu tiên CPU 3, B có ưu tiên CPU 4. Trong các lượt so sánh, semaphore xác lập thứ tự: A cấp phát và giữ buffer, B thực hiện các yêu cầu của mình, sau đó cả hai kiểm tra dữ liệu và giải phóng. Thứ tự này giúp tái hiện áp lực bộ nhớ có kiểm soát.

Mỗi yêu cầu trong bảng có payload **256 byte**. B giữ các buffer đến khi hoàn tất bốn yêu cầu.

| Kịch bản | Số yêu cầu A | Số yêu cầu B | Chèn lỗi | Mục đích |
|---|---:|---:|---|---|
| NORMAL | 4 | 4 | Không | Kiểm tra phục vụ khi nhu cầu thấp |
| BURST | 20 | 4 | Không | Đánh giá lợi ích mượn vùng chung khi A tăng nhu cầu |
| PRESSURE | 40 | 4 | Không | Đánh giá khả năng cấp phát của B khi A gây cạn bộ nhớ |
| INJECTED | 20 | 4 | Mỗi yêu cầu thứ 3 của từng nhóm | Kiểm tra xử lý thất bại và bảo toàn trạng thái |
| B_OVER_RESERVE *(bổ sung)* | 40 | 12 | Không | B cần nhiều hơn phần dự trữ 2048 byte của ADAPTIVE trong lúc A giữ tối đa |

Ba chính sách × bốn kịch bản × ba lần lặp tạo **36 lượt so sánh**. *Bổ sung (nhánh `minh/d8-d9-scenarios`): thêm cấu hình thứ tư SHARED_QUOTA (heap chung 8 KiB, mỗi nhóm giữ tối đa 6144 byte, cùng trần với ADAPTIVE) và kịch bản B_OVER_RESERVE, thành 4 × 5 × 3 = **60 lượt**. Với B_OVER_RESERVE, số buffer B nhận được khi vượt phần bảo đảm là số đo, firmware chỉ kiểm tra B nhận đủ phần bảo đảm.* Bộ kiểm thử bổ sung gồm kiểm tra API/con trỏ, phân mảnh và gộp khối, mượn trả, hạn mức, injection, 4000 thao tác ngẫu nhiên cho mỗi chính sách và kiểm thử SMP với 1000 lần cấp phát/giải phóng mỗi task cho mỗi chính sách.

STATIC dành 4 KiB riêng cho B, ADAPTIVE dành 2 KiB. Hai phương án có cùng tổng arena, nhưng khác dung lượng dự trữ. Nhu cầu bốn buffer của B nằm trong mức dự trữ của cả hai. Trong INJECTED, yêu cầu thứ ba của B bị từ chối có chủ đích và được thống kê tách khỏi lỗi thiếu dung lượng.

## Thu thập và phân tích kết quả

Sau khi nạp firmware, đóng IDF Monitor. Trong terminal ESP-IDF:

```powershell
python tools/capture_serial.py --port COM7 --out results/board_run_01.log
```

Nhấn EN một lần khi script yêu cầu. Thời gian chờ mặc định là 90 giây; dùng `--seconds 180` nếu cần tăng thời gian. Mỗi lần thu log sử dụng một tên file riêng. Chỉ một chương trình được giữ cổng COM tại một thời điểm.

```powershell
python tools/analyze_results.py results/board_run_01.log --out results/run_01
```

Đầu ra gồm `results.csv`, `timings.csv` và `summary.md`. Bộ phân tích sử dụng phiên khởi động cuối, kiểm tra đủ self-test, lượt so sánh và timing; phát hiện bản ghi trùng hoặc sai định dạng. Mã thoát 2 biểu thị kết quả lỗi hoặc không đầy đủ. Kiểm thử Python sử dụng fixture trong bộ nhớ để kiểm tra bộ phân tích; dữ liệu đó không phải phép đo trên ESP32.

| Dòng log | Ý nghĩa |
|---|---|
| `TEST,<tên>,PASS` | Kiểm thử tương ứng đáp ứng các điều kiện kiểm tra |
| `RESULT,...` | Số liệu một lượt; tên trường nằm ở `CSV_HEADER` |
| `TIMING,...` | Tổng và thời gian lớn nhất của lời gọi, đơn vị microsecond |
| `SUITE_PASS` | Tất cả lượt và điều kiện kiểm tra của suite đạt |
| `SUITE_FAIL` | Suite dừng do điều kiện kiểm tra không đạt |

Kết luận thực nghiệm dựa trên log đầy đủ của firmware chạy trên bo. Chỉ số chính gồm số lần cấp phát thành công của A/B, từng loại lỗi, mức sử dụng cao nhất, lượng mượn và dung lượng thu hồi. Header được ghi trong trường `header_bytes`; chi phí này ảnh hưởng số block cấp phát được.

Thời gian lời gọi bao gồm chờ mutex, kiểm tra bất biến và ảnh hưởng lập lịch. Các số đo là quan sát của workload thử nghiệm, không xác lập WCET hoặc bảo đảm deadline.

## Phạm vi và giới hạn

- Arena 8 KiB chỉ là vùng dữ liệu ứng dụng. Stack, đối tượng RTOS, metadata ngoài arena, logger và SDK sử dụng bộ nhớ riêng; header block nằm trong arena.
- Một yêu cầu phải nằm trọn trong một vùng. Tổng bộ nhớ trống đủ vẫn có thể không đáp ứng do phân mảnh hoặc ranh giới vùng.
- Cơ chế bảo vệ dựa trên việc các tác vụ tuân thủ API. Allocator không thay thế heap hệ thống và không cung cấp cách ly phần cứng.
- Phát hiện double-free có hiệu lực trước khi địa chỉ được cấp lại; API không dùng generation handle.
- OOM được xử lý bằng mã lỗi và bỏ yêu cầu thất bại. Không thu hồi cưỡng bức buffer đang dùng hoặc tự sửa rò rỉ.
- Workload thể hiện nhu cầu buffer bằng dữ liệu mẫu. Phạm vi thực nghiệm không bao gồm camera, PSRAM, giảm độ phân giải ảnh hoặc đánh giá deadline liên tục.

## Cấu trúc mã nguồn

| Đường dẫn | Vai trò |
|---|---|
| `components/adaptive_heap/include/adaptive_heap.h` | API, trạng thái và thống kê |
| `components/adaptive_heap/adaptive_heap.c` | Cấp phát, phân vùng, mượn trả và kiểm tra bất biến |
| `main/allocator_tests.c` | Kiểm thử API, phân mảnh, hạn mức, injection và ngẫu nhiên |
| `main/experiments.c` | Worker, kiểm thử SMP, workload và xuất log |
| `main/main.c` | Điểm vào và lựa chọn chế độ thực nghiệm |
| `main/legacy_pool_demo.c` | Thực nghiệm pool khối cố định |
| `tools/capture_serial.py` | Thu log UART |
| `tools/analyze_results.py` | Phân tích log thành CSV và bảng tổng hợp |

## Đóng gói mã nguồn

```powershell
python tools/package_source.py
```

Script tạo ZIP trong `B:\OS\deliverables`, gồm mã nguồn, cấu hình, tài liệu và manifest SHA256. Build cache, thư mục báo cáo `hdh` và log phần cứng nằm ngoài gói nguồn. Trên máy khác, giải nén, mở thư mục `os-btl` và build bằng terminal ESP-IDF v6.1.
