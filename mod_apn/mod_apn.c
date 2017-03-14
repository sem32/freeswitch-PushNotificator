#include <switch.h>
#include <switch_types.h>
#include <switch_core.h>

#include <capn/apn.h>
#include <capn/apn_array.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_apn_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_apn_shutdown);
SWITCH_MODULE_DEFINITION(mod_apn, mod_apn_load, mod_apn_shutdown, NULL);

static switch_event_node_t *register_event = NULL;
static switch_event_node_t *push_event = NULL;

static struct {
	switch_memory_pool_t *pool;
	switch_bool_t debug;
	switch_hash_t *profile_hash;
	char *dbname;
	char *odbc_dsn;
	int db_online;
	switch_sql_queue_manager_t *qm;
	switch_mutex_t *dbh_mutex;
	int init_lib;
	char *voip_app;
} globals;

struct profile_obj {
    char *name;
    uint16_t id;
    char *path_p12;
    char *password;
    char *path_cert;
    char *path_key;
    switch_bool_t sandbox;
};
typedef struct profile_obj profile_t;

struct callback {
	cJSON *root;
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

struct response_event_data {
	char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	enum apn_state state;
	switch_mutex_t *mutex;
};
typedef struct response_event_data response_t;

static void mod_apn_logging(apn_log_levels level, const char * const message, uint32_t len) {
    switch_log_level_t fs_level = SWITCH_LOG_DEBUG;
    switch(level) {
        case APN_LOG_LEVEL_INFO:
        	fs_level = SWITCH_LOG_INFO;
            break;
        case APN_LOG_LEVEL_ERROR:
        	fs_level = SWITCH_LOG_ERROR;
            break;
        case APN_LOG_LEVEL_DEBUG:
        	fs_level = SWITCH_LOG_DEBUG;
            break;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, fs_level, "======>[apn] %s\n", message);
}

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

static void mod_apn_invalid_token(const char * const token, uint32_t index) {
	char *query = NULL;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "CARUSTO. Delete invalid token: %s\n", token);
	query = switch_mprintf("DELETE FROM apple_tokens WHERE token = '%q'", token);
	execute_sql_now(&query);

	switch_safe_free(query);
}

static profile_t *mod_apn_locate_profile(char *app_id, char *type)
{
    profile_t *profile = NULL;
    char *name = NULL;

    if (zstr(app_id) || zstr(type)) {
    	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Can't find profile '%s-%s'\n", app_id, type);
    	return NULL;
    }

    name = switch_mprintf("%s-%s", app_id, type);

    if (!(profile = switch_core_hash_find(globals.profile_hash, name))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "CARUSTO. Error invalid profile %s\n", name);
    }

    switch_safe_free(name);
    return profile;
}

static apn_ctx_t *mod_apn_init_context_with_data(profile_t *profile)
{
    apn_ctx_t *context = NULL;

    if (!profile) {
        return NULL;
    }

    if (NULL == (context = apn_init())) {
        return NULL;
    }

    if (!zstr(profile->path_p12)) {
    	apn_set_pkcs12_file(context, profile->path_p12, profile->password);
    } else if (!zstr(profile->path_cert) && !zstr(profile->path_key)) {
    	apn_set_certificate(context, profile->path_cert, profile->path_key, profile->password);
    } else {
    	apn_free(context);
    	return NULL;
    }

    apn_set_mode(context, profile->sandbox ? APN_MODE_SANDBOX : APN_MODE_PRODUCTION);
    if (globals.debug) {
    	apn_set_log_level(context, APN_LOG_LEVEL_INFO | APN_LOG_LEVEL_ERROR | APN_LOG_LEVEL_DEBUG);
    } else {
    	apn_set_log_level(context, APN_LOG_LEVEL_ERROR);
    }
    apn_set_log_callback(context, mod_apn_logging);
    apn_set_behavior(context, APN_OPTION_RECONNECT | APN_OPTION_LOG_STDERR);

    return context;
}

