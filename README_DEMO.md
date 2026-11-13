# Demo cô lập cạn bộ nhớ trên ESP32-S3

## Mục đích và trạng thái

Demo minh họa ảnh hưởng của việc phân chia ngân sách bộ nhớ giữa hai nhóm task. Chương trình chạy trực tiếp trên ESP32-S3, in kết quả qua UART. Không sử dụng mô phỏng.

Đây là **pool các khối cố định 256 byte**, chưa phải allocator kích thước biến đổi trong báo cáo `hdh`. Chưa có first-fit trên các khối kích thước khác nhau, tách/gộp khối hay phép đo phân mảnh và độ trễ. Không thay thế `malloc()` hoặc heap hệ thống của ESP-IDF.

Chỉ vùng dữ liệu ứng dụng 8192 byte được đưa vào thí nghiệm. Metadata, mutex, semaphore, stack và TCB của hai task được cấp tĩnh riêng bên ngoài vùng này. Vì vậy 8 KiB không phải tổng RAM mà firmware sử dụng. PSRAM không cần thiết cho demo này.

Đã kiểm tra build thành công bằng ESP-IDF v6.1 cho ESP32-S3: firmware `build/os-btl.bin` có kích thước 173264 byte. Linker map xác nhận arena 8192 byte trong RAM nội (địa chỉ 0x3fc95e60 ở bản build này). Cấu hình build hiện chọn flash 16 MB và chưa bật PSRAM. Build có các cảnh báo từ cấu hình/thư viện SDK, nhưng kết thúc với mã 0.

Chưa nạp hoặc chạy bản demo này trên bo trong phiên viết code. Chỉ ghi nhận demo đạt sau khi firmware được nạp và log thực tế có `DEMO PASS` cùng các kết quả bên dưới.

## Các file

- `main/main.c`: khởi tạo, hai task A/B, điều phối hai lượt thử, kiểm tra giải phóng và in kết quả.
- `components/demo_pool/include/demo_pool.h`: giao diện và mã lỗi.
- `components/demo_pool/demo_pool.c`: arena, trạng thái chủ sở hữu từng khối, giới hạn nhóm và mutex.

## Cách hoạt động

Arena có 32 khối, mỗi khối 256 byte, căn chỉnh 16 byte. Trạng thái khối nằm ngoài arena: 0 là trống, 1 thuộc A, 2 thuộc B.

| Chế độ | Phạm vi nhóm A | Phạm vi nhóm B |
|---|---|---|
| SHARED | Toàn bộ 32 khối | Toàn bộ 32 khối |
| SPLIT | 16 khối đầu | 16 khối cuối |

Cả hai chế độ dùng cùng thuật toán quét trạng thái khối và cùng mutex. Mutex chung giúp bảo vệ metadata; demo không đánh giá lợi ích của khóa riêng mỗi heap hoặc mức song song giữa hai lõi. Hai task không được gán cố định vào hai lõi.

Một lần khởi động tự chạy:

1. Chọn SHARED khi tất cả khối đang trống.
2. Task A cấp phát đến khi nhận NULL, đồng thời ghi mẫu 0xA5 và giữ các khối.
3. A báo cho B bằng semaphore. A chưa giải phóng lúc này.
4. B thử lấy một khối. Nếu thành công, B ghi/kiểm tra mẫu 0x5A và trả khối. Nếu thất bại, B ghi nhận, không truy cập con trỏ NULL.
5. B báo hoàn tất. A kiểm tra dữ liệu các khối mình giữ rồi giải phóng chúng.
6. Kiểm tra số khối thu hồi, sau đó lặp lại với SPLIT trên chính arena đó.
7. Chạy các kiểm tra giải phóng sai và bảo toàn trạng thái.
8. In `DEMO PASS` hoặc `DEMO FAIL`. Hai worker chờ semaphore; không tự khởi động lại.

Thứ tự này là một kịch bản tạo cạn bộ nhớ có kiểm soát. Nó không đo xác suất lỗi hoặc hiệu năng của workload ngoài thực tế. Nếu một vòng quá 10 giây, chương trình báo lỗi và không chuyển chế độ trên bộ nhớ còn đang được sử dụng.

## Chạy bằng VS Code

1. Mở folder `B:\OS\os-btl`.
2. Giữ cáp dữ liệu ở cổng **UART** của bo.
3. Chạy `ESP-IDF: Select Port to Use` và chọn cổng COM của bo. Lần trước là COM7; kiểm tra lại nếu Windows thay đổi cổng.
4. Chạy `ESP-IDF: Select Flash Method`, chọn **UART**.
5. Build bằng `ESP-IDF: Build Your Project`.
6. Đóng monitor/ứng dụng khác đang giữ cổng COM, rồi chạy `ESP-IDF: Flash Your Project`.
7. Mở `ESP-IDF: Monitor Your Device`. Nếu đã bỏ lỡ log đầu, nhấn RESET/EN trên bo một lần khi monitor đang mở.

Nạp firmware sẽ ghi đè chương trình ứng dụng đang có trên bo. Không cần erase-flash, thay đổi partition table hoặc thiết lập JTAG cho demo này.

## Chạy bằng terminal ESP-IDF

Mở terminal ESP-IDF đã kích hoạt môi trường, rồi chạy:

```powershell
Set-Location -LiteralPath 'B:\OS\os-btl'
idf.py build
idf.py -p COM7 flash monitor
```

