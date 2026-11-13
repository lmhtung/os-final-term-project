# Thực nghiệm cô lập cạn bộ nhớ bằng pool khối cố định

## Bài toán và mục tiêu

Một nhóm tác vụ có thể chiếm hết pool dùng chung, khiến nhóm khác không lấy được bộ nhớ. Thực nghiệm so sánh pool dùng chung với pool chia ngân sách để minh họa khả năng cô lập cạn bộ nhớ giữa hai nhóm A và B.

Chương trình chạy trực tiếp trên ESP32-S3 với ESP-IDF v6.1 và xuất log qua UART. Mỗi lần cấp phát thành công trả về một block 256 byte. Arena dữ liệu có 8192 byte RAM nội; metadata, mutex, semaphore, stack và TCB nằm ngoài arena. Tổng RAM của firmware lớn hơn dung lượng arena.

Chế độ này được chọn bằng `CONFIG_APP_LEGACY_POOL_DEMO`. Suite cấp phát kích thước biến đổi được mô tả trong [README.md](README.md).

## Tổ chức pool và cơ chế bảo vệ

Arena gồm 32 block căn chỉnh 16 byte. Metadata bên ngoài arena ghi trạng thái: 0 là trống, 1 thuộc A, 2 thuộc B.

| Chế độ | Phạm vi A | Phạm vi B |
|---|---|---|
| SHARED | Toàn bộ 32 block | Toàn bộ 32 block |
| SPLIT | 16 block đầu | 16 block cuối |

Cả hai chế độ dùng thuật toán quét trạng thái và một mutex chung. API giải phóng kiểm tra địa chỉ đầu block và chủ sở hữu. Thay đổi chế độ bị từ chối khi còn block đang được sử dụng.

## Trình tự thực nghiệm

1. Chọn SHARED khi toàn bộ block đang trống.
2. A cấp phát đến khi nhận `NULL`, ghi mẫu `0xA5` và giữ các block.
3. A dùng semaphore cho phép B thực hiện yêu cầu trong khi A vẫn giữ bộ nhớ.
4. B thử cấp phát một block. Nếu thành công, B ghi và kiểm tra mẫu `0x5A`, sau đó giải phóng; nếu thất bại, B ghi nhận lỗi.
5. Sau tín hiệu hoàn tất của B, A kiểm tra dữ liệu rồi giải phóng các block đang giữ.
6. Kiểm tra thu hồi đủ 32 block và thực hiện cùng trình tự với SPLIT.
7. Kiểm tra các thao tác giải phóng không hợp lệ và bảo toàn trạng thái.
8. Xuất `DEMO PASS` hoặc `DEMO FAIL`. Hai worker chờ semaphore sau khi kết thúc.

Đây là tình huống cạn bộ nhớ có thứ tự xác định. Hai task không được gán cố định vào hai lõi. Nếu một vòng vượt 10 giây, chương trình báo lỗi và dừng chuyển chế độ trên vùng nhớ đang sử dụng.

## Cách chạy

### VS Code

1. Mở `B:\OS\os-btl`, kết nối bo qua cổng UART bằng cáp dữ liệu.
2. Mở SDK Configuration Editor → **OS project demonstrations**.
3. Bật `Run the original fixed-block pool demo` (`CONFIG_APP_LEGACY_POOL_DEMO`) và lưu cấu hình.
4. Chọn cổng COM bằng **ESP-IDF: Select Port to Use**, chọn phương thức nạp UART.
5. Build, đóng chương trình đang giữ COM, sau đó Flash và Monitor.
6. Nhấn EN một lần khi monitor đã mở nếu cần thu log từ đầu.

### Terminal ESP-IDF

```powershell
Set-Location -LiteralPath 'B:\OS\os-btl'
idf.py menuconfig
idf.py build
idf.py -p COM7 flash monitor
```

Trong `menuconfig`, bật chế độ pool theo mục trên. Sử dụng cổng COM thực tế của bo. Nếu terminal chưa nhận `idf.py`, mở **ESP-IDF: Open ESP-IDF Terminal** trong VS Code.