static apn_payload_t *init_payload_with_data(cJSON *payload_json)
{
    apn_payload_t *payload = NULL;
    time_t time_now = 0;
    const char *body = NULL, *sound = NULL, *action_key = NULL, *image = NULL, *category = NULL;
//    const char *localized_key = NULL;
    cJSON *barge_json = NULL, *content_available_json = NULL, *custom = NULL, *iterator = NULL;
    int size = 0, i;

    if (!payload_json) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No payload object\n");
        return NULL;
    }

    time(&time_now);

    if (NULL == (payload = apn_payload_init())) {
        return NULL;
    }

    apn_payload_set_priority(payload, APN_NOTIFICATION_PRIORITY_HIGH);

    barge_json = cJSON_GetObjectItem(payload_json, "barge");
    if (barge_json && barge_json->type == cJSON_Number && barge_json->valueint && barge_json->valueint > 0) {
        apn_payload_set_badge(payload, barge_json->valueint);
    }

    body = cJSON_GetObjectCstr(payload_json, "body");
    if (!zstr(body)) {
        apn_payload_set_body(payload, body);
    }

    sound = cJSON_GetObjectCstr(payload_json, "sound");
    if (!zstr(sound)) {
        apn_payload_set_sound(payload, sound);
    }

    content_available_json = cJSON_GetObjectItem(payload_json, "content_available");
    if (content_available_json) {
        if (content_available_json->type == cJSON_True) {
            apn_payload_set_content_available(payload, 1);
        } else if (content_available_json->type == cJSON_Number) {
            apn_payload_set_content_available(payload, content_available_json->valueint);
        }
    }

//    apn_payload_set_expiry(payload, time_now + 3600);

    action_key = cJSON_GetObjectCstr(payload_json, "action_key");
    if (!zstr(action_key)) {
    	apn_payload_set_localized_action_key(payload, action_key);
    }

    image = cJSON_GetObjectCstr(payload_json, "image");
    if (!zstr(image)) {
    	apn_payload_set_launch_image(payload, image);
    }

//    localized_key = cJSON_GetObjectCstr(payload_json, "localized_key");
//    if (!zstr(localized_key)) {
//    	apn_payload_set_localized_key(payload, localized_key, NULL);
//    }

    category = cJSON_GetObjectCstr(payload_json, "category");
    if (!zstr(category)) {
    	apn_payload_set_category(payload, category);
    }

    custom = cJSON_GetObjectItem(payload_json, "custom");
    if (custom && (size = cJSON_GetArraySize(custom)) > 0) {
        const char *name = NULL;
        cJSON *value = NULL;
        for (i = 0; i < size; i++) {
            if ((iterator = cJSON_GetArrayItem(custom, i)) == NULL) {
                break;
            }
            name = cJSON_GetObjectCstr(iterator, "name");
            value = cJSON_GetObjectItem(iterator, "value");
            if (!zstr(name) && value) {
                if (value->type == cJSON_False || value->type == cJSON_True) {
                    apn_payload_add_custom_property_bool(payload, name, value->type == cJSON_False ? 0 : 1);
                } else if (value->type == cJSON_NULL) {
                    apn_payload_add_custom_property_null(payload, name);
                } else if (value->type == cJSON_Number) {
                    if (value->valuedouble > 0 && (value->valuedouble / (int)value->valuedouble) > 0) {
                        apn_payload_add_custom_property_double(payload, name, value->valuedouble);
                    } else {
                        apn_payload_add_custom_property_integer(payload, name, value->valueint);
                    }
                } else if (value->type == cJSON_String) {
                    apn_payload_add_custom_property_string(payload, name, value->valuestring);
                } else if (value->type == cJSON_Array) {
//                    apn_payload_add_custom_property_array;
                }
            }
        }
    }

    return payload;
}

static apn_array_t *json_get_tokens_array(cJSON *tokens_json)
{
    apn_array_t *tokens = NULL;
    int size = 0, i;
    cJSON *iterator = NULL;
    char *token = NULL;

    if (!tokens_json) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "CARUSTO. No tokens JSON object\n");
        return NULL;
    }

    size = cJSON_GetArraySize(tokens_json);
    if (size <= 0) {
        return tokens;
    }

    tokens = apn_array_init(size, NULL, NULL);
    if (!tokens) {
       return NULL;
    }

    for (i = 0; i < size; i++) {
        if ((iterator = cJSON_GetArrayItem(tokens_json, i)) == NULL) {
            break;
        }
        if (iterator->type == cJSON_String && iterator->valuestring) {
            token = iterator->valuestring;
        }
        if (!zstr(token)) {
            apn_array_insert(tokens, token);
        }
    }

    return tokens;
}

