#ifndef UNO_QUEUE_H
#define UNO_QUEUE_H

#include "shared/contracts.h"
#include "shared/protocol.h"

enum {
    QUEUE_MATCH_SEATS = MATCH_SEAT_COUNT,
    QUEUE_ENTRY_MAX_USERS = 2,
    /* Mỗi kết nối tối đa một mục, nên đây là số mục nhiều nhất mỗi chế độ có thể có. */
    QUEUE_MAX_ENTRIES = 64,
    /* Thời gian chờ trước khi thử mở lại trận vừa thất bại (Store, hết chỗ). */
    QUEUE_RETRY_MS = 500
};

typedef struct {
    UserId users[QUEUE_ENTRY_MAX_USERS];
    size_t count; /* 1 = chơi đơn, 2 = party */
} QueueEntry;

/**
 * @brief Cấp phát hàng chờ ghép trận rỗng cho mọi chế độ.
 * @param[out] outQueue Nhận hàng chờ; hủy bằng queueDestroy.
 * @return OK khi thành công, BAD_REQUEST khi @p outQueue là NULL, STORAGE_ERROR khi
 *         không cấp phát được.
 */
Status queueInit(Queue **outQueue);

/**
 * @brief Giải phóng hàng chờ do queueInit tạo.
 * @param[in] queue Hàng chờ cần giải phóng, hoặc NULL.
 */
void queueDestroy(Queue *queue);

/**
 * @brief Thêm một mục chơi đơn hoặc party vào cuối hàng FIFO của một chế độ.
 * @param[in,out] queue Hàng chờ do vòng poll sở hữu.
 * @param[in] mode Chế độ mà mục chờ.
 * @param[in] entry Mục được chép vào hàng chờ.
 * @return OK khi đã xếp hàng; BAD_REQUEST khi chế độ, số người sai hoặc có user 0
 *         hay trùng; INVALID_ACTION khi party chờ CLASSIC hoặc DRAFT (party chỉ chơi
 *         chế độ đồng đội); BUSY khi có user đang chờ ở chế độ bất kỳ; SERVER_FULL
 *         khi chế độ đã đầy.
 */
Status queueAdd(Queue *queue, Mode mode, const QueueEntry *entry);

/**
 * @brief Kiểm tra user có đang chờ ở chế độ nào không.
 * @param[in] queue Hàng chờ, hoặc NULL.
 * @param[in] user Tài khoản cần tìm.
 * @return true khi có mục chứa @p user.
 */
bool queueContains(const Queue *queue, UserId user);

/**
 * @brief Bỏ nguyên mục chứa user, giữ thứ tự FIFO của các mục còn lại.
 * @param[in,out] queue Hàng chờ do vòng poll sở hữu.
 * @param[in] user Tài khoản có mục (đơn hoặc party) cần bỏ.
 * @param[out] outMode Nhận chế độ của mục; có thể NULL.
 * @param[out] removed Nhận bản sao mục đã bỏ; có thể NULL.
 * @return true khi đã bỏ một mục, false khi @p user không ở hàng chờ.
 */
bool queueRemove(Queue *queue, UserId user, Mode *outMode, QueueEntry *removed);

/**
 * @brief Chọn các mục sớm nhất theo FIFO cộng lại đúng bốn người chơi.
 *
 * Trong mọi tập mục có tổng bốn người, tập có danh sách index tăng dần nhỏ nhất
 * theo thứ tự từ điển được chọn: mục cũ nhất luôn được chơi khi có tập hợp lệ chứa
 * nó, và không bỏ sót tập hợp lệ nào (ví dụ [party, solo, party] ghép hai party).
 * CLASSIC/DRAFT xếp ghế theo thứ tự chọn; 2v2 đội A là ghế {0,2}, đội B là {1,3};
 * side đội A là {0,1}, đội B là {2,3}. Party đầu nhận đội A, party thứ hai nhận
 * đội B, người chơi đơn lấp các ghế còn lại theo FIFO.
 * @param[in] queue Hàng chờ cần xem (không bị sửa).
 * @param[in] mode Chế độ cần ghép.
 * @param[out] outSeats Thứ tự ghế của trận, chỉ hợp lệ khi trả true.
 * @param[out] outIndexes Index các mục đã chọn, tăng dần.
 * @param[out] outEntryCount Số index đã ghi vào outIndexes.
 * @return true khi tìm đủ bốn người, false nếu không.
 */
bool queuePick(const Queue *queue, Mode mode, UserId outSeats[QUEUE_MATCH_SEATS],
               size_t outIndexes[QUEUE_MATCH_SEATS], size_t *outEntryCount);

/**
 * @brief Bỏ các mục mà queuePick trả về sau khi trận của chúng đã mở.
 * @param[in,out] queue Hàng chờ do vòng poll sở hữu.
 * @param[in] mode Chế độ mà các index thuộc về.
 * @param[in] indexes Index mục từ queuePick; index ngoài phạm vi được bỏ qua.
 * @param[in] count Số index.
 */
void queueTake(Queue *queue, Mode mode, const size_t indexes[], size_t count);

/**
 * @brief Xử lý QUEUE_JOIN {mode} và QUEUE_LEAVE {} cho tài khoản của phiên.
 *
 * QUEUE_JOIN xếp hàng cả party nếu người gọi có party (chế độ phải trùng chế độ
 * của party), trả OK {queue: payload QUEUE_STATE}, push QUEUE_STATE cho hàng chờ
 * của chế độ đó và thử mở trận ranked ngay.
 * @param[in,out] app Trạng thái server do vòng poll sở hữu.
 * @param[in] conn Kết nối đã gửi request.
 * @param[in] envelope Request; chỉ mượn trong lúc gọi.
 * @return OK khi đã trả lời (lỗi được gửi dạng ERROR), BAD_REQUEST khi đối số NULL.
 */
Status queueHandle(App *app, ConnId conn, const Envelope *envelope);

/**
 * @brief Bỏ mục của user sau khi rớt mạng hoặc PARTY_LEAVE và báo cho mọi người liên quan.
 * @param[in,out] app Trạng thái server do vòng poll sở hữu.
 * @param[in] user Tài khoản vừa rời; user không ở hàng chờ được bỏ qua.
 */
void queueUserGone(App *app, UserId user);

/**
 * @brief Gửi lại QUEUE_STATE của user sau khi LOGIN hoặc RESUME gắn socket mới.
 * @param[in,out] app Trạng thái server do vòng poll sở hữu.
 * @param[in] user Tài khoản vừa gắn vào kết nối; user không ở hàng chờ không nhận gì.
 */
void queueSyncUser(App *app, UserId user);

/**
 * @brief Loại các mục có user đã offline và thử lại các chế độ mở trận thất bại.
 * @param[in,out] app Trạng thái server do vòng poll sở hữu.
 * @param[in] nowMs Thời gian monotonic từ appNowMs.
 */
void queueTick(App *app, uint64_t nowMs);

#endif
