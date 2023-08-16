#include <switch.h>
#include <switch_types.h>
#include <switch_core.h>
#include <switch_curl.h>
#include <string.h>
#include <switch_version.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_apn_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_apn_shutdown);
SWITCH_MODULE_DEFINITION(mod_apn, mod_apn_load, mod_apn_shutdown, NULL);

#define SWITCH_LESS_THAN(x,y)                                                           \
   (((FS_VERSION_MAJOR == x) && (FS_VERSION_MINOR == y)) || \
   ((FS_VERSION_MAJOR == x) && (FS_VERSION_MINOR < y)) || (FS_VERSION_MAJOR < x))

static switch_event_node_t *register_event = NULL;
static switch_event_node_t *push_event = NULL;

static struct {
	switch_memory_pool_t *pool;
	switch_hash_t *profile_hash;
	char *dbname;
	char *odbc_dsn;
	int db_online;
	switch_sql_queue_manager_t *qm;
	switch_mutex_t *dbh_mutex;
	char *contact_voip_token_param;
	char *contact_im_token_param;
	char *contact_app_id_param;
	char *contact_platform_param;
} globals;

enum auth_type {
	NONE,
	JWT,
	BASIC,
	DIGEST
};

struct http_auth_obj {
	enum auth_type type;
	char *data;
};
typedef struct http_auth_obj http_auth_t;

struct profile_obj {
	char *name;
	uint16_t id;
	char *url;
	char *method;
	char *content_type;
	char *post_data_template;
	int timeout;
	int connect_timeout;
	http_auth_t *auth;
};
typedef struct profile_obj profile_t;

struct callback {
	cJSON *array;
};
typedef struct callback callback_t;

struct originate_register_data {
	switch_memory_pool_t *pool;
	char *destination;
	char *realm;
	char *user;
	switch_mutex_t *mutex;
	uint32_t *timelimit;
	switch_bool_t wait_any_register;
};
typedef struct originate_register_data originate_register_t;

enum apn_state {
	MOD_APN_UNDEFINE,
	MOD_APN_SENT,
	MOD_APN_NOTSENT
};

static void push_event_handler(switch_event_t *event);

struct response_event_data {
	char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	enum apn_state state;
	switch_mutex_t *mutex;
};
typedef struct response_event_data response_t;

static switch_cache_db_handle_t *mod_apn_get_db_handle(void)
{
	switch_cache_db_handle_t *dbh = NULL;
	char *dsn;

	if (!zstr(globals.odbc_dsn)) {
		dsn = globals.odbc_dsn;
	} else {
		dsn = globals.dbname;
	}

	if (switch_cache_db_get_db_handle_dsn(&dbh, dsn) != SWITCH_STATUS_SUCCESS) {
		dbh = NULL;
	}

	return dbh;
}

static void execute_sql_now(char **sqlp)
{
	char *sql;

	switch_assert(sqlp && *sqlp);
	sql = *sqlp;

	switch_mutex_lock(globals.dbh_mutex);
	switch_sql_queue_manager_push_confirm(globals.qm, sql, 0, SWITCH_FALSE);
	switch_mutex_unlock(globals.dbh_mutex);

	*sqlp = NULL;
}


static int do_curl(switch_event_t *event, profile_t *profile)
{
	switch_CURL *curl_handle = NULL;
	int httpRes = 0;
	switch_curl_slist_t *headers = NULL;
	char *query = NULL;

	const char *url_template = profile->url;
	const char *method = profile->method;
	const char *content_type = profile->content_type;

	curl_handle = switch_curl_easy_init();
	query = switch_event_expand_headers(event, url_template);

	if (profile->connect_timeout) {
		switch_curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, profile->connect_timeout);
	}

	if (profile->timeout) {
		switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, profile->timeout);
	}

	if (!strncasecmp(query, "https", 5)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Not verifying TLS cert for %s; connection is not secure\n", query);
		switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0);
		switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0);
	}

	if (!strcasecmp(method, "post")) {
		if (!zstr(profile->post_data_template)) {
			char *post_data = switch_event_expand_headers(event, profile->post_data_template);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "method: %s, url: %s, data: %s\n", method, query,
							  post_data);
			switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, strlen(post_data));
			switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, (void *) post_data);
			switch_safe_free(post_data);
		}
		if (content_type) {
			char *ct = switch_mprintf("Content-Type: %s", content_type);
			headers = switch_curl_slist_append(headers, ct);
			switch_safe_free(ct);
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "method: %s, url: %s\n", method, query);
		switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1);
	}
	switch_curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1);
	switch_curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 15);
	if (profile->auth) {
		if (profile->auth->type == DIGEST) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
			switch_curl_easy_setopt(curl_handle, CURLOPT_USERPWD, profile->auth->data);
		} else if (profile->auth->type == BASIC) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
			switch_curl_easy_setopt(curl_handle, CURLOPT_USERPWD, profile->auth->data);
		} else if (profile->auth->type == JWT && !zstr(profile->auth->data)) {
			char *token = switch_mprintf("Authorization: Bearer %s", profile->auth->data);
			headers = switch_curl_slist_append(headers, token);
			switch_safe_free(token);
		}
	}
	if (headers) {
		switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	}
	switch_curl_easy_setopt(curl_handle, CURLOPT_URL, query);
	switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);
	switch_curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "freeswitch-mod_apn/2.0");

	switch_curl_easy_perform(curl_handle);
	switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpRes);
	switch_curl_easy_cleanup(curl_handle);
	switch_curl_slist_free_all(headers);

	if (query != url_template) switch_safe_free(query);

	return httpRes;
}


