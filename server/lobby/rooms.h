#ifndef UNO_ROOMS_H
#define UNO_ROOMS_H

#include "shared/contracts.h"
#include "shared/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MAX_ROOMS = 16,
    ROOM_MAX_SEATS = MATCH_SEAT_COUNT,
    MAX_INVITES = 64,
    MAX_PARTIES = 32,
    PARTY_MAX_MEMBERS = 2,
    /* Lời mời chưa trả lời sau 30 giây thì hết hạn và nhường ô cho lời mời mới. */
    INVITE_TIMEOUT_MS = 30000,
    /* Trận ghép từ hàng chờ dùng mã phòng "Q" + 5 chữ số, nên bộ đếm quay vòng ở đây. */
    QUEUE_ROOM_SERIAL_LIMIT = 100000,
    ROOM_CODE_LENGTH = 6,
    ROOM_CODE_SIZE = ROOM_CODE_LENGTH + 1,
    /* Mã lời mời là UUID dạng chuỗi 36 ký tự cộng NUL. */
    INVITE_ID_SIZE = 37,
    INVITE_KIND_SIZE = 16
};

typedef enum {
    ROOM_STATUS_WAITING,
    ROOM_STATUS_IN_MATCH
} RoomStatus;

typedef struct {
    char roomId[ROOM_CODE_SIZE];
    Mode mode;
    bool ranked;
    PlayerSpec players[ROOM_MAX_SEATS];
} RoomStart;

typedef Status (*OnMatchStart)(App *app, const RoomStart *roomStart, MatchId *outMatchId);

typedef struct {
    bool occupied;
    bool isBot;
    UserId userId;
    char name[PLAYER_NAME_SIZE];
    char avatar[AVATAR_ID_SIZE];
    BotLevel botLevel;
} RoomSeat;

typedef struct {
    bool active;
    char code[ROOM_CODE_SIZE];
    Mode mode;
    bool ranked;
    Seat hostSeat;
    RoomSeat seats[ROOM_MAX_SEATS];
    uint64_t revision;
    RoomStatus status;
} Room;

typedef struct {
    bool active;
    char inviteId[INVITE_ID_SIZE];
    UserId senderId;
    UserId recipientId;
    char kind[INVITE_KIND_SIZE]; /* "room" hoặc "party" */
    char roomCode[ROOM_CODE_SIZE];
    Mode mode; /* chỉ dùng cho lời mời party: chế độ mà party mới sẽ xếp hàng */
    uint64_t createdAtMs;
} Invite;

typedef struct {
    bool active;
    uint32_t partyId;
    UserId leaderId;
    UserId members[PARTY_MAX_MEMBERS];
    size_t memberCount;
    Mode mode;
    uint64_t revision;
} Party;

struct Rooms {
    App *app;
    OnMatchStart onStart;
    Room rooms[MAX_ROOMS];
    Invite invites[MAX_INVITES];
    Party parties[MAX_PARTIES];
    uint32_t nextPartyId;
    uint32_t nextInviteId;
    uint32_t nextQueueRoom; /* số thứ tự cho mã trận hàng chờ "Qnnnnn" kế tiếp */
};

/**
 * @brief Cấp phát bảng phòng, lời mời và party.
 * @param[out] outRooms Nhận các bảng; giải phóng bằng roomsDestroy.
 * @param[in] app Trạng thái server mà các bảng dùng để push; chỉ mượn.
 * @param[in] onStart Được roomsStart/roomsMakeMatch gọi để mở trận.
 * @return OK, BAD_REQUEST khi @p outRooms là NULL, hoặc STORAGE_ERROR khi hết bộ nhớ.
 */
Status roomsInit(Rooms **outRooms, App *app, OnMatchStart onStart);

/** @brief Giải phóng các bảng; NULL được bỏ qua. */
void roomsDestroy(Rooms *rooms);

/**
 * @brief Chọn ngẫu nhiên mã phòng 6 ký tự trong [0-9A-Z].
 *
 * Mã trông giống mã trận hàng chờ ("Q" + 5 chữ số) hoặc trùng phòng đang mở
 * thì được rút lại.
 *
 * @param[in] rooms Bảng dùng để kiểm tra trùng, hoặc NULL để bỏ bước đó.
 * @param[out] outCode Buffer ROOM_CODE_SIZE byte, kết thúc bằng NUL.
 */
void roomsGenerateCode(Rooms *rooms, char outCode[ROOM_CODE_SIZE]);

/** @brief Phòng đang mở có mã này (không phân biệt hoa thường), hoặc NULL. */
Room *roomsFindByCode(Rooms *rooms, const char *code);

