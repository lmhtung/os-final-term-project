# Thiết kế cơ chế quản lý bộ nhớ đa heap thích nghi

## Vấn đề và câu hỏi nghiên cứu

Heap dùng chung cho phép một nhóm tác vụ tiêu thụ toàn bộ dung lượng khả dụng. Phân vùng cố định bảo vệ dung lượng của từng nhóm, nhưng hạn chế khả năng phục vụ tải tăng đột biến khi nhóm khác đang sử dụng ít bộ nhớ.

Câu hỏi nghiên cứu: với cùng tổng ngân sách, cơ chế vùng riêng kết hợp mượn vùng chung có tăng khả năng phục vụ nhu cầu của A, đồng thời giữ khả năng đáp ứng nhu cầu đã xác định của B khi A gây áp lực bộ nhớ hay không?

Memory QoS trong thiết kế gồm dung lượng dự trữ, trần sử dụng, kiểm tra chủ sở hữu và phản hồi lỗi. OOM fault injection tạo các thất bại có kiểm soát để kiểm thử cách xử lý thiếu bộ nhớ.

## Tổ chức bộ nhớ và thuật toán

Arena 8192 byte RAM nội được căn chỉnh 16 byte. SHARED có một vùng 8192 byte; STATIC có hai vùng 4096 byte; ADAPTIVE có hai vùng riêng 2048 byte và một vùng chung 4096 byte.

Mỗi block chứa các trường `span`, `requested`, `next`, `owner`. `span` là dung lượng chiếm dụng gồm header, payload và căn chỉnh. Free list và allocated list được sắp xếp theo địa chỉ.

Cấp phát dùng first-fit. Khối được tách khi phần dư chứa được header và ít nhất 16 byte payload; nếu phần dư quá nhỏ, toàn bộ khối được tính vào dung lượng cấp phát. Khi giải phóng, allocator xác nhận địa chỉ đầu payload trong allocated list, kiểm tra chủ sở hữu rồi gộp với các khối trống liền kề.

ADAPTIVE tìm khối trong vùng riêng trước, sau đó tìm trong vùng chung nếu hạn mức cho phép. Block thuộc vùng nguồn đến khi được giải phóng. Vì vậy chủ sở hữu nhóm và vùng nguồn là hai thuộc tính khác nhau. Cơ chế thích nghi thể hiện qua lượng mượn đang sử dụng, không di chuyển buffer hoặc thay đổi ranh giới vật lý của vùng.

## Ngân sách và Memory QoS

- `live_bytes`: tổng span của các block nhóm đang giữ.
- `peak_bytes`: mức sử dụng cao nhất.
- `borrowed_bytes`: tổng span của các block nhóm mượn từ vùng chung.
- `granted_bytes`: dung lượng vùng riêng cộng lượng đang mượn trong ADAPTIVE.
- `limit_bytes`: trần dung lượng sử dụng của nhóm, tính cả chi phí allocator.

Một yêu cầu cần thỏa cả điều kiện có khối phù hợp và hạn mức còn lại. Tăng trần không tạo thêm RAM. API `ah_set_limit` từ chối giảm trần dưới lượng đang dùng hoặc dưới mức dự trữ của ADAPTIVE. Khi giải phóng hết block mượn, `borrowed_bytes` trở về 0.

Vùng riêng của B bảo vệ nhu cầu nằm trong ngân sách dự trữ trước áp lực cấp phát của A. Ưu tiên CPU cao hơn của B tác động đến lập lịch; phần bảo vệ dung lượng đến từ chính sách phân vùng. Buffer đang dùng không bị thu hồi cưỡng bức.

## Kiểm tra lỗi và bảo toàn trạng thái

Trước các thao tác cấp phát, giải phóng và thống kê, allocator kiểm tra:

- Metadata nằm trong vùng và đúng căn chỉnh; span hợp lệ.
- Danh sách tăng theo địa chỉ, không chồng lấn hoặc có chu trình vượt giới hạn.
- Các khối trống liền kề được gộp.
- Hợp của hai danh sách phủ đúng toàn vùng, không có khoảng hở hoặc khối trùng.
- Tổng dung lượng live và borrowed khớp các bộ đếm.

Khi phát hiện trạng thái không hợp lệ, API trả `AH_CORRUPT` và bộ thực nghiệm dừng. Con trỏ giải phóng được đối chiếu với allocated list trước khi thao tác; con trỏ lệch, nằm ngoài vùng hoặc sai chủ sở hữu bị từ chối.