static switch_bool_t mod_apn_send(switch_event_t *event, profile_t *profile)
{
	switch_bool_t ret = SWITCH_FALSE;

	if (!profile) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "CARUSTO. APN profile not found\n");
		return ret;
	}

	if (do_curl(event, profile) == CURLE_OK) {
		ret = SWITCH_TRUE;
	}

	return ret;
}

static char *mod_apn_execute_sql2str(char *sql, char *resbuf, size_t len)
{
	char *ret = NULL, *err = NULL;
	switch_cache_db_handle_t *dbh = NULL;

	switch_mutex_lock(globals.dbh_mutex);

	if (!(dbh = mod_apn_get_db_handle())) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error Opening DB\n");
		switch_mutex_unlock(globals.dbh_mutex);
		return NULL;
	}

	ret = switch_cache_db_execute_sql2str(dbh, sql, resbuf, len, &err);

	switch_mutex_unlock(globals.dbh_mutex);

	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "SQL ERR: [%s]\n%s\n", err, sql);
		free(err);
	}

	switch_cache_db_release_db_handle(&dbh);

	return ret;
}

static switch_bool_t mod_apn_execute_sql_callback(char *sql, switch_core_db_callback_func_t callback, void *pdata)
{
	switch_bool_t ret = SWITCH_FALSE;
	char *errmsg = NULL;
	switch_cache_db_handle_t *dbh = NULL;

	if (!(dbh = mod_apn_get_db_handle())) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error Opening DB\n");
		goto end;
	}

	switch_cache_db_execute_sql_callback(dbh, sql, callback, pdata, &errmsg);

	if (errmsg) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "SQL ERR: [%s] %s\n", sql, errmsg);
		free(errmsg);
	}

end:
	if (dbh) {
		switch_cache_db_release_db_handle(&dbh);
	}

	return ret;
}

static int sql2str_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	callback_t *cbt = (callback_t *) pArg;
	cJSON *item = NULL;

	if (!cbt) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Something wrong with callback structure\n");
		return 0;
	}

	if (argc < 3) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Something wrong with SQL callback (%d)\n", argc);
		return 0;
	}

	if (zstr(argv[0]) || zstr(argv[1]) || zstr(argv[2])) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Something wrong with SQL callback (token: '%s', platform: '%s', app_id: '%s')\n", argv[0], argv[1], argv[2]);
		return 0;
	}

	item = cJSON_CreateObject();
	cJSON_AddItemToArray(cbt->array, item);
	cJSON_AddItemToObject(item, "platform", cJSON_CreateString(argv[0]));
	cJSON_AddItemToObject(item, "app_id", cJSON_CreateString(argv[1]));
	cJSON_AddItemToObject(item, "token", cJSON_CreateString(argv[2]));

	return 0;
}

#define APN_USAGE """{\"uuid\":\"\",\"realm\":\"\",\"user\":\"\",\"type\":\"[im|voip]\",\"payload\":{\"body\":\"\",\"sound\":\"\",\"\":[{\"name\":\"\",\"value\":\"\"},\"image\":\"\",\"category\":\"\"}}"""
SWITCH_STANDARD_API(apn_api_function)
{
	char *pdata = NULL, *json_payload = NULL;
	cJSON *root = NULL, *payload = NULL;
	switch_event_t *event = NULL;
	switch_status_t res = SWITCH_STATUS_FALSE;

	if (cmd) {
		pdata = strdup(cmd);
	}

	if (!pdata) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No data\n");
		stream->write_function(stream, "USAGE: %s", APN_USAGE);
		goto end;
	}

	root = cJSON_Parse(pdata);
	if (!root) {
		stream->write_function(stream, "Wrong JSON data. USAGE: %s", APN_USAGE);
		goto end;
	}

	if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "Can't create event");
		goto end;
	}

	payload = cJSON_GetObjectItem(root, "payload");
	if (payload) {
		json_payload = cJSON_PrintUnformatted(payload);
		switch_event_add_body(event, json_payload);
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "type", cJSON_GetObjectCstr(root, "type"));
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "user", cJSON_GetObjectCstr(root, "user"));
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "realm", cJSON_GetObjectCstr(root, "realm"));
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "uuid", cJSON_GetObjectCstr(root, "uuid"));

	push_event_handler(event);

	res = SWITCH_STATUS_SUCCESS;
	stream->write_function(stream, "Sent");

end:

	if (event) {
		switch_event_destroy(&event);
	}

	if (root) {
		cJSON_Delete(root);
	}

	switch_safe_free(json_payload);
	switch_safe_free(pdata);

	return res;
}

