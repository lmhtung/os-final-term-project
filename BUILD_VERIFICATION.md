# Hồ sơ xác minh bản dựng — 13/11/2026

## Môi trường kiểm tra

ESP-IDF v6.1, target ESP32-S3, flash cấu hình 16 MB, CPU 160 MHz, tick FreeRTOS 100 Hz, PSRAM tắt. Chế độ thực nghiệm là suite adaptive với ba lần lặp cho mỗi chính sách và kịch bản.

## Kết quả build và kiểm tra trên máy phát triển

| Hạng mục | Kết quả |
|---|---|
| Build | Thành công, mã thoát 0 |
| Kích thước `build/os-btl.bin` | 183104 byte |
| Phân vùng ứng dụng | 1 MiB; firmware nằm trong dung lượng phân vùng |
| Arena theo linker map | 8192 byte RAM nội, địa chỉ `0x3fc96fb0` |
| Stack của hai worker | Tổng 12288 byte, nằm ngoài arena |
| Kiểm thử Python | Sáu ca kiểm thử bộ phân tích log đạt |
| Cảnh báo build | Cảnh báo Kconfig/dependency của SDK; không có lỗi ngăn tạo firmware |

SHA256 của firmware được kiểm tra:

```text
ed3c8103acaaaa75fc72ade282176e6bd90bb82b6c57e16ef91bf0cec921e9c4
```

Sáu ca Python kiểm tra log đầy đủ, thiếu kết thúc, nhiều phiên khởi động, bản ghi trùng, self-test FAIL và dữ liệu số sai. Các ca này sử dụng fixture để xác minh bộ phân tích.

Kích thước, địa chỉ và hash xác định artifact của lần build được ghi nhận trong hồ sơ này. Mỗi bản dựng cần được đối chiếu với cấu hình và mã nguồn tương ứng.

## Xác minh trên phần cứng

Hồ sơ này chưa có log chạy firmware adaptive trên ESP32-S3. Build thành công xác nhận khả năng biên dịch và liên kết, chưa xác nhận hành vi thực nghiệm trên bo.

Quy trình xác minh gồm nạp firmware, thu log từ `SUITE_BEGIN` đến `SUITE_PASS,cases=36`, sau đó chạy `tools/analyze_results.py`. Ở cấu hình mặc định, log đầy đủ chứa chín tên kiểm thử bắt buộc, 36 dòng RESULT và 72 dòng TIMING. Kết quả FAIL, dữ liệu thiếu hoặc phiên chạy bị ngắt không đáp ứng tiêu chí hoàn tất.

*Bổ sung (Minh, nhánh `minh/d8-d9-scenarios`):* firmware từ nhánh này ghi `adaptive-v2` ở dòng `SUITE_BEGIN`. Log đầy đủ chứa mười tên kiểm thử bắt buộc (thêm `region_isolation_and_group_reset`), 60 dòng RESULT, 120 dòng TIMING và kết thúc ở `SUITE_PASS,cases=60`. `tools/analyze_results.py` nhận ra phiên bản và vẫn phân tích log `adaptive-v1` theo tiêu chí cũ.
