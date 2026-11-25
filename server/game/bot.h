#ifndef UNO_BOT_H
#define UNO_BOT_H

#include "server/game/view.h"

/**
 * @brief Choose an action using only the bot seat's view.
 * @param[in] view Seat-specific snapshot with its legal actions.
 * @param[in] level Difficulty used to score options.
 * @param[in,out] randomState State used for random choices and tie-breaking.
 * @param[out] out Receives the selected action on success.
 * @return OK on success or a status describing why no action was selected.
 */
Status botChoose(const GameView *view, BotLevel level, uint64_t *randomState, Intent *out);

#endif
