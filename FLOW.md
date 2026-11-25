# UNO Multiplayer — Luồng Mạng Tổng Quan

> **Mục đích:** Tài liệu tóm tắt các luồng trao đổi mạng giữa Client và Server để đưa lên slide báo cáo và xin ý kiến giảng viên.  
> **Nguyên tắc:** Dễ hiểu, tập trung vào luồng đi của dữ liệu, không đi sâu vào chi tiết code/cài đặt.

---

## 1. Mô hình Mạng & Phân chia Dữ liệu Tổng quan

Hệ thống hoạt động theo mô hình **Client – Server tập trung** qua kết nối hai chiều liên tục (WebSocket):

* **Tại Server (Máy chủ trung tâm):** Lưu trữ **1 bộ dữ liệu ván chơi duy nhất** trong bộ nhớ (bao gồm: chồng bài rút, lá bài đang ở trên bàn, lượt đánh hiện tại, và bài trên tay của cả 4 người).
* **Tại mỗi Client (Người chơi):** Chỉ nhận được **dữ liệu hiển thị riêng cho mình** (bản nhìn tương ứng):
  * Thấy rõ chi tiết các lá bài của chính mình để chọn đánh.
  * Chỉ nhận **số lượng lá bài** của đối phương (giấu hoàn toàn thông tin lá bài của người khác để chống gian lận soi trộm bài qua mạng).

```mermaid
flowchart TD
    subgraph ServerSide["MÁY CHỦ (SERVER) — Lưu trữ 1 dữ liệu ván chơi duy nhất"]
        GS["DỮ LIỆU VÁN CHƠI TẬP TRUNG<br/>- Chồng bài rút & bài trên bàn<br/>- Lượt đánh hiện tại<br/>- Toàn bộ bài của người 1, 2, 3, 4"]
    end

    subgraph ClientSide["CÁC NGƯỜI CHƠI (CLIENT) — Mỗi người nhận góc nhìn riêng"]
        C1["Client 1: Thấy bài mình + Số lá của người khác"]
        C2["Client 2: Thấy bài mình + Số lá của người khác"]
        C3["Client 3: Thấy bài mình + Số lá của người khác"]
        C4["Client 4: Thấy bài mình + Số lá của người khác"]
    end

    GS -- "Bản hiển thị riêng của 1" --> C1
    GS -- "Bản hiển thị riêng của 2" --> C2
    GS -- "Bản hiển thị riêng của 3" --> C3
    GS -- "Bản hiển thị riêng của 4" --> C4
```

---

## 2. Các Luồng Mạng Chính Trong Ván Chơi

### Luồng 1: Hàng chờ & Ghép trận theo Trình độ (Matchmaking)

Khi có nhiều người chơi cùng lúc bấm tìm trận:
* Tất cả người chơi được đưa vào **Hàng chờ chung** trên Server kèm mức trình độ (điểm xếp hạng/ELO).
* **Nguyên tắc ghép phòng:**
  1. **Gom theo trình độ:** Server tự động tìm và gom các nhóm người chơi có trình độ tương đương nhau để tạo thành một ván đấu cân bằng.
  2. **Nới rộng biên độ theo thời gian chờ:** Với những người chưa tìm được đủ người cùng trình độ, Server sẽ mở rộng dần khoảng cách trình độ cho phép theo thời gian chờ để họ sớm tìm được trận.
  3. **Lấp Bot dự phòng:** Nếu thời gian chờ vượt quá giới hạn cho phép, Server tự động thêm Bot vào các vị trí còn trống để ván đấu được bắt đầu ngay, tránh để người chơi phải đợi quá lâu.

```mermaid
flowchart TD
    subgraph Queue["HÀNG CHỜ GHÉP TRẬN (Nhiều người cùng tìm trận)"]
        G1["Nhóm người chơi trình độ A"]
        G2["Nhóm người chơi trình độ B"]
        G3["Người chơi chưa có đối thủ cùng trình độ"]
    end

    subgraph Matcher["Bộ quét ghép trận Server"]
        M1["Gom 4 người có trình độ tương đương"]
    end

    subgraph Rooms["Phân bổ Phòng Chơi"]
        R1["PHÒNG 1: 4 người trình độ A"]
        R2["PHÒNG 2: 4 người trình độ B"]
        WAIT["Tiếp tục chờ người mới<br/>(hoặc nới rộng biên độ trình độ / lấp Bot nếu chờ lâu)"]
    end

    Queue --> Matcher
    Matcher --> R1
    Matcher --> R2
    Matcher --> WAIT
```

```mermaid
sequenceDiagram
    autonumber
    actor P as Người chơi
    participant S as Server
    actor Others as Các người chơi cùng trình độ

    P->>S: Yêu cầu tìm trận
    S-->>P: Xác nhận đang xếp hàng chờ
    
    Note over S: Gom đủ 4 người có trình độ tương đương
    
    par Thông báo vào phòng
        S-->>P: Báo tìm thấy trận + Số phòng
        S-->>Others: Báo tìm thấy trận + Số phòng
    end

    Note over S: Khởi tạo dữ liệu ván chơi chung và chia bài
    par Gửi bài ban đầu (Gửi lần lượt)
        S-->>P: Gửi bài ban đầu của P
        S-->>Others: Gửi bài ban đầu của từng người
    end
```

