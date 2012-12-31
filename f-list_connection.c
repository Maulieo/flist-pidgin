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
#include "f-list_connection.h"

/* disconnect after 90 seconds without a ping response */
#define FLIST_TIMEOUT 90
/* how often we request a new ticket for the API */
#define FLIST_TICKET_TIMER_TIMEOUT 600

GHashTable *ticket_table;
const gchar *flist_get_ticket(FListAccount *fla) {
    return g_hash_table_lookup(ticket_table, fla->username);
}

static gboolean flist_disconnect_cb(gpointer user_data) {
    PurpleConnection *pc = user_data;
    FListAccount *fla = pc->proto_data;

    fla->ping_timeout_handle = 0;

    purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Connection timed out.");

    return FALSE;
}

void flist_receive_ping(PurpleConnection *pc) {
    FListAccount *fla = pc->proto_data;

    if(fla->ping_timeout_handle) {
        purple_timeout_remove(fla->ping_timeout_handle);
    }
    fla->ping_timeout_handle = purple_timeout_add_seconds(FLIST_TIMEOUT, flist_disconnect_cb, pc);
}

void flist_request(PurpleConnection *pc, const gchar* type, JsonObject *object) {
    FListAccount *fla = pc->proto_data;
    gsize json_len;
    gchar *json_text = NULL;
    gsize sent;
    GString *to_write_str = g_string_new(NULL);
    gchar *to_write;
    gsize to_write_len;
    
    g_string_append_c(to_write_str, '\x00');
    g_string_append(to_write_str, type);
    
    if(object) {
        JsonNode *root = json_node_new(JSON_NODE_OBJECT);
        JsonGenerator *gen = json_generator_new();
        json_node_set_object(root, object);
        json_generator_set_root(gen, root);
        json_text = json_generator_to_data(gen, &json_len);
        g_string_append(to_write_str, " ");
        g_string_append(to_write_str, json_text);
        g_free(json_text);
        g_object_unref(gen);
        json_node_free(root);
    }
    
    g_string_append_c(to_write_str, '\xFF');
    
    to_write_len = to_write_str->len;
    to_write = g_string_free(to_write_str, FALSE);
    // TODO: check the return value of write()
    sent = write(fla->fd, to_write, to_write_len);
    g_free(to_write);
}

static gboolean flist_recv(PurpleConnection *pc, gint source, PurpleInputCondition cond) {
    FListAccount *fla = pc->proto_data;
    gchar buf[4096];
    gssize len;
        
    len = recv(fla->fd, buf, sizeof(buf) - 1, 0);
    if(len <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return FALSE; //try again later
        //TODO: better error reporting
        purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The connection has failed.");
        return FALSE;
    }
    buf[len] = '\0';
    fla->rx_buf = g_realloc(fla->rx_buf, fla->rx_len + len + 1);
    memcpy(fla->rx_buf + fla->rx_len, buf, len + 1);
    fla->rx_len += len;
    return TRUE;
}

static gboolean flist_handle_input(PurpleConnection *pc) {
    FListAccount *fla = pc->proto_data;
    gchar *start, *end; gchar *rx_buf_end;
    JsonParser *parser = NULL;
    JsonNode *root = NULL;
    JsonObject *object = NULL;
    GError *err = NULL;
    gboolean ret = FALSE;
    gchar *code;

    g_return_val_if_fail(fla, FALSE);

    if(fla->rx_len == 0) return FALSE; //nothing to read here!
    
    if(fla->rx_buf[0] != '\x00') {
        purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Invalid WebSocket data (not a WebSocket frame).");
    }
    rx_buf_end = fla->rx_buf + fla->rx_len;
    start = fla->rx_buf + 1;
    end = fla->rx_buf;
    while(end < rx_buf_end && *end != '\xff') end++;
    if(end == rx_buf_end) return FALSE; //we don't have a full packet yet
    code = g_strndup(start, 3);
    start += 3;
    if(start < end && strcmp(code, "WSH")) {
        start++;
        parser = json_parser_new();
        json_parser_load_from_data(parser, start, (gsize) (end - start), &err);
        
        if(fla->debug_mode) {
            gchar *full_packet = g_strndup(start, (gsize) (end - start));
            purple_debug_info(FLIST_DEBUG, "JSON Received: %s\n", full_packet);
            g_free(full_packet);
        }
        
        if(err) { /* not valid json */
            purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Invalid WebSocket data (expecting JSON).");
            g_error_free(err);
            goto cleanup;
        }
        root = json_parser_get_root(parser);
        if(json_node_get_node_type(root) != JSON_NODE_OBJECT) {
            purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Invalid WebSocket data (JSON not an object).");
            goto cleanup;
        }
        object = json_node_get_object(root);
    }
    
    ret = TRUE;
    purple_debug_info("flist", "Received Packet. Code: %s\n", code);
    flist_callback(pc, code, object);
    
    cleanup:
    
    end++;
    fla->rx_len = (gsize) (fla->rx_buf + fla->rx_len - end);
    memmove(fla->rx_buf, end, fla->rx_len + 1);
    
    g_free(code);
    if(parser) g_object_unref(parser);
    
    return ret;
}

static gboolean flist_handle_handshake(PurpleConnection *pc) {
    FListAccount *fla = pc->proto_data;
    gchar *last = fla->rx_buf;
    gchar *read = strstr(last, "\r\n");
    
    while(read != NULL && read > last) {
        last = read + 2;
        read = strstr(last, "\r\n");
    }
    
    if(read == NULL) return FALSE;

    read += 2; //last line
    read += 16; //useless token
    if(read >= fla->rx_buf + fla->rx_len) { //make sure we didn't overflow
        fla->rx_len -= (gsize) (read - fla->rx_buf);
        memmove(fla->rx_buf, read, fla->rx_len + 1);
        flist_IDN(pc);
        fla->connection_status = FLIST_IDENTIFY;
        return TRUE;
    } else return FALSE;
}

