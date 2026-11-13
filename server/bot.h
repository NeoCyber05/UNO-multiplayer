#ifndef UNO_BOT_H
#define UNO_BOT_H

#include "view.h"

Status bot_choose(const GameView *view, BotLevel level, uint64_t *rng_state, Intent *out);

#endif
