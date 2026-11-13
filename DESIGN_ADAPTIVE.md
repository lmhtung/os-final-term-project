# Thiết kế bản adaptive-v1 và cách đối chiếu báo cáo

Tên đề xuất: **Thiết kế và đánh giá cơ chế quản lý bộ nhớ đa heap thích nghi nhằm bảo vệ tác vụ quan trọng trên ESP32-S3 FreeRTOS.**

## Câu hỏi nghiên cứu

Với tổng ngân sách cố định, cơ chế phần riêng + mượn vùng chung có phục vụ được tải tăng của A tốt hơn chia cứng, đồng thời giữ khả năng đáp ứng nhu cầu đã xác định của B khi A gây áp lực không?

Memory QoS ở bản này gồm phần dự trữ, trần sử dụng, kiểm tra chủ sở hữu và cách trả lỗi. OOM injection là công cụ kiểm thử, không phải thuật toán thích nghi.

## Cơ chế và các giới hạn ngân sách

Mỗi block gồm `span`, `requested`, `next`, `owner`. `span` bao gồm header đã làm tròn và payload/căn chỉnh. Free list và allocated list sắp xếp theo địa chỉ; first-fit tìm khối, tách phần dư nếu chứa được header và ít nhất 16 byte payload. Free xác nhận địa chỉ đầu payload trong allocated list rồi gộp với hai láng giềng free.

Các nhóm dùng chung thuật toán và arena căn chỉnh 16 byte. ADAPTIVE tìm trong vùng riêng trước, rồi vùng chung nếu hạn mức và dung lượng cho phép. Block vẫn thuộc vùng nguồn đến khi free; chủ sở hữu nhóm và nguồn vùng là hai khái niệm khác nhau. `borrowed_bytes` đếm span của các block thuộc vùng chung; tăng theo cấp phát và giảm về 0 khi trả hết. Không thu hồi block đang được sử dụng.

`granted_bytes` mô tả dung lượng vùng riêng cộng phần chung đã mượn; `limit_bytes` là trần riêng biệt. Một request cần thỏa cả điều kiện khối phù hợp lẫn trần còn lại. Việc tăng trần không tạo thêm RAM. `ah_set_limit` từ chối giảm dưới mức đang dùng hoặc dưới mức dự trữ của ADAPTIVE.

Không có chuyện chuyển 8 MB PSRAM vào RAM nội bằng thay quota. Bản thí nghiệm dùng RAM nội. PSRAM và camera thật là phần mở rộng sau.

## Bảo toàn trạng thái

Trước các thao tác cấp phát/free/thống kê, kiểm tra: con trỏ metadata trong vùng và đúng căn chỉnh; span hợp lệ; danh sách tăng địa chỉ; không chồng lấn; không chu trình vượt giới hạn; hai free block kề nhau đã gộp; hợp hai danh sách phủ đúng toàn vùng; tổng live/borrowed khớp số đếm. Nếu phát hiện lỗi, trả `AH_CORRUPT` và bộ chạy dừng.

Mutex tĩnh chung bảo vệ các vùng và quota. Các API dành cho task context, không ISR. Output pointer phải là địa chỉ ghi được do caller cung cấp. Allocator không chứng minh mọi địa chỉ đầu ra là hợp lệ và không tự sửa lỗi metadata.

## Thiết kế thực nghiệm

Các lượt so sánh đều dùng arena 8192 byte. STATIC có 2 header khởi tạo, ADAPTIVE có 3, SHARED có 1; payload trống ban đầu khác nhau. Chính sách chia vùng cũng làm khác khả năng cấp một buffer lớn. Báo cáo cần thể hiện điều này, không đánh đồng raw arena với payload.

Các lượt NORMAL/BURST/PRESSURE/INJECTED là kịch bản có thứ tự A giữ trước, B xin sau; dùng semaphore để tránh kết quả bị quyết định bởi cuộc đua lập lịch. Một bộ kiểm thử SMP riêng cho A trên lõi 0 và B trên lõi 1 kiểm tra dữ liệu khi hai bên cùng alloc/free. Không dùng bài SMP đó làm bằng chứng deadline.

Mọi kết quả thực nghiệm xuất phát từ log thật. Mức ưu tiên task B là 4, A là 3; ưu tiên CPU không tự dành heap. Phần bảo vệ của B đến từ vùng riêng 2048 byte, đủ cho workload bốn buffer 256 byte đã chọn. Các lỗi bị chèn vào B không bị cơ chế dự trữ ngăn lại và phải đếm riêng.

## Các điểm cần cập nhật trong `hdh`

- Mở đầu: thêm vấn đề dung lượng bị giữ cứng và câu hỏi nghiên cứu adaptive.
- Thiết kế: bổ sung chính sách ADAPTIVE, ba vùng, owner, charged bytes, borrowed bytes, hạn mức và cách xử lý lỗi.
- Đồng bộ: bản này dùng **một mutex chung**, khác phương án khóa riêng từng heap trong bản dự thảo.
- API: tên thực tế là `ah_*`; cần đồng bộ với các tên dự kiến `FT_*` trong báo cáo.
- Kiểm chứng: test runner C tự kiểm tra trên board và in PASS/FAIL; chưa dùng framework Unity dù dự thảo từng đề xuất Unity.
- Số thao tác: hiện 4000 thao tác ngẫu nhiên mỗi chính sách; SMP 1000 alloc/free mỗi task mỗi chính sách. Không ghi đã chạy 100.000 thao tác nếu chưa thực hiện.
- Thực nghiệm: dùng ba chế độ, bốn kịch bản; bổ sung file log và các bảng thật. Không dùng lại số 32/16 của demo pool cho allocator mới.
- Phục hồi: chỉ giải phóng đúng và thu hồi loan khi kết thúc lượt; không reset cưỡng bức heap đang dùng.
- Kết luận: phân biệt kết quả đã đo, giả thuyết và giới hạn; giữ trích dẫn luận văn gốc cùng nguồn hình.

Thư mục `hdh` không được tự sửa trong lần triển khai code này. Tài liệu này là hướng dẫn đồng bộ báo cáo với phiên bản code thực tế.