static switch_bool_t mod_apn_send(profile_t *profile, apn_payload_t *payload, apn_array_t *tokens, int handle_invalid_tokens)
{
    apn_ctx_t *context = NULL;

    apn_array_t *invalid_tokens = NULL;
    switch_bool_t ret = SWITCH_FALSE;

    if (!profile) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. APN profile not found\n");
        return SWITCH_FALSE;
    }

    if (!payload) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. APN payload not set: %d\n", errno);
        goto end;
    }

    if (!tokens || apn_array_count(tokens) <= 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. APN tokens not set\n");
        goto end;
    }

    context = mod_apn_init_context_with_data(profile);
    if (!context) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Unable to init APN context: %d\n", errno);
        goto end;
    }
    if (handle_invalid_tokens) {
    	apn_set_invalid_token_callback(context, mod_apn_invalid_token);
    }

    if (APN_ERROR == apn_connect(context)) {
       switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Could not connected to Apple Push Notification Service: %s (errno: %d)\n", apn_error_string(errno), errno);
       goto end;
    }

    if (APN_ERROR == apn_send(context, payload, tokens, &invalid_tokens)) {
       switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Could not send APN: %s (errno: %d)\n", apn_error_string(errno), errno);
    } else {
       switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "CARUSTO. Apple Push Notification was successfully sent to %u device(s)\n",
              apn_array_count(tokens) - ((invalid_tokens) ? apn_array_count(invalid_tokens) : 0));
       if (invalid_tokens) {
           int i = 0;
           switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. APN Invalid tokens:\n");
           for (i = 0; i < apn_array_count(invalid_tokens); i++) {
               switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "\t%u. %s\n", i, (char *)apn_array_item_at_index(invalid_tokens, i));
           }
       }
       if ((apn_array_count(tokens) - ((invalid_tokens) ? apn_array_count(invalid_tokens) : 0)) > 0) {
    	   ret = SWITCH_TRUE;
       }
    }

end:
   if (context) {
       apn_free(context);
   }
   if (invalid_tokens) {
	   apn_array_free(invalid_tokens);
   }

   return ret;
}

static char *mod_apn_execute_sql2str(char *sql, char *resbuf, size_t len)
{
	char *ret = NULL;
	char *err = NULL;
	switch_cache_db_handle_t *dbh = NULL;

	switch_mutex_lock(globals.dbh_mutex);

	if (!(dbh = mod_apn_get_db_handle())) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Opening DB\n");

		if (globals.dbh_mutex) {
			switch_mutex_unlock(globals.dbh_mutex);
		}

		return NULL;
	}

	ret = switch_cache_db_execute_sql2str(dbh, sql, resbuf, len, &err);

	switch_mutex_unlock(globals.dbh_mutex);

	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SQL ERR: [%s]\n%s\n", err, sql);
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
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Opening DB\n");
		goto end;
	}

	switch_cache_db_execute_sql_callback(dbh, sql, callback, pdata, &errmsg);

	if (errmsg) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SQL ERR: [%s] %s\n", sql, errmsg);
		free(errmsg);
	}

end:
	if (dbh)
		switch_cache_db_release_db_handle(&dbh);

	return ret;
}

static int sql2str_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	callback_t *cbt = (callback_t *) pArg;

	if (argc > 0 && !zstr(argv[0])) {
		cJSON_AddItemToArray(cbt->root, cJSON_CreateString(argv[0]));
	}
	return 0;
}

