#ifndef UNO_STORE_H
#define UNO_STORE_H

#include "shared/contracts.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

typedef struct {
    char email[255];
    char hash[128];
    char name[65];
    char avatar[32];
} AccountCreate;

typedef struct {
    UserId id;
    char email[255];
    char hash[128];
    char name[65];
    char avatar[32];
    int elo;
} Account;

Status store_open(Store **out, const char *path);
void store_close(Store *s);
Status store_match_start(Store *s, const MatchId *match_id, const PlayerSpec players[4], Mode mode, bool ranked);
Status store_events(Store *s, const MatchId *match_id, const EventBatch *batch);
Status store_finalize(Store *s, const MatchResult *result, const ProfileDelta deltas[4]);
Status store_export_match(Store *s, const MatchId *match_id, const char *path);

Status store_account_create(Store *s, const AccountCreate *c, UserId *out_id);
Status store_account_get(Store *s, const char *email, Account *out);
Status store_account_get_by_id(Store *s, UserId id, Account *out);

sqlite3 *store_get_db(Store *s);

#endif
