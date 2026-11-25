# Chạy demo UNO trên Ubuntu (LAN)

Một máy chạy `uno-server`, mỗi người chơi chạy `uno-desktop` (GTK3 + WebKitGTK 4.1) trỏ tới IP LAN của máy server. Desktop tự giữ kết nối (PING 5 giây/lần), tự nối lại khi mất mạng và tự `RESUME` để về đúng ghế.

## 1. Cài gói (Ubuntu 22.04)

```bash
sudo apt update
sudo apt install -y build-essential pkg-config \
    libjson-c-dev libsqlite3-dev libsodium-dev \
    libgtk-3-dev libwebkit2gtk-4.1-dev \
    nodejs npm xvfb python3
```

`xvfb` chỉ cần cho `make desktop-smoke` trên máy không có màn hình.

## 2. Build

```bash
make server desktop          # tạo build/uno-server và build/uno-desktop
(cd client && npm ci && npm run build)   # tạo client/dist cho desktop
make test                    # unit test; xvfb-run -a make desktop-smoke để thử cửa sổ
```

## 3. Chạy server

```bash
mkdir -p runtime
build/uno-server --bind 0.0.0.0 --port 9090 --db runtime/uno.db
```

Server in `PORT 9090` khi sẵn sàng. Server đóng kết nối nào im lặng quá 15000 ms; desktop gửi PING mỗi 5000 ms nên không bị đóng khi người chơi chỉ đứng xem.

Lấy IP LAN của máy server: `hostname -I` (ví dụ `192.168.1.20`).

## 4. Mở firewall cổng 9090

```bash
sudo ufw allow 9090/tcp
sudo ufw status
```

## 5. Chạy desktop trên từng máy

```bash
build/uno-desktop --host <IP LAN> --port 9090 --assets ./client/dist
```

Ví dụ `build/uno-desktop --host 192.168.1.20 --port 9090 --assets ./client/dist`. Nên dùng IP: tên máy chỉ được phân giải một lần lúc khởi động (nếu không phân giải được, desktop báo `cannot resolve` và thoát). Nếu server chưa bật, cửa sổ vẫn mở và desktop thử nối lại (1 s, 2 s, 4 s, 8 s, 8 s, ...). Trạng thái được in ra stderr, ví dụ:

```
desktop: connected to 192.168.1.20:9090
desktop: connection lost (closed by peer or read error)
desktop: reconnecting to 192.168.1.20:9090 in 1000 ms
desktop: session resumed
```

## 6. Ghi chú WSL2

Mặc định WSL2 dùng NAT nên máy khác trong LAN không thấy server chạy trong WSL. Bật mirrored networking trong `%UserProfile%\.wslconfig` trên Windows:

```ini
[wsl2]
networkingMode=mirrored
```

Sau đó chạy `wsl --shutdown` rồi mở lại WSL. Server trong WSL khi đó dùng chung IP LAN của Windows. Cần mở thêm cổng 9090 trên Windows Defender Firewall (và Hyper-V firewall nếu bật), ví dụ trong PowerShell quyền admin:

```powershell
New-NetFirewallRule -DisplayName "UNO 9090" -Direction Inbound -Protocol TCP -LocalPort 9090 -Action Allow
```

Desktop chạy trong WSL cần WSLg (Windows 11) để hiện cửa sổ.

## 7. Kịch bản kiểm reconnect

1. Bốn máy chạy desktop, đăng nhập, vào cùng một phòng và bắt đầu ván.
2. Máy B tắt Wi-Fi hoặc rút dây mạng khoảng 10 giây rồi bật lại.
   - Máy B: UI nhận `CONNECTION_STATE` `reconnecting`, sau đó `connected` (kèm `me`) rồi `GAME_STATE`; B về đúng ghế cũ.
   - Các máy khác: ghế B hiện `connected:false` rồi trở lại `true`.
   - Nếu mạng mất mà TCP chưa báo lỗi, desktop tự coi là mất kết nối sau 15 giây không nhận được gì (log `connection lost (heartbeat timeout)`).
3. Tắt server (Ctrl+C): mọi desktop hiện `reconnecting` và vẫn mở. Bật lại server: desktop nối lại, `RESUME` bị từ chối nên UI nhận `CONNECTION_STATE` `connected` với `sessionExpired:true` và quay về màn đăng nhập.
4. Đóng cửa sổ desktop khi đang `reconnecting`: chương trình thoát ngay, không treo. `connect` chạy non-blocking nên UI không bao giờ bị đứng; mỗi lần thử tới một IP không phản hồi bị hủy sau 3 giây rồi chờ backoff.

Chẩn đoán: thêm `--loop-probe` để desktop in ra stderr mỗi khi vòng lặp GTK bị đứng quá 500 ms (`main loop stalled for N ms`) và độ trễ lớn nhất lúc thoát (`loop probe max gap N ms`).

Trong lúc chưa nối lại (hoặc đang `RESUME`), mọi yêu cầu từ UI bị trả ngay `ERROR` với `code:"DISCONNECTED"`; desktop không xếp hàng hay gửi lại yêu cầu cũ.