// auth_type: "none|jwt|basic|digest"
// auth_data: "token"|"login:password"
static http_auth_t *parse_auth_param(char *auth_type, char *auth_data, switch_memory_pool_t *pool)
{
	http_auth_t *res = NULL;

	if ((res = switch_core_alloc(pool, sizeof(*res))) == NULL) {
		return NULL;
	}
	res->type = NONE;

	if (zstr(auth_type)) {
		return res;
	}

	if (!zstr(auth_type)) {
		if (!strcasecmp(auth_type, "jwt")) {
			res->type = JWT;
		} else if (!strcasecmp(auth_type, "basic")) {
			res->type = BASIC;
		} else if (!strcasecmp(auth_type, "digest")) {
			res->type = DIGEST;
		} else if (!strcasecmp(auth_type, "none")) {
			res->type = NONE;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_apn doesn't support type auth: %s\n", auth_type);
		}

		if (!zstr(auth_data)) {
			res->data = switch_core_strdup(pool, auth_data);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Something wrong with auth data\n");
			res->type = NONE;
		}
	}

	return res;
}

static switch_status_t do_config(switch_memory_pool_t *pool)
{
	char *cf = "apn.conf";
	switch_xml_t cfg, xml, settings, param, x_profile, x_profiles;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	profile_t *profile = NULL;
	switch_cache_db_handle_t *dbh = NULL;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "open of %s failed\n", cf);
		return SWITCH_STATUS_TERM;
	}

	if (globals.db_online) {
		switch_mutex_destroy(globals.dbh_mutex);
		globals.db_online = 0;
	}

	if (globals.qm) {
		switch_sql_queue_manager_destroy(&globals.qm);
		globals.qm = NULL;
	}

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;
	globals.db_online = 1;
	switch_mutex_init(&globals.dbh_mutex, SWITCH_MUTEX_NESTED, pool);

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = NULL;
			char *val = NULL;
			var = (char *) switch_xml_attr_soft(param, "name");
			val = (char *) switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "odbc_dsn") && !zstr(val)) {
				globals.odbc_dsn = switch_core_strdup(globals.pool, val);
			} else if (!strcasecmp(var, "contact_voip_token_param") && !zstr(val)) {
				globals.contact_voip_token_param = switch_core_strdup(globals.pool, val);
			} else if (!strcasecmp(var, "contact_platform_param") && !zstr(val)) {
				globals.contact_platform_param = switch_core_strdup(globals.pool, val);
			} else if (!strcasecmp(var, "contact_im_token_param") && !zstr(val)) {
				globals.contact_im_token_param = switch_core_strdup(globals.pool, val);
			} else if (!strcasecmp(var, "contact_app_id_param") && !zstr(val)) {
				globals.contact_app_id_param = switch_core_strdup(globals.pool, val);
			}
		}
	}

	if (zstr(globals.contact_voip_token_param)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "contact_voip_token_param not set\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	if (zstr(globals.contact_platform_param)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "contact_platform_param not set\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	if (zstr(globals.contact_app_id_param)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "contact_app_id_param not set\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	if (globals.odbc_dsn) {
		if (!(dbh = mod_apn_get_db_handle())) {
			globals.odbc_dsn = NULL;
		}
	}

	if (zstr(globals.odbc_dsn)) {
		globals.dbname = "carusto";
		dbh = mod_apn_get_db_handle();
	}

	if (!dbh) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "dbh is NULL\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	switch_core_hash_init(&globals.profile_hash);
	if ((x_profiles = switch_xml_child(cfg, "profiles"))) {
		for (x_profile = switch_xml_child(x_profiles, "profile"); x_profile; x_profile = x_profile->next) {
			char *name = (char *) switch_xml_attr_soft(x_profile, "name");
			char *id_s = NULL, *url = NULL, *method = NULL, *auth_type = NULL, *auth_data = NULL, *content_type = NULL,
					*connect_timeout = NULL, *timeout = NULL, *post_data_template = NULL;

			for (param = switch_xml_child(x_profile, "param"); param; param = param->next) {
				char *var, *val;
				var = (char *) switch_xml_attr_soft(param, "name");
				val = (char *) switch_xml_attr_soft(param, "value");

				if (!strcasecmp(var, "id") && !zstr(val)) {
					id_s = val;
				} else if (!strcasecmp(var, "url") && !zstr(val)) {
					url = val;
				} else if (!strcasecmp(var, "method") && !zstr(val)) {
					method = val;
				} else if (!strcasecmp(var, "auth_type") && !zstr(val)) {
					auth_type = val;
				} else if (!strcasecmp(var, "auth_data") && !zstr(val)) {
					auth_data = val;
				} else if (!strcasecmp(var, "content_type") && !zstr(val)) {
					content_type = val;
				} else if (!strcasecmp(var, "connect_timeout") && !zstr(val)) {
					connect_timeout = val;
				} else if (!strcasecmp(var, "timeout") && !zstr(val)) {
					timeout = val;
				} else if (!strcasecmp(var, "post_data_template") && !zstr(val)) {
					post_data_template = val;
				}
			}

			if (zstr(url) || zstr(method)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No url (%s) or method (%s) specified.\n", url, method);
				goto done;
			}

			if (strcasecmp(method, "get") && strcasecmp(method, "post")) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Method %s doesn't support\n", method);
				goto done;
			}

			if (zstr(name)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No name specified.\n");
			} else {
				profile = switch_core_alloc(globals.pool, sizeof(*profile));
				memset(profile, 0, sizeof(profile_t));
				profile->name = switch_core_strdup(globals.pool, name);

				if (!zstr(id_s)) {
					profile->id = (uint16_t)strtol(id_s, NULL, 10);
				}

				profile->url = switch_core_strdup(globals.pool, url);
				profile->method = switch_core_strdup(globals.pool, method);
				if (!zstr(content_type)) {
					profile->content_type = switch_core_strdup(globals.pool, content_type);
				}
				if (!zstr(connect_timeout)) {
					profile->connect_timeout = (int)strtol(connect_timeout, NULL, 10);
				}
				if (!zstr(timeout)) {
					profile->timeout = (int)strtol(timeout, NULL, 10);
				}
				if (!zstr(post_data_template)) {
					profile->post_data_template = switch_core_strdup(globals.pool, post_data_template);
				} else {
					profile->post_data_template = switch_core_strdup(globals.pool, "{\"type\": \"${type}\",\"app\":\"${app_id}\","
																				   "\"token\":\"${token}\",\"user\":\"${user}\","
																				   "\"realm\":\"${realm}\",\"payload\":${payload}"
																				   "\"platform\":\"${platform}\"}");
				}
				profile->auth = parse_auth_param(auth_type, auth_data, globals.pool);
				if (!zstr(content_type)) {
					profile->content_type = switch_core_strdup(globals.pool, content_type);
				}

				switch_core_hash_insert(globals.profile_hash, profile->name, profile);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Loaded APN profile '%s'\n", profile->name);
			}
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "No APN profiles defined.\n");
	}

	switch_sql_queue_manager_init_name("mod_apn", &globals.qm, 2, globals.odbc_dsn ? globals.odbc_dsn : globals.dbname, SWITCH_MAX_TRANS, NULL, NULL, NULL, NULL);
	switch_sql_queue_manager_start(globals.qm);

