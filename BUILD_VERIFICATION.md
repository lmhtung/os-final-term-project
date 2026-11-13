# Kiểm tra bản adaptive-v1 — 13/11/2026

## Đã kiểm tra trên máy phát triển

- ESP-IDF v6.1, target ESP32-S3, flash cấu hình 16 MB, CPU 160 MHz, tick 100 Hz, PSRAM chưa bật.
- Build cuối thành công, exit code 0; app vừa partition 1 MiB (còn khoảng 83%).
- `build/os-btl.bin`: **183104 byte**.
- SHA256 firmware: `ed3c8103acaaaa75fc72ade282176e6bd90bb82b6c57e16ef91bf0cec921e9c4`.
- Linker map: arena 8192 byte ở RAM nội, địa chỉ `0x3fc96fb0` trong bản này. Stack hai worker riêng tổng 12288 byte, không nằm trong arena.
- Sáu test Python của bộ phân tích log đều đạt: log đủ, thiếu kết thúc, nhiều lần khởi động, trùng dòng, self-test FAIL và dữ liệu số sai.
- `git diff --check` không báo lỗi khoảng trắng ở các thay đổi tracked.
- Build có cảnh báo từ Kconfig/dependency của SDK; không có lỗi biên dịch ngăn tạo firmware.

## Chưa xác minh trên phần cứng cho bản mới này

Chưa nạp hoặc chạy firmware adaptive trong phiên triển khai. Kết quả PASS của demo pool cũ không thay thế bằng chứng cho allocator mới.

Cần nạp, lưu log từ `SUITE_BEGIN` đến `SUITE_PASS,cases=36` và chạy `tools/analyze_results.py`. Có 9 tên kiểm thử bắt buộc và 36 RESULT / 72 TIMING ở cấu hình mặc định. Nếu bất kỳ ca nào FAIL, không coi đó là kết quả đã hoàn thành.

Hash và kích thước chỉ áp dụng bản firmware tại lần kiểm tra này; build lại với thay đổi code/cấu hình có thể tạo hash khác. ZIP mã nguồn không bao gồm firmware hoặc log giả định.