/** @brief Phòng đang mở mà @p userId ngồi với tư cách người chơi thật, hoặc NULL. */
Room *roomsFindByUser(Rooms *rooms, UserId userId);

/**
 * @brief Mở phòng chờ, không ranked, với @p hostId ở ghế 0.
 * @param[in] name Tên hiển thị, hoặc NULL để dùng "Host"; được chép lại.
 * @param[in] avatar Mã avatar, hoặc NULL để dùng "fox"; được chép lại.
 * @param[out] outCode Nhận mã phòng mới; có thể NULL.
 * @return OK; BAD_REQUEST khi đối số sai; BUSY khi host đã ngồi ở phòng khác;
 *         SERVER_FULL khi đã dùng hết MAX_ROOMS phòng.
 */
Status roomsCreate(Rooms *rooms, UserId hostId, const char *name, const char *avatar, Mode mode,
                   char outCode[ROOM_CODE_SIZE]);

/**
 * @brief Đặt @p userId vào ghế trống có số nhỏ nhất của phòng mang mã @p code.
 * @param[out] outSeat Nhận số ghế; có thể NULL.
 * @return OK; BAD_REQUEST khi đối số sai; BUSY khi user đã ngồi ở phòng khác;
 *         NOT_FOUND khi mã không tồn tại; ROOM_FULL khi hết ghế.
 */
Status roomsJoin(Rooms *rooms, UserId userId, const char *name, const char *avatar,
                 const char *code, Seat *outSeat);

/**
 * @brief Giải phóng ghế của user; người thật cuối cùng rời đi thì đóng phòng, host
 *        rời đi thì quyền host chuyển cho người thật ở ghế nhỏ nhất còn lại.
 * @return Luôn OK, kể cả khi user không ngồi phòng nào.
 */
Status roomsLeave(Rooms *rooms, UserId userId);

/**
 * @brief Đặt bot mức @p level vào ghế @p seat (chỉ host).
 * @return OK; BAD_REQUEST khi ghế sai; NOT_FOUND khi không có phòng; FORBIDDEN với
 *         người không phải host; INVALID_ACTION với ghế host hoặc ghế người thật.
 */
Status roomsSetBot(Rooms *rooms, UserId requesterId, int seat, BotLevel level);

/**
 * @brief Làm trống một ghế đang có người, trừ ghế host (chỉ host).
 * @return OK; BAD_REQUEST khi ghế sai; NOT_FOUND khi không có phòng; FORBIDDEN với
 *         người không phải host; INVALID_ACTION với ghế host hoặc ghế trống.
 */
Status roomsKick(Rooms *rooms, UserId requesterId, int seat);

/**
 * @brief Đổi chế độ chơi của phòng (chỉ host).
 * @return OK, BAD_REQUEST, NOT_FOUND khi không có phòng, hoặc FORBIDDEN với người
 *         không phải host.
 */
Status roomsSetMode(Rooms *rooms, UserId requesterId, Mode mode);

/**
 * @brief Bắt đầu trận cho phòng chờ đã đủ người qua Rooms.onStart.
 *
 * Phòng chỉ chuyển sang ROOM_STATUS_IN_MATCH khi callback thành công; nếu thất
 * bại, phòng vẫn đang chờ và không đổi gì.
 *
 * @return OK; BAD_REQUEST; NOT_FOUND khi không có phòng; INVALID_ACTION khi phòng
 *         không ở trạng thái chờ hoặc còn ghế trống; BUSY khi không có callback;
 *         còn lại là lỗi của callback.
 */
Status roomsStart(App *app, const char *roomId);

/**
 * @brief Lưu lời mời vào phòng hoặc party vào một ô trống hoặc đã hết hạn.
 *
 * Ô được dùng lại khi lời mời trong đó cũ hơn INVITE_TIMEOUT_MS.
 *
 * @param[in] kind "room" hoặc "party"; được chép lại.
 * @param[in] roomCode Phòng mà lời mời trỏ tới (lời mời phòng); được chép lại.
 * @param[out] outInviteId Nhận UUID ngẫu nhiên; buffer INVITE_ID_SIZE byte.
 * @return OK, BAD_REQUEST khi thiếu user, hoặc SERVER_FULL khi hết ô.
 */
Status roomsInviteCreate(Rooms *rooms, UserId senderId, UserId recipientId, const char *kind,
                         const char *roomCode, char outInviteId[INVITE_ID_SIZE]);