done:
	if (dbh) {
		switch_cache_db_release_db_handle(&dbh);
	}
	switch_xml_free(xml);
	return status;
}

static void db_get_tokens_array(char *user, char *realm, char *type, callback_t *cbt)
{
	char *query = NULL;
	if (zstr(user) || zstr(realm)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No parameters for get token. user: '%s', realm: '%s'\n", user, realm);
		return;
	}

	query = switch_mprintf("SELECT platform, app_id, token FROM push_tokens WHERE extension = '%q' AND realm = '%q' AND type = '%q'", user, realm, type);
	mod_apn_execute_sql_callback(query, sql2str_callback, cbt);

	switch_safe_free(query);
}

static void add_item_to_event(switch_event_t *event, char *name, cJSON *obj)
{
	if (zstr(name) || !event || !obj) {
		return;
	}

	switch_event_del_header(event, name);
	if (obj->type == cJSON_String && obj->valuestring && !zstr(obj->valuestring)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, name, obj->valuestring);
	}
}

static void push_event_handler(switch_event_t *event)
{
	char *payload = NULL, *user = NULL, *realm = NULL, *type = NULL, *uuid = NULL, *json_tokens = NULL;
	profile_t *profile = NULL;
	callback_t cbt = { cJSON_CreateArray() };
	switch_bool_t res = SWITCH_FALSE;
	cJSON *iterator;
	int size, i;
	switch_event_t *res_event;

	payload = switch_event_get_body(event);
	type = switch_event_get_header(event, "type");
	user = switch_event_get_header(event, "user");
	realm = switch_event_get_header(event, "realm");
	uuid = switch_event_get_header(event, "uuid");

	if (zstr(type) || zstr(user) || zstr(realm)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No parameters, data: '%s', type: '%s', user: '%s', realm: '%s'\n", payload, type, user, realm);
		goto end;
	}

	if (!(profile = switch_core_hash_find(globals.profile_hash, type))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Profile '%s' not found\n", type);
		goto end;
	}

	if (!zstr(payload)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "payload", payload);
	}

	db_get_tokens_array(user, realm, type, &cbt);

	if ((size = cJSON_GetArraySize(cbt.array)) == 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No one token found for type: '%s', user: '%s', realm: '%s'\n", type, user, realm);
		goto end;
	}

	for (i = 0; i < size; i++) {
		if ((iterator = cJSON_GetArrayItem(cbt.array, i)) == NULL) {
			break;
		}
		add_item_to_event(event, "token", cJSON_GetObjectItem(iterator, "token"));
		add_item_to_event(event, "app_id", cJSON_GetObjectItem(iterator, "app_id"));
		add_item_to_event(event, "platform", cJSON_GetObjectItem(iterator, "platform"));

		res |= mod_apn_send(event, profile);
	}

end:
	if (!zstr(uuid) && switch_event_create_subclass(&res_event, SWITCH_EVENT_CUSTOM, "mobile::push::response") == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(res_event, SWITCH_STACK_BOTTOM, "uuid", uuid);
		switch_event_add_header_string(res_event, SWITCH_STACK_BOTTOM, "response", res ? "sent" : "notsent");
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Fire event mobile::push::response with ID: '%s' and result: '%s'\n", uuid, res ? "sent" : "notsent");
		switch_event_fire(&res_event);
		switch_event_destroy(&res_event);
	}

	switch_safe_free(json_tokens);

	if (cbt.array) {
		cJSON_Delete(cbt.array);
	}
}