void flist_process(gpointer data, gint source, PurpleInputCondition cond) {
    PurpleConnection *pc = data;
    FListAccount *fla = pc->proto_data;
    
    if(!flist_recv(pc, source, cond)) return;
    if(fla->connection_status == FLIST_HANDSHAKE && !flist_handle_handshake(pc)) return;
    while(flist_handle_input(pc));
}

void flist_IDN(PurpleConnection *pc) {
    FListAccount *fla = pc->proto_data;
    JsonObject *object;
    const gchar *ticket = flist_get_ticket(fla);
    
    object = json_object_new();
    if(ticket) {
        json_object_set_string_member(object, "method", "ticket");
        json_object_set_string_member(object, "ticket", ticket);
        json_object_set_string_member(object, "account", fla->username);
        json_object_set_string_member(object, "cname", FLIST_CLIENT_NAME);
        json_object_set_string_member(object, "cversion", FLIST_PLUGIN_VERSION);
    }
    json_object_set_string_member(object, "character", fla->character);
    flist_request(pc, "IDN", object);
    json_object_unref(object);
}

void flist_connected(gpointer user_data, int fd, const gchar *err) {
    FListAccount *fla = user_data;
    
    if(err) {
        purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, err);
        fla->connection_status = FLIST_OFFLINE;
        return;
    }

    fla->fd = fd;

    fla->input_handle = purple_input_add(fla->fd, PURPLE_INPUT_READ, flist_process, fla->pc);
    fla->ping_timeout_handle = purple_timeout_add_seconds(FLIST_TIMEOUT, flist_disconnect_cb, fla->pc);
    if(fla->use_websocket_handshake) {
        GString *headers_str = g_string_new(NULL);
        gchar *headers;
        int len;
        //TODO: insert proper randomness here!
        g_string_append(headers_str, "GET / HTTP/1.1\r\n");
        g_string_append(headers_str, "Upgrade: WebSocket\r\n");
        g_string_append(headers_str, "Connection: Upgrade\r\n");
        g_string_append_printf(headers_str, "Host: %s:%d\r\n", fla->server_address, fla->server_port);
        g_string_append(headers_str, "Origin: http://www.f-list.net\r\n");
        g_string_append(headers_str, "Cookie: \r\n");
        g_string_append(headers_str, "Sec-WebSocket-Key1: ?1:70X 1q057L74,6>\\\r\n");
        g_string_append(headers_str, "Sec-WebSocket-Key2: 3qJ1  16=8v97(98:8Mah\r\n");
        g_string_append(headers_str, "\r\n");
        g_string_append(headers_str, "d.;~w.A."); //TODO: throw in randomness!
        headers = g_string_free(headers_str, FALSE);

        len = write(fla->fd, headers, strlen(headers)); //TODO: check return value
        fla->connection_status = FLIST_HANDSHAKE;
        g_free(headers);
    } else {
        flist_request(fla->pc, "WSH", NULL);
        fla->connection_status = FLIST_IDENTIFY;
    }

}

static void flist_receive_ticket(FListWebRequestData *req_data, gpointer data, JsonObject *root, const gchar *error) {
    FListAccount *fla = data;
    const gchar *ticket;
    gboolean first = fla->connection_status == FLIST_OFFLINE;
    
    fla->ticket_request = NULL;
    flist_ticket_timer(fla, FLIST_TICKET_TIMER_TIMEOUT);
    
    if(error) {
        if(first) purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error);
        return;
    }
    
    error = json_object_get_string_member(root, "error");
    if(error && strlen(error)) {
        if(first) purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error);
        return;
    }
    
    ticket = json_object_get_string_member(root, "ticket");
    if(!ticket) {
        if(first) purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "No ticket returned.");
        return;
    }
    
    g_hash_table_insert(ticket_table, g_strdup(fla->username), g_strdup(ticket));
    purple_debug_info("flist", "Login Ticket: %s\n", ticket);
    
    if(first) {
        if(!purple_proxy_connect(fla->pc, fla->pa, fla->server_address, fla->server_port, flist_connected, fla)) {
            purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, _("Unable to open a connection."));
            return;
        }
        fla->connection_status = FLIST_CONNECT;
    }
}

static gboolean flist_ticket_timer_cb(gpointer data) {
    FListAccount *fla = data;
    const gchar *url_pattern = "http://www.f-list.net/json/getApiTicket.php";
    GHashTable *args = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_insert(args, "account", g_strdup(fla->username));
    g_hash_table_insert(args, "password", g_strdup(fla->password));
    g_hash_table_insert(args, "secure", g_strdup("no"));
    
    fla->ticket_request = flist_web_request(url_pattern, args, TRUE, flist_receive_ticket, fla); 
    fla->ticket_timer = 0;
    
    g_hash_table_destroy(args);
    
    return FALSE;
}

void flist_ticket_timer(FListAccount *fla, guint timeout) {
    if(fla->ticket_timer) {
        purple_timeout_remove(fla->ticket_timer);
    }
    fla->ticket_timer = purple_timeout_add_seconds(timeout, (GSourceFunc) flist_ticket_timer_cb, fla);
}

void flist_ticket_init() {
    ticket_table = g_hash_table_new_full((GHashFunc) flist_str_hash, (GEqualFunc) flist_str_equal, g_free, g_free);
}