---

### Luồng 2: Lượt chơi & Đồng bộ bàn chơi (Trọng tâm)
Khi tới lượt, một người chơi thực hiện đánh bài hoặc rút bài:

```mermaid
sequenceDiagram
    autonumber
    actor A as Người chơi A (Đang tới lượt)
    participant S as Server (Máy chủ trung tâm)
    actor Others as Các người chơi khác

    A->>S: Đánh lá bài X (hoặc Rút bài)
    
    Note over S: Server kiểm tra trên dữ liệu ván chơi chung:<br/>1. Có đúng lượt A không?<br/>2. A có lá bài này không?<br/>3. Đánh lá này có đúng luật UNO không?

    alt Nếu ĐÁNH SAI LUẬT
        S-->>A: Báo lỗi (Lá bài không hợp lệ)
        Note over A: Bàn cờ giữ nguyên, A chọn lại
    else Nếu ĐÁNH ĐÚNG LUẬT
        Note over S: 1. Server cập nhật trực tiếp vào dữ liệu chung:<br/>- Đặt lá bài lên bàn, trừ bài của A, chuyển lượt...
        
        S-->>A: Xác nhận nước đi thành công
        
        Note over S: 2. Server gửi lần lượt bản hiển thị mới cho 4 người:
        par Gửi cập nhật bàn cờ
            S-->>A: Gửi bàn cờ mới (thấy bài còn lại của mình)
            S-->>Others: Gửi bàn cờ mới (thấy A vừa đánh lá gì, chỉ thấy A còn n-1 lá)
        end
    end
```

---

### Luồng 3: Xử lý khi rớt mạng & Kết nối lại (Reconnect)
Giúp trận đấu không bị gián đoạn khi có người chơi gặp sự cố đường truyền:

```mermaid
sequenceDiagram
    autonumber
    actor A as Người chơi A
    participant S as Server
    actor Others as Các người chơi khác

    Note over A: Mất mạng (rớt Wi-Fi, tải lại trang)
    S->>S: Nhận biết A mất kết nối<br/>-> Đánh dấu A: Tạm thời vắng mặt
    S-->>Others: Thông báo A mất mạng (bật avatar xám)
    
    Note over S: Trong lúc A vắng mặt, nếu tới lượt:<br/>Server tự động đánh hộ để ván chơi không bị treo

    Note over A: Có mạng trở lại
    A->>S: Gửi yêu cầu xin vào lại ván cũ
    S->>S: Kiểm tra: Đúng tài khoản đang trong trận
    S-->>A: Gửi ngay TOÀN BỘ trạng thái bàn chơi hiện tại
    S-->>Others: Báo A đã quay lại
    Note over A: A nhìn thấy ngay bàn cờ mới nhất và chơi tiếp
```

---

### Luồng 4: Kết thúc ván chơi

```mermaid
sequenceDiagram
    autonumber
    actor Winner as Người thắng
    participant S as Server
    actor Losers as Những người chơi khác

    Winner->>S: Đánh lá bài cuối cùng
    Note over S: Xác nhận người này hết bài -> Trận đấu kết thúc
    
    par Báo kết quả cho cả bàn
        S-->>Winner: Báo chiến thắng + Thống kê ván + Cập nhật xếp hạng
        S-->>Losers: Báo thứ hạng + Thống kê ván + Cập nhật xếp hạng
    end
    
    Note over S: Đóng phòng chơi, đưa mọi người về lại Sảnh chính
```

---

## 3. Gợi ý 4 Slide Đơn giản để Báo cáo Thầy

| Slide | Nội dung chính cần trình bày | Hình ảnh / Sơ đồ minh họa |
|---|---|---|
| **Slide 1: Mô hình mạng & Dữ liệu** | - Mô hình Client – Server tập trung qua kết nối WebSocket.<br/>- Server giữ 1 bản dữ liệu ván chơi chung duy nhất.<br/>- Mỗi Client chỉ nhận góc nhìn riêng (giấu bài đối thủ để chống gian lận). | Sơ đồ mục 1 |
| **Slide 2: Hàng chờ & Ghép trận theo Trình độ** | - Người chơi vào hàng chờ trên Server kèm mức trình độ.<br/>- Gom các nhóm có trình độ tương đương để tạo phòng.<br/>- Nới rộng biên độ trình độ nếu chờ lâu; tự động lấp Bot nếu quá hạn. | Sơ đồ Luồng 1 |
| **Slide 3: Luồng đánh bài & Đồng bộ** *(Trọng tâm)* | - Client gửi nước đi $\rightarrow$ Server kiểm tra luật trên dữ liệu chung.<br/>- Sai: Báo lỗi $\rightarrow$ Đúng: Cập nhật dữ liệu và gửi lần lượt cho 4 người. | Sơ đồ Luồng 2 |
| **Slide 4: Xử lý rớt mạng & Kết thúc ván** | - Mất mạng: Server tạm thời đánh hộ để ván không bị dừng.<br/>- Vào lại: Server gửi lại ngay bàn cờ hiện tại.<br/>- Hết bài: Cập nhật xếp hạng và giải phóng bàn chơi. | Sơ đồ Luồng 3 & 4 |