static char *get_url_from_contact(char *buf)
{
	char *url = NULL, *e;

	switch_assert(buf);

	while(*buf == ' ') {
		buf++;
	}

	if (*buf == '"') {
		buf++;
		if((e = strchr(buf, '"'))) {
			buf = e+1;
		}
	}

	while(*buf == ' ') {
		buf++;
	}

	url = strchr(buf, '<');

	if (url && (e = switch_find_end_paren(url, '<', '>'))) {
		url++;
		url = strdup(url);
		e = strchr(url, '>');
		*e = '\0';
	} else {
		if (url) buf++;
		url = strdup(buf);
	}
	return url;
}

static void originate_register_event_handler(switch_event_t *event)
{
	char *dest = NULL;
	originate_register_t *originate_data = (struct originate_register_data *)event->bind_user_data;
	char *event_username = NULL, *event_realm = NULL, *event_call_id = NULL, *event_contact = NULL, *event_profile = NULL;
	char *destination = NULL;
	const char *domain_name = NULL, *dial_user = NULL, *update_reg = NULL;
	uint32_t timelimit_sec = 0;

	switch_memory_pool_t *pool;
	switch_mutex_t *handles_mutex;

	if (!originate_data) {
		return;
	}

	pool = originate_data->pool;
	handles_mutex = originate_data->mutex;
	domain_name = originate_data->realm;
	dial_user = originate_data->user;

	update_reg = switch_event_get_header(event, "update-reg");
	if (!zstr(update_reg) && switch_true(update_reg)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Update existing registration, skip originate\n");
		return;
	}

	event_username = switch_event_get_header(event, "username");
	event_realm = switch_event_get_header(event, "realm");
	event_call_id = switch_event_get_header(event, "call-id");
	event_contact = switch_event_get_header(event, "contact");
	event_profile = switch_event_get_header(event, "profile-name");

	if (zstr(event_username) || zstr(event_realm) || zstr(event_call_id) || zstr(event_profile) ||  zstr(event_contact) || zstr(domain_name) || zstr(dial_user)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No parameter for originate call via sofia::register\n");
		return;
	}

	if (strcasecmp(event_realm, domain_name) || strcasecmp(event_username, dial_user)) {
		return;
	}

	dest = get_url_from_contact(event_contact);

	if (zstr(dest)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No destination contact data string\n");
		goto end;
	}

	timelimit_sec = *originate_data->timelimit;

	destination = switch_mprintf("[registration_token=%s,originate_timeout=%u]sofia/%s/%s:_:[originate_timeout=%u,enable_send_apn=false,apn_wait_any_register=%s]apn_wait/%s@%s",
								 event_call_id,
								 timelimit_sec,
								 event_profile,
								 dest,
								 timelimit_sec,
								 originate_data->wait_any_register == SWITCH_TRUE ? "true" : "false",
								 event_username,
								 event_realm);

	switch_mutex_lock(handles_mutex);
	originate_data->destination = switch_core_strdup(pool, destination);
	switch_mutex_unlock(handles_mutex);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Try originate to '%s' (by registration event)\n", destination);

end:
	switch_safe_free(destination);
	switch_safe_free(dest);
}

