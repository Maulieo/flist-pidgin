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

void flist_send_hybi(FListAccount *fla, guint8 opcode, gchar *content, gsize len) {
    guint8 *frame, *ptr;
    gsize frame_len;
    guint8 mask[4];
    int offset;
    gsize sent;
    
    frame = g_malloc(10 + len);
    ptr = frame;
    *ptr = (opcode << 4) | 0x01; ptr++;
    
    /* The length is sent as a big endian integer. */
    if(len < 126) {
        *ptr = (guint8) ((len & 0x7F) | 0x80); ptr++;
    } else if(len >= 126 && len < 65536) {
        *ptr = 0xFE; ptr++;
        *ptr = (guint8) ((len >>  8) & 0xFF); ptr++;
        *ptr = (guint8) ((len >>  0) & 0xFF); ptr++;
    } else if(len >= 65536) {
        *ptr = 0xFF; ptr++;
        *ptr = (guint8) ((len >>  56) & 0xFF); ptr++;
        *ptr = (guint8) ((len >>  48) & 0xFF); ptr++;
        *ptr = (guint8) ((len >>  40) & 0xFF); ptr++;
        *ptr = (guint8) ((len >>  32) & 0xFF); ptr++;
        *ptr = (guint8) ((len >>  24) & 0xFF); ptr++;
        *ptr = (guint8) ((len >>  16) & 0xFF); ptr++;
        *ptr = (guint8) ((len >>   8) & 0xFF); ptr++;
        *ptr = (guint8) ((len >>   0) & 0xFF); ptr++;
    }
    
    mask[0] = (guint8) (g_random_int() & 0xFF);
    mask[1] = (guint8) (g_random_int() & 0xFF);
    mask[2] = (guint8) (g_random_int() & 0xFF);
    mask[3] = (guint8) (g_random_int() & 0xFF);
    memcpy(ptr, mask, 4); ptr += 4;
    memcpy(ptr, content, len);
    
    for(offset = 0; offset < len; offset++) { /* Apply the mask. */
        ptr[offset] = ptr[offset] ^ mask[offset % 4];
    }
    
    frame_len = (gsize) (ptr + len - frame);
    // TODO: check the return value of write()
    sent = write(fla->fd, frame, frame_len);
    g_free(frame);
}