Thay COM7 nếu bo sử dụng cổng khác. Nếu PowerShell thông thường chưa nhận `idf.py`, mở terminal bằng lệnh `ESP-IDF: Open ESP-IDF Terminal` trong VS Code. Có thêm script build dành cho bản cài ESP-IDF v6.1 hiện tại trên máy:

```powershell
Set-Location -LiteralPath 'B:\OS\os-btl'
& .\tools\build_demo.ps1
```

Script đọc đường dẫn công cụ từ profile trên C, thiết lập biến môi trường trong tiến trình hiện tại và chỉ build; không gọi EIM để đồng bộ lựa chọn, không sửa cài đặt hay nạp firmware. Dùng các lệnh ESP-IDF trong VS Code để flash/monitor nếu terminal chưa nhận `idf.py`.

Thoát monitor bằng `Ctrl+]`. Muốn chỉ mở lại monitor mà không nạp lại:

```powershell
idf.py -p COM7 monitor
```

Nếu mất kết nối, kiểm tra cổng COM và dây USB dữ liệu trước. Nếu không tự vào chế độ nạp, giữ BOOT, nhấn rồi thả RESET/EN, sau đó thả BOOT và thử nạp lại.

## Kết quả kỳ vọng — chưa phải số liệu đo

Bỏ qua timestamp và tiền tố log ESP-IDF; tìm các dòng chính:

```text
[SHARED] A: allocated=32 blocks, next allocation=NO_MEMORY
[SHARED] B: allocation=NO_MEMORY (A still holds its blocks)
[SHARED] A_blocks=32 B_success=0 free_after_cleanup=32/32 RESULT=PASS

[SPLIT] A: allocated=16 blocks, next allocation=NO_MEMORY
[SPLIT] B: allocation=SUCCESS (A still holds its blocks)
[SPLIT] A_blocks=16 B_success=1 free_after_cleanup=32/32 RESULT=PASS
```

`NO_MEMORY` của A và việc B thất bại ở SHARED là tình huống cố ý, không phải kết quả FAIL của demo. Vòng kiểm tra PASS khi hành vi đúng dự kiến, mẫu dữ liệu được giữ nguyên và thu hồi đủ 32 khối.

Sau đó phải có PASS cho:

- `wrong owner rejected`: A cấp phát nhưng B yêu cầu giải phóng, bị từ chối.
- `interior pointer rejected`: con trỏ lệch 1 byte, bị từ chối.
- `outside pointer rejected`: con trỏ nằm ngoài arena, bị từ chối.
- `reset with live block rejected`: từ chối đổi chế độ khi còn khối đang dùng.
- `live data and allocation preserved`: các thao tác sai không làm đổi mẫu dữ liệu hoặc số khối còn sống.
- `valid free accepted`: đúng chủ sở hữu và đúng con trỏ, được giải phóng.
- `double free rejected`: giải phóng lại trước khi địa chỉ được cấp lại, bị từ chối.
- `NULL free accepted`: NULL với nhóm hợp lệ là thao tác không làm gì.
- `all blocks reclaimed`: toàn bộ 32 khối đều trống.

Cuối cùng:

```text
DEMO PASS: press RESET/EN to run again
```

Nếu xuất hiện FAIL hoặc timeout, giữ lại log để phân tích; không ghi kết quả là thành công. Việc `app_main()` kết thúc sau demo là bình thường.

## Trình bày với thầy

Sau khi chạy thành công trên bo, có thể trình bày:

> Đây là demo ban đầu dùng pool các khối cố định. Ở chế độ dùng chung, A có thể chiếm hết 32 khối nên B không cấp phát được. Khi chia ngân sách, A dùng hết 16 khối dành cho mình nhưng B vẫn lấy được khối từ phần riêng. Hai chế độ giữ cùng tổng dung lượng dữ liệu thô. Chương trình cũng từ chối một số thao tác giải phóng không hợp lệ và kiểm tra thu hồi đầy đủ sau mỗi lượt. Bước tiếp theo là triển khai allocator kích thước biến đổi theo thiết kế báo cáo.

Minh chứng nên lưu: ảnh bo kết nối, log hai vòng SHARED/SPLIT, các kiểm tra, dòng DEMO PASS, cấu hình và phiên bản code. Không lấy đoạn log kỳ vọng trong tài liệu này làm kết quả thực nghiệm.

## Giới hạn

- Pool không có thao tác tách/gộp khối; mọi yêu cầu đều nhận 256 byte.
- Giới hạn nhóm là quy ước API của ứng dụng. Task có thể truyền mã nhóm khác hoặc ghi RAM sai nếu code cố ý/sai; đây không phải bảo vệ bằng phần cứng.
- Không có kiểm tra ghi tràn buffer hoặc tự sửa metadata bị hỏng. Các mẫu dữ liệu chỉ giúp phát hiện sai lệch trong bài thử cụ thể.
- Phát hiện double-free chỉ chắc chắn trong ca thử trước khi địa chỉ được cấp lại; không theo dõi thế hệ cấp phát.
- Không reset cưỡng bức heap, không sửa rò rỉ bộ nhớ tự động. Chỉ thu hồi bằng các lời gọi giải phóng hợp lệ.
- Chưa đo độ trễ, thông lượng, deadline, phân mảnh hoặc xác minh PSRAM.