static void register_event_handler(switch_event_t *event)
{
	char *event_user = NULL, *event_realm = NULL, *event_contact = NULL;
	char *contact_ptr = NULL, *voip_token = NULL, *im_token = NULL, *platform = NULL, *foo = NULL, *query = NULL, *app_id = NULL;
	char *update_reg = NULL;

	update_reg = switch_event_get_header(event, "update-reg");
	if (!zstr(update_reg) && switch_true(update_reg)) {
		return;
	}

	event_contact = switch_event_get_header(event, "contact");
	if (zstr(event_contact)) {
		return;
	}

	contact_ptr = app_id = voip_token = im_token = platform = strdup(event_contact);

	if (zstr(contact_ptr)) {
		goto end;
	}

	/*Get contact parameter pn-os */
	if (!zstr(globals.contact_platform_param) && (foo = strcasestr(platform, globals.contact_platform_param))) {
		platform = foo + strlen(globals.contact_platform_param) + 1;
	} else {
		platform = NULL;
	}

	/*Get contact parameter pn-voip-tok */
	if (!zstr(globals.contact_voip_token_param) && (foo = strcasestr(voip_token, globals.contact_voip_token_param))) {
		voip_token = foo + strlen(globals.contact_voip_token_param) + 1;
	} else {
		voip_token = NULL;
	}

	/*Get contact parameter pn-im-tok */
	if (!zstr(globals.contact_im_token_param) && (foo = strcasestr(im_token, globals.contact_im_token_param))) {
		im_token = foo + strlen(globals.contact_im_token_param) + 1;
	} else {
		im_token = NULL;
	}

	/*Get contact parameter app-id */
	if (!zstr(globals.contact_app_id_param) && (foo = strcasestr(app_id, globals.contact_app_id_param))) {
		app_id = foo + strlen(globals.contact_app_id_param) + 1;
	} else {
		app_id = NULL;
	}

	if (voip_token && (foo = strchr(voip_token, ';')) != NULL) {
		*foo = '\0';
	}
	if (im_token && (foo = strchr(im_token, ';')) != NULL) {
		*foo = '\0';
	}
	if (app_id && (foo = strchr(app_id, ';')) != NULL) {
		*foo = '\0';
	}
	if (platform && (foo = strchr(platform, ';')) != NULL) {
		*foo = '\0';
	}

	if (zstr(app_id) || (zstr(voip_token) && zstr(im_token)) || zstr(platform)) {
		goto end;
	}

	event_user = switch_event_get_header(event, "from-user");
	event_realm = switch_event_get_header(event, "realm");

	if (zstr(event_user) || zstr(event_realm)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No parameter\n");
		goto end;
	}

	/*Check existing VoIP token*/
	if (!zstr(voip_token)) {
		char voip_count[2] = {0, };
		query = switch_mprintf("SELECT count(*) FROM push_tokens WHERE token = '%q' AND extension = '%q' AND realm = '%q' AND app_id = '%q' AND type = 'voip'", voip_token, event_user, event_realm, app_id);
		mod_apn_execute_sql2str(query, voip_count, sizeof(voip_count));
		switch_safe_free(query);

		if (!zstr(voip_count) && strtol(voip_count, NULL, 10) == 0) {
			/*Add new VoIP token to DB*/
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Add new VoIP token: '%s' to push_tokens for user %s@%s and application: %s\n", voip_token, event_user, event_realm, app_id);
			query = switch_mprintf("INSERT INTO push_tokens (token, extension, realm, app_id, type, platform) VALUES ('%q', '%q', '%q', '%q', 'voip', '%q')", voip_token, event_user, event_realm, app_id, platform);
			execute_sql_now(&query);
			switch_safe_free(query);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. VoIP token: '%s' for user %s@%s and application: %s already exists\n", voip_token, event_user, event_realm, app_id);
			query = switch_mprintf("UPDATE push_tokens SET last_update = CURRENT_TIMESTAMP WHERE token = '%q' AND extension = '%q' AND realm = '%q' AND app_id = '%q' AND type = 'voip'", im_token, event_user, event_realm, app_id);
			execute_sql_now(&query);
			switch_safe_free(query);
		}
	}

	/*Check existing IM token*/
	if (!zstr(im_token)) {
		char im_count[2] = {0, };
		query = switch_mprintf("SELECT count(*) FROM push_tokens WHERE token = '%q' AND extension = '%q' AND realm = '%q' AND app_id = '%q' AND type = 'im'", im_token, event_user, event_realm, app_id);
		mod_apn_execute_sql2str(query, im_count, sizeof(im_count));
		switch_safe_free(query);

		if (!zstr(im_count) && strtol(im_count, NULL, 10) == 0) {
			/*Add new IM token to DB*/
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Add new IM token: '%s' to push_tokens for user %s@%s and application: %s\n", im_token, event_user, event_realm, app_id);
			query = switch_mprintf("INSERT INTO push_tokens (token, extension, realm, app_id, type, platform) VALUES ('%q', '%q', '%q', '%q', 'im', '%q')", im_token, event_user, event_realm, app_id, platform);
			execute_sql_now(&query);
			switch_safe_free(query);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. IM token: '%s' for user %s@%s and application: %s already exists\n", im_token, event_user, event_realm, app_id);
			query = switch_mprintf("UPDATE push_tokens SET last_update = CURRENT_TIMESTAMP WHERE token = '%q' AND extension = '%q' AND realm = '%q' AND app_id = '%q' AND type = 'im'", im_token, event_user, event_realm, app_id);
			execute_sql_now(&query);
			switch_safe_free(query);
		}
	}

	end:
	switch_safe_free(contact_ptr);
}

static int init_sql(void)
{
	char sql[] =
		"CREATE TABLE push_tokens ("
			"id				serial NOT NULL,"
			"token			VARCHAR(255) NOT NULL,"
			"extension		VARCHAR(255) NOT NULL,"
			"realm			VARCHAR(255) NOT NULL,"
			"app_id			VARCHAR(255) NOT NULL,"
			"type			VARCHAR(255) NOT NULL,"
			"platform	    VARCHAR(255) NOT NULL,"
			"last_update    timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,"
			"CONSTRAINT push_tokens_pkey PRIMARY KEY (id)"
		");";

	switch_cache_db_handle_t *dbh = mod_apn_get_db_handle();

	if (!dbh) {
		return 0;
	}

	switch_cache_db_test_reactive(dbh, "SELECT count(*) FROM push_tokens", NULL, sql);
	switch_cache_db_release_db_handle(&dbh);

	return 1;
}

static void response_event_handler(switch_event_t *event)
{
	char *uuid = NULL, *response = NULL;
	response_t *data = (response_t *)event->bind_user_data;

	uuid = switch_event_get_header(event, "uuid");
	if (zstr(uuid) || !data) {
		return;
	}

	if (strcmp(uuid, data->uuid) != 0) {
		return;
	}

	response = switch_event_get_header(event, "response");
	if (zstr(response)) {
		return;
	}

	switch_mutex_lock(data->mutex);
	if (!strcasecmp(response, "sent")) {
		data->state = MOD_APN_SENT;
	} else {
		data->state = MOD_APN_NOTSENT;
	}
	switch_mutex_unlock(data->mutex);
}