Thoát monitor bằng `Ctrl+]`. Để chỉ mở monitor:

```powershell
idf.py -p COM7 monitor
```

Khi gặp lỗi kết nối, kiểm tra cổng COM, cáp dữ liệu và ứng dụng đang giữ cổng. Nếu bo không tự vào chế độ nạp, giữ BOOT, nhấn rồi thả EN, sau đó thả BOOT và thực hiện nạp.

## Kết quả kỳ vọng và cách diễn giải

Các dòng dưới đây mô tả hành vi kỳ vọng. Kết quả thực nghiệm được ghi nhận từ log của bo:

```text
[SHARED] A: allocated=32 blocks, next allocation=NO_MEMORY
[SHARED] B: allocation=NO_MEMORY (A still holds its blocks)
[SHARED] A_blocks=32 B_success=0 free_after_cleanup=32/32 RESULT=PASS

[SPLIT] A: allocated=16 blocks, next allocation=NO_MEMORY
[SPLIT] B: allocation=SUCCESS (A still holds its blocks)
[SPLIT] A_blocks=16 B_success=1 free_after_cleanup=32/32 RESULT=PASS
```

Trong SHARED, A giữ toàn bộ 32 block nên B không cấp phát được. Trong SPLIT, A chỉ tiêu thụ 16 block của mình và B vẫn có thể lấy block từ phần riêng. Hai chế độ có cùng tổng dung lượng dữ liệu.

`NO_MEMORY` của A và thất bại cấp phát của B trong SHARED là điều kiện chủ động tạo ra. Một lượt đạt PASS khi hành vi phù hợp điều kiện kiểm tra, dữ liệu giữ nguyên và thu hồi đủ 32 block.

| Kiểm tra | Điều kiện đạt |
|---|---|
| `wrong owner rejected` | Từ chối giải phóng block của nhóm khác |
| `interior pointer rejected` | Từ chối con trỏ lệch một byte so với đầu block |
| `outside pointer rejected` | Từ chối con trỏ ngoài arena |
| `reset with live block rejected` | Từ chối đổi chế độ khi còn block sống |
| `live data and allocation preserved` | Dữ liệu và trạng thái cấp phát giữ nguyên sau thao tác sai |
| `valid free accepted` | Giải phóng với đúng chủ sở hữu và con trỏ |
| `double free rejected` | Từ chối giải phóng lại trước khi địa chỉ được cấp lại |
| `NULL free accepted` | NULL với nhóm hợp lệ không làm thay đổi trạng thái |
| `all blocks reclaimed` | Thu hồi đủ 32 block |

Dòng kết thúc thành công là `DEMO PASS: press RESET/EN to run again`. Việc `app_main()` trả về sau thực nghiệm là hành vi bình thường. Log có FAIL hoặc timeout cần được phân tích theo điều kiện lỗi tương ứng.

## Ý nghĩa và giới hạn

Phân chia ngân sách duy trì khả năng cấp phát của B trong kịch bản A gây cạn bộ nhớ. Kiểm tra con trỏ và chủ sở hữu giúp từ chối các thao tác giải phóng không hợp lệ được bao phủ bởi bộ thử.

Pool cấp phát block cố định, không có tách/gộp hoặc mượn dung lượng giữa nhóm. Cơ chế dựa trên API ứng dụng, không cung cấp cách ly phần cứng hoặc ngăn task ghi sai RAM. Không kiểm tra tổng quát ghi tràn buffer, không tự sửa metadata và không thu hồi rò rỉ tự động. Phép thử không đo độ trễ, thông lượng, deadline hoặc PSRAM.

## Cấu trúc mã nguồn

| Đường dẫn | Vai trò |
|---|---|
| `main/main.c` | Chọn chế độ thực nghiệm |
| `main/legacy_pool_demo.c` | Worker A/B, điều phối lượt thử, kiểm tra và xuất log |
| `components/demo_pool/include/demo_pool.h` | API và trạng thái lỗi |
| `components/demo_pool/demo_pool.c` | Arena, metadata chủ sở hữu, phân vùng và mutex |