Một mutex tĩnh chung bảo vệ tất cả vùng và hạn mức. API dành cho ngữ cảnh task, không dùng trong ISR. Bên gọi có trách nhiệm cung cấp địa chỉ ghi hợp lệ cho tham số đầu ra. Allocator phát hiện các bất biến bị vi phạm nhưng không tự sửa metadata.

## Chèn lỗi và xử lý OOM

`ah_inject_every` cấu hình từ chối mỗi yêu cầu hợp lệ thứ N của từng nhóm được chọn; giá trị 0 tắt injection. Lỗi được tạo trước khi thay đổi trạng thái cấp phát.

`NO_MEMORY` biểu thị thiếu khối phù hợp; `QUOTA` biểu thị vượt hạn mức; `INJECTED` biểu thị lỗi có chủ đích. Worker ghi nhận lỗi, bỏ yêu cầu thất bại và tiếp tục xử lý các buffer đã được cấp phát hợp lệ. Sau lượt thử, dữ liệu được kiểm tra và buffer được giải phóng về vùng nguồn.

## Phương pháp thực nghiệm

Các chính sách dùng cùng arena và cùng thuật toán cấp phát. SHARED có một header khởi tạo, STATIC có hai và ADAPTIVE có ba; vì vậy payload trống ban đầu khác nhau. Ranh giới vùng cũng ảnh hưởng kích thước lớn nhất của một buffer.

Các lượt NORMAL, BURST, PRESSURE và INJECTED sử dụng semaphore để xác lập thứ tự A cấp phát và giữ buffer trước khi B yêu cầu. A chạy trên lõi 0 với ưu tiên 3; B chạy trên lõi 1 với ưu tiên 4. B yêu cầu bốn buffer 256 byte, nằm trong phần dự trữ 2048 byte của ADAPTIVE. STATIC có phần dự trữ B là 4096 byte; phép so sánh giữ bằng nhau tổng arena và workload, không giữ bằng nhau dung lượng riêng của B.

Bộ kiểm thử gồm:

| Nhóm | Nội dung và quy mô |
|---|---|
| API và con trỏ | Căn chỉnh, kích thước không hợp lệ, sai chủ sở hữu, con trỏ sai, double-free và reset khi còn block sống |
| Phân mảnh | Tạo các khoảng trống rời, kiểm tra yêu cầu thất bại và gộp khối sau giải phóng |
| Chính sách | Mượn trả, điều chỉnh hạn mức và injection theo nhóm |
| Ngẫu nhiên | 4000 thao tác mỗi chính sách, seed cố định, kiểm tra dữ liệu và bất biến |
| SMP | 1000 lần cấp phát/giải phóng mỗi task mỗi chính sách trên hai lõi |
| So sánh workload | Ba chính sách × bốn kịch bản × ba lần lặp mặc định |

Test runner C chạy trên bo và xuất PASS/FAIL. Kiểm thử SMP kiểm tra truy cập đồng thời; phép thử có thứ tự đánh giá hành vi chính sách dưới áp lực đã xác định. Hai nhóm kiểm thử phục vụ các mục đích khác nhau.

## Tiêu chí đánh giá và phạm vi kết luận

Kết quả được xác định từ log UART đầy đủ. Các tiêu chí gồm số yêu cầu được phục vụ, từng loại lỗi, lượng mượn, mức sử dụng cao nhất, bảo toàn mẫu dữ liệu, thu hồi bộ nhớ và thời gian lời gọi.

Hiệu quả thích nghi được đánh giá qua khả năng phục vụ tải BURST của A; khả năng bảo vệ được đánh giá qua cấp phát của B trong PRESSURE. Lỗi injection được phân tích riêng vì dự trữ bộ nhớ không ngăn lỗi chủ động này. PASS xác nhận các điều kiện của workload đã chạy, không chứng minh hệ thống chịu được mọi dạng lỗi.

Phạm vi triển khai là allocator tầng ứng dụng trong RAM nội. Thiết kế không thay thế heap hệ thống, không cách ly phần cứng và không bảo vệ tác vụ bỏ qua API hoặc ghi sai địa chỉ. Double-free chỉ được nhận diện trước khi địa chỉ được cấp lại. Một yêu cầu không thể ghép nhiều vùng rời thành một buffer liên tục.

Thời gian đo bao gồm khóa, kiểm tra bất biến và lập lịch; không xác lập WCET hoặc deadline. Camera, PSRAM, thu hồi cưỡng bức, tự sửa rò rỉ và chính sách giảm chất lượng xử lý nằm ngoài phạm vi thực nghiệm.
