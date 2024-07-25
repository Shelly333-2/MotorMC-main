#pragma once
#include "../../main.h"
#include "../../io/packet/packet.h"
#include "../listening.h"

extern bool phd_login(ltg_client_t*, pck_packet_t*);

extern size_t phd_auth_response_write(void*, size_t, size_t, string_t*);

//inbound
extern bool phd_handle_login_start(ltg_client_t*, pck_packet_t*);
extern bool phd_handle_encryption_response(ltg_client_t*, pck_packet_t*);
extern bool phd_handle_login_plugin_response(ltg_client_t*, pck_packet_t*);

//outbound
extern void phd_send_disconnect_login(ltg_client_t*, const char*, size_t);
extern void phd_send_encryption_request(ltg_client_t*);
extern void phd_send_login_success(ltg_client_t*);
extern void phd_send_set_compression(ltg_client_t*);
extern void phd_send_login_plugin_request(ltg_client_t*, const char* identifier, size_t identifier_length, const byte_t* data, size_t data_length);

extern void phd_update_login_success(ltg_client_t* client);