void flist_request(PurpleConnection *pc, const gchar* type, JsonObject *object) {
    FListAccount *fla = pc->proto_data;
    gsize json_len;
    gchar *json_text = NULL;
    GString *to_write_str = g_string_new(NULL);
    gchar *to_write;
    gsize to_write_len;
    
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
        
    to_write_len = to_write_str->len;
    to_write = g_string_free(to_write_str, FALSE);
    flist_send_hybi(fla, WS_OPCODE_TYPE_TEXT, to_write, to_write_len);
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

static gchar *flist_process_hybi(FListAccount *fla, gboolean is_final, guint8 opcode, gchar *payload, gsize payload_len) {
    gchar *tmp;
    switch(opcode) { /* Check for all possible error conditions. */
    case WS_OPCODE_TYPE_BINARY:
    case WS_OPCODE_TYPE_TEXT:
        if(fla->frame_buffer) {
            purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server sent a WebSocket data frame, but we are expecting a continuation frame.");
            return NULL;
        }
        if(!is_final) { /* We only have part of a frame. Save it instead of returning it. */
            fla->frame_buffer = g_string_new_len(payload, payload_len);
            return NULL;
        }
        return g_strndup(payload, payload_len);
    case WS_OPCODE_TYPE_CONTINUATION:
        if(!fla->frame_buffer) {
            purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server unexpectedly sent a WebSocket continuation frame.");
            return NULL;
        }
        g_string_append_len(fla->frame_buffer, payload, payload_len);
        if(!is_final) return NULL;
        tmp = g_string_free(fla->frame_buffer, FALSE);
        fla->frame_buffer = NULL;
        return tmp;
    case WS_OPCODE_TYPE_PING: //We have to respond with a 'pong' frame.
        if(!is_final) {
            purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server sent a fragmented WebSocket ping frame.");
            return NULL;
        }
        flist_send_hybi(fla, WS_OPCODE_TYPE_PONG, payload, payload_len);
        return NULL;
    case WS_OPCODE_TYPE_PONG: //Silently discard.
        if(!is_final) {
            purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server sent a fragmented WebSocket pong frame.");
            return NULL;
        }
        return NULL;
    case WS_OPCODE_TYPE_CLOSE:
        purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server has terminated the connection with a WebSocket close frame.");
        return NULL;
    default:
        purple_connection_error_reason(fla->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server sent a WebSocket frame with an unknown opcode.");
        return NULL;
    }
}

/* This function returns the payload of the next incoming data frame, or NULL if none is available. */
/* It processes, handles, and silently discards control frames. */
static gchar *flist_handle_hybi(FListAccount *fla) {
    gchar *cur, *rx_end;
    guint8 opcode;
    gboolean is_final, is_masked;
    guint8 raw_len;
    guint8 mask[4];
    gchar *payload;
    gsize payload_len;
    int offset;
    gchar *full_payload = NULL;
    
    while(!full_payload) { //TODO: When a connection error happens, this loop should stop.
        rx_end = fla->rx_buf + fla->rx_len;
        
        cur = fla->rx_buf;
        if(cur + 2 > rx_end) return NULL;

        is_final = (*cur & 0x80) != 0;
        opcode = *cur & 0x0F;
        purple_debug_info(FLIST_DEBUG, "opcode: %d\n", opcode);
        cur += 1;
        is_masked = (*cur & 0x80) != 0;
        raw_len = *cur & 0x7F;
        cur += 1;
        
        switch(raw_len) { /* TODO: These conversations should work, but we should not be casting pointers. */
        case 0x7E: /* The actual length is the next two bytes in big endian format. */
            if(cur + 2 > rx_end) return NULL;
            payload_len = GUINT16_FROM_BE(*((guint16*) cur));
            cur += 2;
            break;
        case 0x7F: /* The actual length is the next eight bytes in big endian format. */
            if(cur + 8 > rx_end) return NULL;
            payload_len = GUINT64_FROM_BE(*((guint64*) cur));
            cur += 8;
            break;
        default: /* The actual length is the raw length. */
            payload_len = raw_len;
        }
        
        if(is_masked) {
            if(cur + 4 > rx_end) return NULL;
            mask[0] = cur[0];
            mask[1] = cur[1];
            mask[2] = cur[2];
            mask[3] = cur[3];
            cur += 4;
        }
        
        if(cur + payload_len > rx_end) return NULL;
        payload = g_malloc(payload_len);
        memcpy(payload, cur, payload_len);
        cur += payload_len;
        
        if(is_masked) { /* Unmask the data. */
            for(offset = 0; offset < payload_len; offset++) {
                payload[offset] = payload[offset] ^ mask[offset % 4];
            }
        }
        
        /* We've read one new frame. We clean up the buffer here. */
        fla->rx_len = (gsize) (fla->rx_buf + fla->rx_len - cur);
        memmove(fla->rx_buf, cur, fla->rx_len + 1);
        
        full_payload = flist_process_hybi(fla, is_final, opcode, payload, payload_len);
        
        g_free(payload);
    }

    return full_payload;
}

static gboolean flist_handle_input(PurpleConnection *pc) {
    FListAccount *fla = pc->proto_data;
    JsonParser *parser = NULL;
    JsonNode *root = NULL;
    JsonObject *object = NULL;
    GError *err = NULL;
    gchar *message = NULL, *code = NULL, *json = NULL;
    gsize len;
    gboolean success = FALSE;

    g_return_val_if_fail(fla, FALSE);
    
    message = flist_handle_hybi(fla);
    if(!message) return FALSE;
    
    len = strlen(message);
    if(len < 3) {
        purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server sent an invalid packet.");
        goto cleanup;
    }
    
    code = g_strndup(message, 3);
    purple_debug_info(FLIST_DEBUG, "The server sent a packet with code: %s\n", code);
    
    if(len > 3) {
        json = g_strdup(message + 4);
        purple_debug_info(FLIST_DEBUG, "Raw JSON text: %s\n", json);

        parser = json_parser_new();
        json_parser_load_from_data(parser, json, -1, &err);
        
        if(err) { /* not valid json */
            purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server sent non-JSON data.");
            g_error_free(err);
            goto cleanup;
        }
        root = json_parser_get_root(parser);
        if(json_node_get_node_type(root) != JSON_NODE_OBJECT) {
            purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "The server sent JSON data that is not an object.");
            goto cleanup;
        }
        object = json_node_get_object(root);
    } else {
        purple_debug_info(FLIST_DEBUG, "(No JSON data was included.)");
    }
    
    success = TRUE;
    flist_callback(pc, code, object);
    
    cleanup:
    g_free(message);
    if(code) g_free(code);
    if(json) g_free(json);
    if(parser) g_object_unref(parser);
    
    return success;
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
    fla->rx_len -= (gsize) (read - fla->rx_buf);
    memmove(fla->rx_buf, read, fla->rx_len + 1);
    
    purple_debug_info(FLIST_DEBUG, "Identifying...");
    flist_IDN(pc);
    fla->connection_status = FLIST_IDENTIFY;
    return TRUE;
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

    GString *headers_str = g_string_new(NULL);
    gchar *headers;
    int len;
    //TODO: insert proper randomness here!
    g_string_append(headers_str, "GET / HTTP/1.1\r\n");
    g_string_append(headers_str, "Upgrade: WebSocket\r\n");
    g_string_append(headers_str, "Connection: Upgrade\r\n");
    g_string_append_printf(headers_str, "Host: %s:%d\r\n", fla->server_address, fla->server_port);
    g_string_append(headers_str, "Origin: http://www.f-list.net\r\n");
    g_string_append(headers_str, "Sec-WebSocket-Key: ?1:70X 1q057L74,6>\\\r\n");
    g_string_append(headers_str, "Sec-WebSocket-Version: 13\r\n");
    g_string_append(headers_str, "\r\n");
    headers = g_string_free(headers_str, FALSE);

    len = write(fla->fd, headers, strlen(headers)); //TODO: check return value
    fla->connection_status = FLIST_HANDSHAKE;
    g_free(headers);
    
    purple_debug_info(FLIST_DEBUG, "Send handshake...");
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
    const gchar *url = "http://www.f-list.net/json/getApiTicket.php";
    GHashTable *args = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_insert(args, "account", g_strdup(fla->username));
    g_hash_table_insert(args, "password", g_strdup(fla->password));
    
    fla->ticket_request = flist_web_request(url, args, TRUE, flist_receive_ticket, fla); 
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
