/*
 * F-List Pidgin - a libpurple protocol plugin for F-Chat
 *
 * Copyright 2011 F-List Pidgin developers.
 *
 * This file is part of F-List Pidgin.
 *
 * F-List Pidgin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * F-List Pidgin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with F-List Pidgin.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "f-list_http.h"

//TODO: you're supposed to change spaces to "+" values??
static void g_string_append_cgi(GString *str, GHashTable *table) {
    GHashTableIter iter;
    gpointer key, value;
    gboolean first = TRUE;
    g_hash_table_iter_init(&iter, table);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        purple_debug_info("flist", "cgi writing key, value: %s, %s\n", (gchar *)key, (gchar *)value);
        if(!first) g_string_append(str, "&");
        g_string_append_printf(str, "%s", purple_url_encode(key));
        g_string_append(str, "=");
        g_string_append_printf(str, "%s", purple_url_encode(value));
        first = FALSE;
    }
}

static void g_string_append_cookies(GString *str, GHashTable *table) {
    GHashTableIter iter;
    gpointer key, value;
    gboolean first = TRUE;
    g_hash_table_iter_init(&iter, table);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        if(!first) g_string_append(str, " ");
        g_string_append_printf(str, "%s", purple_url_encode(key));
        g_string_append(str, "=");
        g_string_append_printf(str, "%s;", purple_url_encode(value));
        first = FALSE;
    }
}

//mostly shamelessly stolen from pidgin's "util.c"
gchar *http_request(const gchar *url, gboolean http11, gboolean post, const gchar *user_agent, GHashTable *req_table, GHashTable *cookie_table) {
    GString *request_str = g_string_new(NULL);
    gchar *address = NULL, *page = NULL, *user = NULL, *password = NULL;
    int port;
    
    purple_url_parse(url, &address, &port, &page, &user, &password);
    
    g_string_append_printf(request_str, "%s /%s%s", (post ? "POST" : "GET"), page, (!post && req_table ? "?" : ""));
    if(req_table && !post) g_string_append_cgi(request_str, req_table);
    g_string_append_printf(request_str, " HTTP/%s\r\n", (http11 ? "1.1" : "1.0"));
    g_string_append_printf(request_str, "Connection: close\r\n");
    if(user_agent) g_string_append_printf(request_str, "User-Agent: %s\r\n", user_agent);
    g_string_append_printf(request_str, "Accept: */*\r\n");
    g_string_append_printf(request_str, "Host: %s\r\n", address);
    
    if(cookie_table) {
        g_string_append(request_str, "Cookie: ");
        g_string_append_cookies(request_str, cookie_table);
        g_string_append(request_str, "\r\n");
    }
    
    if(post) {
        GString *post_str = g_string_new(NULL);
        gchar *post = NULL;
        
        if(req_table) g_string_append_cgi(post_str, req_table);
        
        post = g_string_free(post_str, FALSE);
        
        purple_debug_info("flist", "posting (len: %d): %s\n", strlen(post), post);

        g_string_append(request_str, "Content-Type: application/x-www-form-urlencoded\r\n");
        g_string_append_printf(request_str, "Content-Length: %d\r\n", strlen(post));
        g_string_append(request_str, "\r\n");
        
        g_string_append(request_str, post);
        
        g_free(post);
    } else {
        g_string_append(request_str, "\r\n");
    }
    
    if(address) g_free(address);
    if(page) g_free(page);
    if(user) g_free(user);
    if(password) g_free(password);
    
    return g_string_free(request_str, FALSE);
}

PurpleUtilFetchUrlData *flist_login_fls_request(PurpleConnection *pc, const gchar *user_agent, 
        const gchar *username, const gchar *password, PurpleUtilFetchUrlCallback callback) {
    gchar *url = "http://www.f-list.net/action/script_login.php";
    PurpleUtilFetchUrlData *ret;
    GHashTable *post = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
    gchar *request;
    
    g_hash_table_insert(post, (gpointer) "username", (gpointer) username);
    g_hash_table_insert(post, (gpointer) "password", (gpointer) password);
    request = http_request(url, FALSE, TRUE, user_agent, post, NULL);
    ret = purple_util_fetch_url_request(url, FALSE, user_agent, FALSE, request, TRUE, callback, pc);
    
    g_free(request);
    g_hash_table_destroy(post);
    
    return ret;
}

PurpleUtilFetchUrlData *flist_login_ticket_request(PurpleConnection *pc, const gchar *user_agent, const gchar *username, const gchar *password, PurpleUtilFetchUrlCallback callback) {
    const gchar *url_pattern = "http://www.f-list.net/json/getApiTicket.php";
    PurpleUtilFetchUrlData *ret;
    GString *url_str = g_string_new(NULL);
    gchar *url;
    
    g_string_append(url_str, url_pattern);
    g_string_append_printf(url_str, "?account=%s", purple_url_encode(username));
    g_string_append_printf(url_str, "&password=%s", purple_url_encode(password));
    g_string_append_printf(url_str, "&secure=%s", "no");
    url = g_string_free(url_str, FALSE);
    ret = purple_util_fetch_url_request(url, FALSE, user_agent, TRUE, NULL, FALSE, callback, pc);
    g_free(url);
    return ret;
}

gchar *flist_parse_FLS_cookie(const gchar *data) {
    //Set-Cookie: FLS=6da62d9f586c83dd6f946f1213abf486b3887d988847c4255c8f0502d9824df2; expires=Mon, 14-Mar-2011 13:28:07 GMT; path=/
    GError *error = NULL;
    GRegex *regex = g_regex_new("^Set-Cookie:\\s*FLS=([^;]*)(?:;|$).*", G_REGEX_CASELESS | G_REGEX_MULTILINE | G_REGEX_NEWLINE_CRLF, G_REGEX_MATCH_NEWLINE_CRLF, &error);
    GMatchInfo *match = NULL;
    gchar *ret = NULL;
    
    g_regex_match(regex, data, G_REGEX_MATCH_NEWLINE_CRLF, &match);
    if(g_match_info_matches(match)) {
        ret = g_match_info_fetch (match, 1);
    }
    
    g_match_info_free(match);
    g_regex_unref(regex);
    if(error) g_error_free(error);
    
    return ret;
}


