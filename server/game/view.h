#ifndef UNO_VIEW_H
#define UNO_VIEW_H

#include "server/game/game.h"
#include <json-c/json.h>

typedef struct {
    PlayerSpec player;
    int team, cardCount;
    bool connected, handVisible;
    Pile hand;
} PlayerView;

typedef struct {
    Seat viewerSeat, turn;
    Mode mode;
    Phase phase;
    int direction, drawCount, winnerSeat, winnerTeam;
    uint32_t turnNo;
    uint64_t revision;
    Color currentColor;
    Card topCard;
    DraftState draft;
    PlayerView players[SEAT_COUNT];
    bool legal[CARD_COUNT]; /* Dùng cùng chỉ số với các lá trong tay người xem. */
    bool canDraw, canPass, canSayUno;
    /* Bộ điều phối bổ sung mã trận, mốc thời gian và trạng thái kết nối. */
    MatchId matchId;
    uint64_t deadline, serverNowMs, durationMs;
} GameView;

/**
 * @brief Tạo bản trạng thái chỉ lộ tay bài của người xem và đồng đội.
 * @param[in] game Trạng thái hiện tại của ván đấu.
 * @param[in] viewer Ghế nhận bản trạng thái.
 * @param[out] view Nhận dữ liệu ván. Bộ điều phối bổ sung thời gian và kết nối.
 * @return OK khi thành công, BAD_REQUEST nếu đối số không hợp lệ.
 */
Status viewBuild(const Game *game, Seat viewer, GameView *view);

/**
 * @brief Mã hóa bản trạng thái riêng của một ghế thành payload GAME_STATE.
 * @param[in] view Bản trạng thái đã lọc theo ghế nhận.
 * @param[out] out Nhận tham chiếu JSON mà bên gọi phải giải phóng nếu thành công.
 * @return OK khi thành công, BAD_REQUEST nếu đối số sai, BUSY nếu không tạo được JSON.
 */
Status viewJson(const GameView *view, json_object **out);

#endif
