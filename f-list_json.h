#ifndef F_LIST_JSON_H
#define	F_LIST_JSON_H

#include "f-list.h"

typedef void            (*FListWebCallback)       (FListWebRequestData*, gpointer data, JsonObject *, const gchar *error);

FListWebRequestData* flist_web_request(const gchar*, GHashTable*, gboolean post, FListWebCallback, gpointer data);
void flist_web_request_cancel(FListWebRequestData*);

GHashTable *flist_web_request_args(FListAccount*);

void flist_web_requests_init();

#endif	/* F_LIST_JSON_H */
