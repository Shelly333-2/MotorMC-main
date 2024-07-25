#include <curl/curl.h>
#include <openssl/evp.h>
#include "login.h"
#include "play.h"
#include "../../util/util.h"
#include "../../motor.h"
#include "../../io/logger/logger.h"
#include "../../io/chat/chat.h"
#include "../../io/chat/translation.h"
#include "../../crypt/random.h"

struct {

	CURL* curl;
	pthread_mutex_t lock;

} phd_authRequest = {
	.curl = NULL,
	.lock = PTHREAD_MUTEX_INITIALIZER
};

size_t phd_auth_response_write(void* ptr, size_t size, size_t nmemb, string_t* r) {
	
	const size_t new_len = r->length + size * nmemb;
	r->value = realloc(r->value, new_len + 1);

	memcpy(r->value + r->length, ptr, size * nmemb);
	r->value[new_len] = '\0';
	r->length = new_len;

	return size*nmemb;

}

bool phd_login(ltg_client_t* client, pck_packet_t* packet) {

	const int32_t id = pck_read_var_int(packet);

	switch (id) {
		case 0x00: {
			return phd_handle_login_start(client, packet);
		}
		case 0x01: {
			return phd_handle_encryption_response(client, packet);
		}
		case 0x02: {
			return phd_handle_login_plugin_response(client, packet);
		}
		default: {
			log_warn("Received unknown packet %x in login state!", id);
			return false;
		}
	}

}

bool phd_handle_login_start(ltg_client_t* client, pck_packet_t* packet) {

	string_t username = {
		.length = pck_read_var_int(packet),
		.value = NULL
	};
	username.value = malloc(username.length + 1);
	username.value[username.length] = '\0';
	pck_read_bytes(packet, (byte_t*) username.value, username.length);
	ltg_client_set_username(client, username);

	if (ltg_client_get_protocol(client) != sky_get_protocol()) {

		cht_translation_t translation = cht_translation_new;

		if (ltg_client_get_protocol(client) < sky_get_protocol()) {
			translation.translate = cht_translation_multiplayer_disconnect_outdated_client;
		} else {
			translation.translate = cht_translation_multiplayer_disconnect_outdated_server;
		}

		cht_component_t version = cht_new;
		version.text = UTL_CSTRTOSTR(__MC_VER__);
		
		cht_add_with(&translation, &version);

		char message[128];
		const size_t message_len = cht_write_translation(&translation, message);

		phd_send_disconnect_login(client, message, message_len);

		cht_term_translation(&translation);

		return false;
	}

	
	if (sky_is_online_mode()) {
		phd_send_encryption_request(client);
	} else {
		phd_update_login_success(client);
	}

	return true;

}