/**
 * @brief Dọn dẹp khi user rớt mạng: rời party, hủy lời mời user đã gửi hoặc nhận,
 *        và nhả ghế ở phòng chờ (phòng đó nhận ROOM_STATE mới). Ghế trong trận
 *        đang chạy được giữ cho RESUME/LOGIN.
 * @param[in] nowMs Hiện chưa dùng.
 */
void roomsDisconnect(App *app, UserId userId, uint64_t nowMs);

/**
 * @brief Bắt đầu trận ranked từ hàng chờ cho bốn người thật qua Rooms.onStart.
 *
 * Dựng RoomStart{roomId = "Q" + 5 chữ số tăng dần, ranked = true} với tên và
 * avatar từng ghế lấy từ Store. Không tạo phòng nào: roomsMatchEnded bỏ qua mã
 * "Q", và mã phòng thường không bao giờ có dạng đó.
 * @param[in,out] app Trạng thái server; app->rooms cung cấp onStart.
 * @param[in] mode Chế độ của trận.
 * @param[in] users Tài khoản theo thứ tự ghế; 0 (bot) bị từ chối.
 * @param[out] outMatchId Nhận mã trận khi thành công.
 * @return OK; BAD_REQUEST khi đối số NULL hoặc có user 0; BUSY khi không có
 *         callback onStart; lỗi tra Store; hoặc lỗi của onStart (matchesStart)
 *         như BUSY, SERVER_FULL, STORAGE_ERROR.
 */
Status roomsMakeMatch(App *app, Mode mode, const UserId users[ROOM_MAX_SEATS],
                      MatchId *outMatchId);

/**
 * @brief Xử lý ROOM_CREATE, ROOM_JOIN, ROOM_LEAVE, ROOM_MODE, ROOM_BOT, ROOM_KICK,
 *        ROOM_START, INVITE_SEND, INVITE_REPLY và PARTY_LEAVE.
 *
 * Người thực hiện luôn lấy từ phiên của kết nối, không lấy từ payload. Mọi lỗi
 * nghiệp vụ được trả về client dưới dạng ERROR {code, message}.
 *
 * @return OK khi đã trả lời; BAD_REQUEST khi đối số NULL.
 */
Status roomsHandle(App *app, ConnId connId, const Envelope *envelope);

/** @brief Gửi ROOM_STATE của @p room cho mọi người thật đang ngồi trong phòng. */
Status roomsBroadcastState(App *app, const Room *room);

/**
 * @brief Nhả phòng khi trận của nó đã chốt để thành viên có thể chơi tiếp.
 * @param[in,out] app Trạng thái server; app->rooms sở hữu bảng phòng.
 * @param[in] roomId Mã phòng gắn với trận vừa xong; mã rỗng hoặc lạ (trận hàng
 *            chờ, hoặc phòng mà mọi người thật đã rời) được bỏ qua, cũng như
 *            phòng không ở ROOM_STATUS_IN_MATCH.
 */
void roomsMatchEnded(App *app, const char *roomId);

/**
 * @brief Gửi ROOM_STATE và PARTY_STATE hiện tại tới kết nối đang sống của user.
 *
 * Dùng sau LOGIN/RESUME: đăng nhập lại chuyển phiên sang socket mới mà không rời
 * phòng hay party, và socket đó chưa nhận snapshot nào.
 * @param[in,out] app Trạng thái server do vòng poll sở hữu.
 * @param[in] userId Tài khoản vừa gắn vào một kết nối.
 */
void roomsSyncUser(App *app, UserId userId);

/**
 * @brief Chép tên và avatar mới của tài khoản vào ghế ở phòng chờ.
 *
 * Phòng còn WAITING thì tăng revision và phát ROOM_STATE; phòng đã vào trận giữ
 * danh tính lúc tạo trận.
 * @param[in,out] app Trạng thái server; app->rooms sở hữu bảng phòng.
 * @param[in] userId Tài khoản vừa đổi hồ sơ; user không ở phòng nào được bỏ qua.
 * @param[in] name Tên hiển thị mới, đã kiểm tra.
 * @param[in] avatar Mã avatar mới, đã kiểm tra.
 */
void roomsUpdateIdentity(App *app, UserId userId, const char *name, const char *avatar);

/** @brief Party đang có @p userId là thành viên, hoặc NULL. */
Party *partyFindByUser(Rooms *rooms, UserId userId);

/**
 * @brief Giải tán party của @p userId (party tối đa hai người, nên một người rời
 *        là hết party) và báo PARTY_STATE rỗng cho các thành viên cũ.
 * @return Luôn OK, kể cả khi user không ở party nào.
 */
Status partyLeave(Rooms *rooms, UserId userId);

#endif