/* fake user_wait */
switch_endpoint_interface_t *apn_wait_endpoint_interface;
static switch_call_cause_t apn_wait_outgoing_channel(switch_core_session_t *session,
			 switch_event_t *var_event,
			 switch_caller_profile_t *outbound_profile,
			 switch_core_session_t **new_session, switch_memory_pool_t **pool, switch_originate_flag_t flags,
			 switch_call_cause_t *cancel_cause);

switch_io_routines_t apn_wait_io_routines = {
		/*.outgoing_channel */ apn_wait_outgoing_channel
};

static switch_call_cause_t apn_wait_outgoing_channel(switch_core_session_t *session,
			 switch_event_t *var_event,
			 switch_caller_profile_t *outbound_profile,
			 switch_core_session_t **new_session, switch_memory_pool_t **_pool, switch_originate_flag_t flags,
			 switch_call_cause_t *cancel_cause)
{
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;
	uint32_t timelimit_sec = 0;
	uint32_t current_timelimit = 0;
	char *user = NULL, *domain = NULL, *dup_domain = NULL;
	char *var_val = NULL;
	switch_event_t *event = NULL;
	switch_event_node_t *response_event = NULL, *register_event = NULL;
	switch_channel_t *channel = NULL;
	switch_memory_pool_t *pool = NULL;
	char *cid_name_override = NULL, *cid_num_override = NULL;
	originate_register_t originate_data = { 0, };
	char *destination = NULL;
	switch_bool_t wait_any_register = SWITCH_FALSE;
	response_t apn_response = { {0, }, MOD_APN_UNDEFINE, NULL };
	switch_time_t start = 0;
	int diff = 0;

	if (var_event && !zstr(switch_event_get_header(var_event, "originate_reg_token"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Skip originate in case have custom originate token registration\n");
		return cause;
	}

	start = switch_epoch_time_now(NULL);
	switch_core_new_memory_pool(&pool);

	if (!pool) {
		return cause;
	}

	if (session) {
		channel = switch_core_session_get_channel(session);
	}

	if (!outbound_profile || zstr(outbound_profile->destination_number)) {
		goto done;
	}

	user = switch_core_strdup(pool, outbound_profile->destination_number);
	if (!user) {
		goto done;
	}

	if ((domain = strchr(user, '@'))) {
		*domain++ = '\0';
	} else {
		domain = switch_core_get_domain(SWITCH_TRUE);
		dup_domain = domain;
	}

	if (!domain) {
		goto done;
	}

	switch_uuid_str(apn_response.uuid, sizeof(apn_response.uuid));
	switch_mutex_init(&apn_response.mutex, SWITCH_MUTEX_NESTED, pool);

	if (var_event) {
		cid_name_override = switch_event_get_header(var_event, "origination_caller_id_name");
		cid_num_override = switch_event_get_header(var_event, "origination_caller_id_number");
		if ((var_val = switch_event_get_header(var_event, "originate_timeout"))) {
			int tmp = (int)strtol(var_val, NULL, 10);
			if (tmp > 0) {
				timelimit_sec = (uint32_t) tmp;
			}
		}
	}

	if (timelimit_sec <= 0) {
		timelimit_sec = 60;
	}

	current_timelimit = timelimit_sec;

	originate_data.pool = pool;
	originate_data.realm = switch_core_strdup(pool, domain);
	originate_data.user = switch_core_strdup(pool, user);
	originate_data.destination = NULL;
	originate_data.mutex = NULL;
	originate_data.timelimit = 0;
	originate_data.wait_any_register = SWITCH_FALSE;

	switch_mutex_init(&originate_data.mutex, SWITCH_MUTEX_NESTED, pool);

	if (var_event && switch_true(switch_event_get_header(var_event, "apn_wait_any_register"))) {
		wait_any_register = originate_data.wait_any_register = SWITCH_TRUE;
	}

	originate_data.timelimit = &current_timelimit;

	/*Bind to event 'sofia::register' for originate call to registration*/
	switch_event_bind_removable("apn_originate_register", SWITCH_EVENT_CUSTOM, "sofia::register", originate_register_event_handler, &originate_data, &register_event);

	if (wait_any_register == SWITCH_FALSE) {
		if ((switch_event_bind_removable(modname, SWITCH_EVENT_CUSTOM, "mobile::push::response", response_event_handler, &apn_response, &response_event) != SWITCH_STATUS_SUCCESS)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't bind event!\n");
			goto done;
		}
	}

	/*Create event 'mobile::push::notification' for send push notification*/
	if (!var_event || (var_event && (!(var_val = switch_event_get_header(var_event, "enable_send_apn")) || zstr(var_val) || switch_true(var_val)))) {
		if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "mobile::push::notification") == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "uuid", apn_response.uuid);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "type", "voip");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "user", user);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "realm", domain);
			switch_event_add_body(event, "{\"content-available\":true,\"custom\":[{\"name\":\"content-message\",\"value\":\"incomming call\"}]}");
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Fire event APN for User: %s@%s\n", user, domain);
			switch_event_fire(&event);
			switch_event_destroy(&event);
		}
	}

	while (current_timelimit > 0) {
		diff = (int)(switch_epoch_time_now(NULL) - start);
		current_timelimit = timelimit_sec - diff;

		if (wait_any_register != SWITCH_TRUE) {
			switch_mutex_lock(apn_response.mutex);
			if (apn_response.state == MOD_APN_NOTSENT) {
				switch_mutex_unlock(apn_response.mutex);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Event APN don't sent to %s@%s, so stop wait for incoming register\n", user, domain);
				break;
			}
			switch_mutex_unlock(apn_response.mutex);
		}

		if (session) {
			switch_ivr_parse_all_messages(session);
		}

		if (channel && !switch_channel_ready(channel)) {
			break;
		}

		if (cancel_cause && *cancel_cause > 0) {
			break;
		}

		switch_mutex_lock(originate_data.mutex);
		if (!zstr(originate_data.destination)) {
			destination = switch_core_strdup(pool, originate_data.destination);
		}
		switch_mutex_unlock(originate_data.mutex);

		if (!zstr(destination)) {
			/*Unbind from 'sofia::register' event for current originate route*/
			if (register_event) {
				switch_event_unbind(&register_event);
				register_event = NULL;
			}


#if SWITCH_LESS_THAN(1,8)
			if (switch_ivr_originate(session, new_session, &cause, destination, current_timelimit, NULL,
					cid_name_override, cid_num_override, outbound_profile, var_event, flags,
					cancel_cause) == SWITCH_STATUS_SUCCESS) {
#else
			if (switch_ivr_originate(session, new_session, &cause, destination, current_timelimit, NULL,
					cid_name_override, cid_num_override, outbound_profile, var_event, flags,
					cancel_cause, NULL) == SWITCH_STATUS_SUCCESS) {
#endif
				const char *context;
				switch_caller_profile_t *cp;
				switch_channel_t *new_channel = NULL;

				new_channel = switch_core_session_get_channel(*new_session);

				if ((context = switch_channel_get_variable(new_channel, "context"))) {
					if ((cp = switch_channel_get_caller_profile(new_channel))) {
						cp->context = switch_core_strdup(cp->pool, context);
					}
				}
				switch_core_session_rwunlock(*new_session);
			}
			break;
		}
		switch_cond_next();
		switch_yield(1000);
	}

done:
	if (response_event) {
		switch_event_unbind(&response_event);
		response_event = NULL;
	}
	if (register_event) {
		switch_event_unbind(&register_event);
		register_event = NULL;
	}
	if (apn_response.mutex) {
		switch_mutex_destroy(apn_response.mutex);
	}
	switch_safe_free(dup_domain);
	if (pool) {
		switch_core_destroy_memory_pool(&pool);
	}

	return cause;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_apn_load)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_api_interface_t *api_interface;

	if ((status = do_config(pool) != SWITCH_STATUS_SUCCESS)) {
		goto error;
	}

	if (!init_sql()) {
		goto error;
	}

	/*Bind to event sofia::register for add new tokens from contact parameters*/
	if ((switch_event_bind_removable(modname, SWITCH_EVENT_CUSTOM, "sofia::register", register_event_handler, NULL, &register_event) != SWITCH_STATUS_SUCCESS)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't bind event!\n");
		goto error;
	}

	/*Bind to event mobile::push::notification for send MobilePushNotification */
	if ((switch_event_bind_removable(modname, SWITCH_EVENT_CUSTOM, "mobile::push::notification", push_event_handler, NULL, &push_event) != SWITCH_STATUS_SUCCESS)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't bind event!\n");
		goto error;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_API(api_interface, "apn", "APN Service", apn_api_function, APN_USAGE);

	apn_wait_endpoint_interface = (switch_endpoint_interface_t *) switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
	apn_wait_endpoint_interface->interface_name = "apn_wait";
	apn_wait_endpoint_interface->io_routines = &apn_wait_io_routines;

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;

error:
	if (globals.qm) {
		switch_sql_queue_manager_destroy(&globals.qm);
		globals.qm = NULL;
	}
	if (register_event) {
		switch_event_unbind(&register_event);
		register_event = NULL;
	}
	if (push_event) {
		switch_event_unbind(&push_event);
		push_event = NULL;
	}
	if (globals.profile_hash) {
		switch_core_hash_destroy(&globals.profile_hash);
		globals.profile_hash = NULL;
	}
	return SWITCH_STATUS_TERM;
}


SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_apn_shutdown)
{
	if (globals.qm) {
		switch_sql_queue_manager_destroy(&globals.qm);
		globals.qm = NULL;
	}
	if (register_event) {
		switch_event_unbind(&register_event);
		register_event = NULL;
	}
	if (push_event) {
		switch_event_unbind(&push_event);
		push_event = NULL;
	}
	if (globals.profile_hash) {
		switch_core_hash_destroy(&globals.profile_hash);
		globals.profile_hash = NULL;
	}

	return SWITCH_STATUS_SUCCESS;
}