bool phd_handle_encryption_response(ltg_client_t* client, pck_packet_t* packet) {

	struct {
		int32_t length;
		byte_t bytes[128];
	} secret;

	// get shared secret
	secret.length = pck_read_var_int(packet);
	if (secret.length > 128) {
		log_error("Secret length is too big (%d)", secret.length);
		packet->cursor = packet->length;
		pck_log(packet);

		return false;
	}
	pck_read_bytes(packet, secret.bytes, secret.length);

	// decrypt shared secret
	cry_rsa_decrypt(secret.bytes, secret.bytes, secret.length, ltg_get_rsa_keys(sky_get_listener()));
	utl_reverse_bytes(secret.bytes, secret.bytes, LTG_AES_KEY_LENGTH);
	
	// start encryption cypher
	const int enc_res = cfb8_init(secret.bytes, &client->encryption.encrypt, &client->encryption.decrypt);
	if (enc_res != 1) {
		log_error("Could not start encryption cipher! Error code: %d", enc_res);
		return false;
	}
	client->encryption.enabled = true;

	struct {
		int32_t length;
		union {
			byte_t bytes[128];
			uint32_t key;
		};
	} verify;

	// get verify
	verify.length = pck_read_var_int(packet);
	if (verify.length > 128) {
		log_error("Verify length is too big! (%d)", verify.length);
		packet->cursor = packet->length;
		pck_log(packet);

		return false;
	}
	pck_read_bytes(packet, verify.bytes, verify.length);

	// decrypt and check verify
	cry_rsa_decrypt(verify.bytes, verify.bytes, verify.length, ltg_get_rsa_keys(sky_get_listener()));
	if (verify.key != ltg_client_get_id(client)) {

		return false;

	}

	// auth with Mojang's servers...

	long http_code = 0;
	string_t response;

	with_lock (&phd_authRequest.lock) {

		if (phd_authRequest.curl == NULL) {
			phd_authRequest.curl = curl_easy_init();
			if (!phd_authRequest.curl) {
				pthread_mutex_unlock(&phd_authRequest.lock);
				log_error("Failed to initialize cURL");
				return false;
			}
			curl_easy_setopt(phd_authRequest.curl, CURLOPT_TCP_FASTOPEN, 1);
			curl_easy_setopt(phd_authRequest.curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
			curl_easy_setopt(phd_authRequest.curl, CURLOPT_WRITEFUNCTION, phd_auth_response_write);
		}
		CURLcode res;

		// create server_id hash
		EVP_MD_CTX* hash = EVP_MD_CTX_create();
		EVP_DigestInit_ex(hash, EVP_sha1(), NULL);
		EVP_DigestUpdate(hash, (byte_t*) "", 0);
		EVP_DigestUpdate(hash, secret.bytes, LTG_AES_KEY_LENGTH);
		EVP_DigestUpdate(hash, cry_get_asn1_bytes(ltg_get_rsa_keys(sky_get_listener())), cry_get_asn1_length(ltg_get_rsa_keys(sky_get_listener())));
		unsigned int digest_length = 20;
		byte_t server_id_hash[digest_length];
		EVP_DigestFinal_ex(hash, server_id_hash, &digest_length);
		EVP_MD_CTX_destroy(hash);

		// create server_id string
		char server_id[(digest_length << 1) + 2];
		utl_to_minecraft_hex(server_id, server_id_hash, digest_length);

		char request[98 + (digest_length << 1)];
		sprintf(request, "https://sessionserver.mojang.com/session/minecraft/hasJoined?username=%s&serverId=%s", UTL_STRTOCSTR(ltg_client_get_username(client)), server_id);
		curl_easy_setopt(phd_authRequest.curl, CURLOPT_URL, request);

		// prepare response string
		response.length = 0;
		response.value = malloc(1);
		response.value[0] = '\0';

		curl_easy_setopt(phd_authRequest.curl, CURLOPT_WRITEDATA, &response);

		res = curl_easy_perform(phd_authRequest.curl);
		if (res != CURLE_OK) {
			
			pthread_mutex_unlock(&phd_authRequest.lock);

			log_error("Could not authenticate client: %s", curl_easy_strerror(res));

			if (response.value != NULL)
				free(response.value);
			
			return false;

		}

		curl_easy_getinfo(phd_authRequest.curl, CURLINFO_RESPONSE_CODE, &http_code);

	}

	if (http_code != 200) {
		
		log_info("User attempted to login with an invalid session! (Server returned %ld)", http_code);
		return false;

	}

	mjson_doc* auth = mjson_read(response.value, response.length);

	mjson_val* auth_obj = mjson_get_root(auth);
	const uint32_t auth_obj_size = mjson_get_size(auth_obj);
	for (uint32_t i = 0; i < auth_obj_size; ++i) {
		mjson_property auth_prop = mjson_obj_get(auth_obj, i);
		switch (i) {
			case 0: { // id
				utl_read_hex_bytes(client->uuid, mjson_get_string(auth_prop.value), 16);
				break;
			}
			case 1: { // username
				const char* auth_username = mjson_get_string(auth_prop.value);
				if (client->username.length != mjson_get_size(auth_prop.value) || memcmp(client->username.value, auth_username, client->username.length) != 0) {
					// copy new username
					client->username.length = mjson_get_size(auth_prop.value);
					client->username.value = realloc(client->username.value, client->username.length + 1);
					client->username.value[client->username.length] = '\0';
					memcpy(client->username.value, auth_username, client->username.length);
				}
				break;
			}
			case 2: { // properties

				const uint32_t auth_prop_size = mjson_get_size(auth_prop.value);
				for (uint32_t j = 0; j < auth_prop_size; ++j) {
					
					enum {
						none,
						textures
					} property_type = none;

					mjson_val* property = mjson_arr_get(auth_prop.value, j);

					const uint32_t property_size = mjson_get_size(property);
					for (uint32_t k = 0; k < property_size; ++k) {
						mjson_property prop_prop = mjson_obj_get(property, k);
						switch (k) {
							case 0: { // name
								if (strcmp(mjson_get_string(prop_prop.value), "textures") == 0) {
									property_type = textures;
								}
								break;
							}
							case 1: { // value
								switch (property_type) {
									case none: {
										log_error("Property type has not been set, is the json response from the auth server curropted?");
										
										mjson_free(auth);
										free(response.value);
										return false;
									}
									case textures: {
										client->textures.value.length = mjson_get_size(prop_prop.value);
										client->textures.value.value = malloc(client->textures.value.length);
										memcpy(client->textures.value.value, mjson_get_string(prop_prop.value), client->textures.value.length);
										break;
									}
								}
								break;
							}
							case 2: { // signature
								switch (property_type) {
									case none: {
										log_error("Property type has not been set, is the json response from the auth server curropted?");
										
										mjson_free(auth);
										free(response.value);
										return false;
									}
									case textures: {
										client->textures.signature.length = mjson_get_size(prop_prop.value);
										client->textures.signature.value = malloc(client->textures.signature.length);
										memcpy(client->textures.signature.value, mjson_get_string(prop_prop.value), client->textures.signature.length);
										break;
									}
								}
								break;
							}
						}
					}

				}
				break;
			}
		}
	}

	// free auth response and auth json doc
	mjson_free(auth);
	free(response.value);

	phd_update_login_success(client);

	return true;

}

bool phd_handle_login_plugin_response(ltg_client_t* client, pck_packet_t* packet) {

	if ((uint32_t) pck_read_var_int(packet) != ltg_client_get_id(client)) {
		return false;
	}

	if (pck_read_int8(packet)) {
		// successful
	} else {
		// unsuccessful
	}

	return true;

}

void phd_send_disconnect_login(ltg_client_t* client, const char* message, size_t message_len) {

	PCK_INLINE(packet, 1 + message_len, io_big_endian);

	pck_write_var_int(packet, 0x00);

	pck_write_string(packet, message, message_len);

	ltg_send(client, packet);

}

void phd_send_encryption_request(ltg_client_t* client) {

	PCK_INLINE(response, 256, io_big_endian);

	// packet type 0x01
	pck_write_var_int(response, 0x01);

	// server id
	pck_write_string(response, UTL_CSTRTOARG(""));

	// the public auth_key
	pck_write_var_int(response, cry_get_asn1_length(ltg_get_rsa_keys(sky_get_listener())));
	pck_write_bytes(response, cry_get_asn1_bytes(ltg_get_rsa_keys(sky_get_listener())), cry_get_asn1_length(ltg_get_rsa_keys(sky_get_listener())));

	// our verify token
	pck_write_var_int(response, 4);
	pck_write_int32(response, ltg_client_get_id(client));

	ltg_send(client, response);

}

void phd_send_login_success(ltg_client_t* client) {

	PCK_INLINE(response, 32, io_big_endian);

	pck_write_var_int(response, 0x02);
	pck_write_bytes(response, ltg_client_get_uuid(client), 16);
	pck_write_string(response, UTL_STRTOARG(ltg_client_get_username(client)));

	ltg_send(client, response);

}

void phd_send_set_compression(ltg_client_t* client) {

	PCK_INLINE(packet, 15, io_big_endian);

	pck_write_var_int(packet, 0x03);
	pck_write_var_int(packet, sky_get_network_compression_threshold());

	ltg_send(client, packet);

	if (sky_get_network_compression_threshold() > 0)
		client->compression_enabled = true;

}

void phd_send_login_plugin_request(ltg_client_t* client, const char* identifier, size_t identifier_length, const byte_t* data, size_t data_length) {

	PCK_INLINE(packet, identifier_length + data_length + 20, io_big_endian);

	pck_write_var_int(packet, 0x04);
	pck_write_var_int(packet, ltg_client_get_id(client));
	pck_write_string(packet, identifier, identifier_length);

	pck_write_bytes(packet, data, data_length);

	ltg_send(client, packet);

}

void phd_update_login_success(ltg_client_t* client) {

	phd_send_set_compression(client);

	// send login success packet
	phd_send_login_success(client);

	// switch to play state and join game
	ltg_client_set_state(client, ltg_play);
	phd_send_join_game(client);

}