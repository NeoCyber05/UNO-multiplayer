#ifndef UNO_GAME_H
#define UNO_GAME_H

#include "shared/contracts.h"

enum { CARD_COUNT = 112, SEAT_COUNT = 4, DRAFT_OFFER_COUNT = SEAT_COUNT };
typedef enum { NO_COLOR, RED, YELLOW, GREEN, BLUE, WILD_COLOR } Color;
typedef enum {
    ZERO,
    ONE,
    TWO,
    THREE,
    FOUR,
    FIVE,
    SIX,
    SEVEN,
    EIGHT,
    NINE,
    SKIP,
    REVERSE,
    DRAW_TWO,
    DISCARD_ALL,
    WILD,
    WILD_FOUR
} Value;
typedef struct {
    char id[5];
    Color color;
    Value value;
} Card;
typedef struct {
    int count;
    Card cards[CARD_COUNT];
} Pile;
typedef enum { PLAY, DRAW, PASS, SAY_UNO, DRAFT_PICK, LEAVE, CHAT } IntentKind;
typedef struct {
    IntentKind kind;
    MatchId matchId; /* Bộ điều phối dùng mã này để xác định trận nhận lệnh. */
    uint32_t turnNo, draftTurn;
    char cardId[5];
    Color color;
    int index, chatId;
} Intent;
typedef enum { PLAYING, FINISHED, DRAFTING } Phase;
typedef struct {
    bool occupied;
    Card card;
} Offer;
typedef struct {
    uint32_t turn;
    Offer offers[DRAFT_OFFER_COUNT];
} DraftState;
typedef struct {
    Mode mode;
    Phase phase;
    PlayerSpec players[SEAT_COUNT];
    Pile deck, discard, hands[SEAT_COUNT];
    DraftState draft;
    Color currentColor;
    Seat turn;
    uint32_t turnNo;
    int direction, winnerSeat, winnerTeam, team[SEAT_COUNT];
    char drawnCardId[5]; /* Nếu có, đây là lá duy nhất được đánh sau khi rút. */
    bool unoDeclared[SEAT_COUNT];
    PlayerStats stats[SEAT_COUNT];
    uint64_t rng, revision, eventSeq;
} Game;

/**
 * @brief Khởi tạo và chia bài theo seed để có thể tái lập ván đấu.
 * @param[out] game Trạng thái được khởi tạo sau khi kiểm tra người chơi.
 * @param[in] mode Chế độ quyết định cách chia đội và giai đoạn đầu trận.
 * @param[in] players Thông tin người chơi theo ghế, được sao chép vào ván đấu.
 * @param[in] seed Giá trị khởi tạo bộ xáo bài, có thể bằng 0.
 * @return OK khi thành công, BAD_REQUEST nếu đầu vào không hợp lệ.
 */
Status gameInit(Game *game, Mode mode, const PlayerSpec players[SEAT_COUNT], uint64_t seed);

/**
 * @brief Kiểm tra và áp dụng một nước đi lên bản sao trước khi bộ điều phối lưu.
 * @param[in,out] game Trạng thái dự kiến để bộ điều phối lưu và công bố.
 * @param[in] actor Ghế thực hiện nước đi, có thể do người chơi hoặc bot điều khiển.
 * @param[in] intent Nước đi và số lượt đánh hoặc lượt chọn bài tương ứng.
 * @param[out] out Nhận các sự kiện phát sinh khi áp dụng nước đi.
 * @return OK khi thành công hoặc mã nêu lý do từ chối nước đi.
 */
Status gameApply(Game *game, Seat actor, const Intent *intent, EventBatch *out);

/**
 * @brief Chuyển lượt khi hết giờ và rút một lá nếu ghế hiện tại chưa rút.
 * @param[in,out] game Trạng thái dự kiến trong giai đoạn đánh bài.
 * @param[in] expectedTurn Số lượt đã ghi nhận để tránh hết giờ nhầm lượt.
 * @param[out] out Nhận sự kiện hết giờ và các sự kiện phát sinh.
 * @return OK, STALE_TURN nếu lượt đã đổi, BAD_REQUEST nếu thiếu con trỏ,
 *         hoặc INVALID_ACTION nếu ván đang ở giai đoạn khác.
 */
Status gameTimeout(Game *game, uint32_t expectedTurn, EventBatch *out);

/**
 * @brief Khi hết giờ, tự chọn lá chưa được lấy có chỉ số nhỏ nhất.
 * @param[in,out] game Trạng thái dự kiến trong giai đoạn chọn bài.
 * @param[in] expectedPick Số lượt chọn đã ghi nhận để tránh chọn nhầm lượt.
 * @param[out] out Nhận các sự kiện do lượt chọn tự động tạo ra.
 * @return OK, STALE_TURN nếu lượt chọn đã đổi, BAD_REQUEST nếu thiếu con trỏ,
 *         hoặc INVALID_ACTION nếu sai giai đoạn hay hết lá để chọn.
 */
Status gameDraftTimeout(Game *game, uint32_t expectedPick, EventBatch *out);

/**
 * @brief Kiểm tra luật đánh một lá bài trong lượt hiện tại.
 * @param[in] game Trạng thái ván hiện tại.
 * @param[in] actor Ghế muốn đánh lá bài.
 * @param[in] card Lá bài lấy từ tay của ghế đó. Hàm không kiểm tra quyền sở hữu lá.
 * @return true nếu đúng lượt, lá đánh được theo luật và thỏa giới hạn sau khi rút.
 */
bool gamePlayable(const Game *game, Seat actor, const Card *card);

/**
 * @brief Kiểm tra quyền hô UNO trước một nước đi khiến tay bài còn đúng một lá.
 * @param[in] game Trạng thái ván hiện tại.
 * @param[in] seat Ghế muốn hô UNO.
 * @return true nếu ghế chưa hô UNO và có nước đi hợp lệ như trên trong lượt này.
 */
bool gameCanSayUno(const Game *game, Seat seat);

/**
 * @brief Sinh giá trị giả ngẫu nhiên có thể tái lập bằng SplitMix64.
 * @param[in,out] state Trạng thái được cập nhật, chấp nhận cả giá trị ban đầu bằng 0.
 * @return Giá trị 64 bit tiếp theo trong chuỗi.
 */
uint64_t gameRandom(uint64_t *state);

#endif