#define APN_USAGE "{\"app_id\":\"\",\"type\":\"[im|voip]\",\"payload\":{\"body\":\"\",\"sound\":\"\",\"\":[{\"name\":\"\",\"value\":\"\"},\"image\":\"\",\"category\":\"\"},\"tokens\":[\"\"]}"
SWITCH_STANDARD_API(apn_api_function)
{
    profile_t *profile = NULL;
    char *pdata = NULL;
    const char *profile_name = NULL, *type = NULL;
    cJSON *root = NULL;
    apn_payload_t *payload = NULL;
    apn_array_t *tokens = NULL;
    switch_bool_t res = SWITCH_TRUE;

    if (cmd) {
    	pdata = strdup(cmd);
    }

    if (!pdata) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No data\n");
        stream->write_function(stream, "USAGE: %s", APN_USAGE);
        return SWITCH_STATUS_FALSE;
    }

    root = cJSON_Parse(pdata);
    if (!root) {
    	stream->write_function(stream, "Wrong JSON data. USAGE: %s", APN_USAGE);
        goto end;
    }

    profile_name = cJSON_GetObjectCstr(root, "app_id");
    type = cJSON_GetObjectCstr(root, "type");

    if (zstr(profile_name) || zstr(type)) {
        stream->write_function(stream, "app_id/type not set. USAGE: %s", APN_USAGE);
        goto end;
    }
    if (!(profile = mod_apn_locate_profile((char *)profile_name, (char *)type))) {
    	stream->write_function(stream, "Profile '%s-%s' not found.", profile_name, type);
        goto end;
    }

    payload = init_payload_with_data(cJSON_GetObjectItem(root, "payload"));
    tokens = json_get_tokens_array(cJSON_GetObjectItem(root, "tokens"));

    res = mod_apn_send(profile, payload, tokens, SWITCH_FALSE);

	if (res != SWITCH_TRUE) {
		stream->write_function(stream, "Not sent");
	} else {
		stream->write_function(stream, "Sent");
	}

end:
    if (root) {
        cJSON_Delete(root);
    }

    if (payload) {
        apn_payload_free(payload);
    }
    if (tokens) {
        apn_array_free(tokens);
    }

    free(pdata);
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_config(switch_memory_pool_t *pool)
{
	char *cf = "apn.conf";
	switch_xml_t cfg, xml, settings, param, x_profile, x_profiles;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	profile_t *profile = NULL;
	switch_cache_db_handle_t *dbh = NULL;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "open of %s failed\n", cf);
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
			if (!strcasecmp(var, "debug") && !zstr(val)) {
			    globals.debug = switch_true(val);
			} else if (!strcasecmp(var, "odbc_dsn") && !zstr(val)) {
				globals.odbc_dsn = switch_core_strdup(globals.pool, val);
			} else if (!strcasecmp(var, "voip_app") && !zstr(val)) {
				globals.voip_app = switch_core_strdup(globals.pool, val);
			}
		}
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

	if (zstr(globals.voip_app)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "voip_app (VoIP application name) parameter not set\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

    switch_core_hash_init(&globals.profile_hash);
    if ((x_profiles = switch_xml_child(cfg, "profiles"))) {
        for (x_profile = switch_xml_child(x_profiles, "profile"); x_profile; x_profile = x_profile->next) {
            char *name = (char *) switch_xml_attr_soft(x_profile, "name");
            switch_bool_t sandbox = SWITCH_TRUE;
            char *id_s = NULL, *password = NULL, *path_p12 = NULL, *path_cert = NULL, *path_key = NULL;

            for (param = switch_xml_child(x_profile, "param"); param; param = param->next) {
                char *var, *val;
                var = (char *) switch_xml_attr_soft(param, "name");
                val = (char *) switch_xml_attr_soft(param, "value");

                if (!strcasecmp(var, "id") && !zstr(val)) {
                    id_s = val;
                } else if (!strcasecmp(var, "sandbox") && !zstr(val)) {
                    sandbox = switch_true(val);
                } else if (!strcasecmp(var, "p12") && !zstr(val)) {
                    path_p12 = val;
                } else if (!strcasecmp(var, "password") && !zstr(val)) {
                    password = val;
                } else if (!strcasecmp(var, "cert") && !zstr(val)) {
                    path_cert = val;
                } else if (!strcasecmp(var, "key") && !zstr(val)) {
                    path_key = val;
                }
            }

            if (zstr(path_p12) && (zstr(path_cert) || zstr(path_key))) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No path to certificate specified.\n");
                goto done;
            } else if (zstr(password)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No password specified.\n");
                goto done;
            }

            if (zstr(name)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No name specified.\n");
            } else {
                profile = switch_core_alloc(globals.pool, sizeof(*profile));
                memset(profile, 0, sizeof(profile_t));
                profile->name = switch_core_strdup(globals.pool, name);

                if (!zstr(id_s)) {
                    profile->id = (uint16_t)atoi(id_s);
                }
                profile->path_p12 = !zstr(path_p12) ? switch_core_strdup(globals.pool, path_p12) : NULL;
                profile->path_cert = !zstr(path_cert) ? switch_core_strdup(globals.pool, path_cert) : NULL;
                profile->path_key = !zstr(path_key) ? switch_core_strdup(globals.pool, path_key) : NULL;
                profile->password = switch_core_strdup(globals.pool, password);
                profile->sandbox = sandbox;

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

static apn_array_t *db_get_tokens_array(char *user, char *realm, char *app_id, char *type, callback_t *cbt)
{
	char *query = NULL;
	apn_array_t *tokens = NULL;
	int size = 0, i;
	cJSON *iterator = NULL;

	if (zstr(user) || zstr(realm) || zstr(app_id)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No parameters for get token. user: '%s', realm: '%s', app_id: '%s'\n", user, realm, app_id);
		goto end;
	}

	query = switch_mprintf("SELECT token FROM apple_tokens WHERE extension = '%q' AND realm = '%q' AND app_id = '%q' AND type = '%q'", user, realm, app_id, type);
	mod_apn_execute_sql_callback(query, sql2str_callback, cbt);

	size = cJSON_GetArraySize(cbt->root);
	if (size <= 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. User: %s@%s don't have any tokens for app_id: '%s'\n", user, realm, app_id);
		goto end;
	}

    if (!(tokens = apn_array_init(size, NULL, NULL))) {
    	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "CARUSTO. Can't init token structure with size: %d\n", size);
       goto end;
    }

	for (i = 0; i < size; i++) {
		if ((iterator = cJSON_GetArrayItem(cbt->root, i)) == NULL) {
			break;
		}
		if (iterator && iterator->type == cJSON_String && iterator->valuestring) {
			apn_array_insert(tokens, iterator->valuestring);
		}
	}

end:
	switch_safe_free(query);

	return tokens;
}

static void push_event_handler(switch_event_t *event)
{
	char *data = NULL, *app_id = NULL, *user = NULL, *realm = NULL, *type = NULL, *uuid = NULL;
	profile_t *profile = NULL;
    apn_payload_t *payload = NULL;
    apn_array_t *tokens = NULL;
    cJSON *root = NULL;
    callback_t cbt = { cJSON_CreateArray() };
    switch_bool_t res = SWITCH_FALSE;

	data = switch_event_get_body(event);
	type = switch_event_get_header(event, "type");
	app_id = switch_event_get_header(event, "app_id");
	user = switch_event_get_header(event, "user");
	realm = switch_event_get_header(event, "realm");
	uuid = switch_event_get_header(event, "uuid");

	if (zstr(data) || zstr(type) || zstr(app_id) || zstr(user) || zstr(realm)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No parameters, data: '%s', type: '%s', app_id: '%s', user: '%s', realm: '%s'\n", data, type, app_id, user, realm);
		goto end;
	}

	root = cJSON_Parse(data);
	if (!root) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "JSON (%s) not parsed\n", data);
		goto end;
	}

	if (!(profile = mod_apn_locate_profile(app_id, type))) {
		goto end;
	}

	payload = init_payload_with_data(root);
    tokens = db_get_tokens_array(user, realm, app_id, type, &cbt);
    if (!tokens) {
        goto end;
    }

    res = mod_apn_send(profile, payload, tokens, SWITCH_TRUE);

end:
	if (!zstr(uuid) && switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "apple::push::response") == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "uuid", uuid);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "response", res ? "sent" : "notsent");
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Fire event apple::push::response with ID: '%s' and result: '%s'\n", uuid, res ? "sent" : "notsent");
		switch_event_fire(&event);
		switch_event_destroy(&event);
	}

   if (payload) {
	   apn_payload_free(payload);
   }
   if (tokens) {
	   apn_array_free(tokens);
   }
   if (root) {
       cJSON_Delete(root);
   }
	if (cbt.root) {
		cJSON_Delete(cbt.root);
	}

	return;
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

    destination = switch_mprintf("[registration_token=%s,originate_timeout=%u]sofia/%s/%s:_:[originate_timeout=%u,enable_send_apn=false,apn_wait_any_register=%d]apn_wait/%s@%s",
    		event_call_id,
    		*originate_data->timelimit,
    		event_profile,
    		dest,
    		*originate_data->timelimit,
    		originate_data->wait_any_register == SWITCH_TRUE ? 1 : 0,
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
    char *contact_ptr = NULL, *voip_token = NULL, *im_token = NULL, *foo = NULL, *query = NULL, *app_id = NULL;
    char *update_reg = NULL;

	update_reg = switch_event_get_header(event, "update-reg");
	if (!zstr(update_reg) && switch_true(update_reg)) {
		return;
	}

	event_contact = switch_event_get_header(event, "contact");
    if (zstr(event_contact)) {
    	return;
    }

    contact_ptr = app_id = voip_token = im_token = strdup(event_contact);

    if (zstr(contact_ptr)) {
    	goto end;
    }

    /*Get contact parameter pn-voip-tok */
    if ((foo = strcasestr(voip_token, "pn-voip-tok="))) {
        voip_token = foo + 12;
    } else {
    	voip_token = NULL;
    }

    /*Get contact parameter pn-im-tok */
    if ((foo = strcasestr(im_token, "pn-im-tok="))) {
    	im_token = foo + 10;
    } else {
    	im_token = NULL;
    }

    if (zstr(voip_token) && zstr(im_token)) {
    	goto end;
    }

    /*Get contact parameter app-id */
    if ((foo = strcasestr(app_id, "app-id=")) == NULL) {
		goto end;
	}

	app_id = foo + 7;
	if (zstr(app_id)) {
		goto end;
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

    event_user = switch_event_get_header(event, "from-user");
    event_realm = switch_event_get_header(event, "realm");

    if (zstr(event_user) || zstr(event_realm)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. No parameter\n");
        goto end;
    }

    /*Check existing VoIP token*/
    if (!zstr(voip_token)) {
		char voip_count[2] = {0, };
		query = switch_mprintf("SELECT count(*) FROM apple_tokens WHERE token = '%q' AND extension = '%q' AND realm = '%q' AND app_id = '%q' AND type = 'voip'", voip_token, event_user, event_realm, app_id);
		mod_apn_execute_sql2str(query, voip_count, sizeof(voip_count));
		switch_safe_free(query);

		if (!zstr(voip_count) && atoi(voip_count) <= 0) {
			/*Add new VoIP token to DB*/
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Add new VoIP token: '%s' to apple_tokens for user %s@%s and application: %s\n", voip_token, event_user, event_realm, app_id);
			query = switch_mprintf("INSERT INTO apple_tokens (token, extension, realm, app_id, type) VALUES ('%q', '%q', '%q', '%q', 'voip')", voip_token, event_user, event_realm, app_id);
			execute_sql_now(&query);
			switch_safe_free(query);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. VoIP token: '%s' for user %s@%s and application: %s already exists\n", voip_token, event_user, event_realm, app_id);
		}
    }

    /*Check existing IM token*/
    if (!zstr(im_token)) {
		char im_count[2] = {0, };
		query = switch_mprintf("SELECT count(*) FROM apple_tokens WHERE token = '%q' AND extension = '%q' AND realm = '%q' AND app_id = '%q' AND type = 'im'", im_token, event_user, event_realm, app_id);
		mod_apn_execute_sql2str(query, im_count, sizeof(im_count));
		switch_safe_free(query);

		if (!zstr(im_count) && atoi(im_count) <= 0) {
			/*Add new IM token to DB*/
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Add new IM token: '%s' to apple_tokens for user %s@%s and application: %s\n", im_token, event_user, event_realm, app_id);
			query = switch_mprintf("INSERT INTO apple_tokens (token, extension, realm, app_id, type) VALUES ('%q', '%q', '%q', '%q', 'im')", im_token, event_user, event_realm, app_id);
			execute_sql_now(&query);
			switch_safe_free(query);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. IM token: '%s' for user %s@%s and application: %s already exists\n", im_token, event_user, event_realm, app_id);
		}
    }

end:
	switch_safe_free(contact_ptr);
}

static int init_sql(void)
{
	char sql[] =
		"CREATE TABLE apple_tokens ("
		"id				serial NOT NULL,"
		"token			VARCHAR(255) NOT NULL,"
		"extension		VARCHAR(255) NOT NULL,"
		"realm			VARCHAR(255) NOT NULL,"
		"app_id			VARCHAR(255) NOT NULL,"
		"type			VARCHAR(255) NOT NULL,"
		"CONSTRAINT apple_tokens_pkey PRIMARY KEY (id)"
		");";

	switch_cache_db_handle_t *dbh = mod_apn_get_db_handle();

	if (!dbh) {
		return 0;
	}

	switch_cache_db_test_reactive(dbh, "SELECT count(*) FROM apple_tokens", NULL, sql);
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

	return;
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
	char *user = NULL, *domain = NULL, *dup_domain = NULL;
	char *var_val = NULL;
	switch_event_t *event = NULL;
	static switch_event_node_t *response_event = NULL, *register_event = NULL;
	switch_channel_t *channel = NULL;
	switch_memory_pool_t *pool = NULL;
	char *cid_name_override = NULL;
	char *cid_num_override = NULL;
	originate_register_t originate_data = { 0, };
	char *destination = NULL;
	switch_bool_t wait_any_register = SWITCH_FALSE;
	response_t apn_response = { {0, }, MOD_APN_UNDEFINE, NULL };

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
			int tmp = atoi(var_val);
			if (tmp > 0) {
				timelimit_sec = (uint32_t) tmp;
			}
		}
	}

	if (timelimit_sec <= 0) {
		timelimit_sec = 60;
	}

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

	originate_data.timelimit = &timelimit_sec;

	/*Bind to event 'sofia::register' for originate call to registration*/
	switch_event_bind_removable("apn_originate_register", SWITCH_EVENT_CUSTOM, "sofia::register", originate_register_event_handler, &originate_data, &register_event);

	if (wait_any_register == SWITCH_TRUE) {
		if ((switch_event_bind_removable(modname, SWITCH_EVENT_CUSTOM, "apple::push::response", response_event_handler, &apn_response, &response_event) != SWITCH_STATUS_SUCCESS)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind event!\n");
			goto done;
		}
	}

	/*Create event 'apple::push::notification' for send push notification*/
    if (!var_event || (var_event && !switch_true(switch_event_get_header(var_event, "enable_send_apn")))) {
    	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "apple::push::notification") == SWITCH_STATUS_SUCCESS) {
    		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "uuid", apn_response.uuid);
    		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "type", "voip");
    		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "app_id", globals.voip_app);
    		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "user", user);
    		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "realm", domain);
    		switch_event_add_body(event, "{\"body\":\"WakeUP\"}");
    		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Fire event APN for User: %s@%s\n", user, domain);
    		switch_event_fire(&event);
    		switch_event_destroy(&event);
		}
    }

	while (timelimit_sec-- > 0) {
		if (wait_any_register != SWITCH_TRUE) {
			switch_mutex_lock(apn_response.mutex);
			if (apn_response.state == MOD_APN_NOTSENT) {
				switch_mutex_unlock(apn_response.mutex);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CARUSTO. Event APN don't sent to %s@%s, so stop wait for incoming register\n", user, domain);
				break;
			}
			switch_mutex_unlock(apn_response.mutex);
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
			if (switch_ivr_originate(session, new_session, &cause, destination, timelimit_sec, NULL,
											cid_name_override, cid_num_override, outbound_profile, var_event, flags,
											cancel_cause) == SWITCH_STATUS_SUCCESS) {
				switch_core_session_rwunlock(*new_session);
			}
			break;
		}

		if (channel && !switch_channel_ready(channel)) {
			break;
		}

		if (*cancel_cause && *cancel_cause > 0) {
			break;
		}
		/*Sleep 1sec*/
		switch_yield(1000000);
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

	if (apn_library_init() != APN_SUCCESS) {
		return status;
	}
	globals.init_lib = 1;

	if (!init_sql()) {
		goto error;
	}

	/*Bind to event sofia::register for add new tokens from contact parameters*/
	if ((switch_event_bind_removable(modname, SWITCH_EVENT_CUSTOM, "sofia::register", register_event_handler, NULL, &register_event) != SWITCH_STATUS_SUCCESS)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind event!\n");
		goto error;
	}

	/*Bind to event apple::push::notification for send ApplePushNotification to iOS applications*/
	if ((switch_event_bind_removable(modname, SWITCH_EVENT_CUSTOM, "apple::push::notification", push_event_handler, NULL, &push_event) != SWITCH_STATUS_SUCCESS)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind event!\n");
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
	if (globals.init_lib) {
		apn_library_free();
		globals.init_lib = 0;
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
	if (globals.init_lib) {
		apn_library_free();
		globals.init_lib = 0;
	}
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
