/* Copyright (C) 2004 Timo Sirainen, Alex Howansky */

#include "config.h"
#undef HAVE_CONFIG_H

#ifdef PASSDB_SQL

#include "common.h"
#include "str.h"
#include "strescape.h"
#include "var-expand.h"
#include "password-scheme.h"
#include "db-sql.h"
#include "passdb.h"

#include <stdlib.h>
#include <string.h>

struct passdb_sql_request {
	struct auth_request *auth_request;
	enum passdb_credentials credentials;
	union {
		verify_plain_callback_t *verify_plain;
                lookup_credentials_callback_t *lookup_credentials;
	} callback;

	char password[1];
};

static struct sql_connection *passdb_sql_conn;

static void sql_query_callback(struct sql_result *result, void *context)
{
	struct passdb_sql_request *sql_request = context;
	struct auth_request *auth_request = sql_request->auth_request;
	const char *user, *password, *scheme;
	int ret;

	user = auth_request->user;
	password = NULL;

	ret = sql_result_next_row(result);
	if (ret < 0) {
		i_error("sql(%s): Password query failed: %s",
			get_log_prefix(auth_request),
			sql_result_get_error(result));
	} else if (ret == 0) {
		if (verbose) {
			i_info("sql(%s): Unknown user",
			       get_log_prefix(auth_request));
		}
	} else {
		password = sql_result_find_field_value(result, "password");
		if (password != NULL)
			password = t_strdup(password);
		else {
			i_error("sql(%s): Password query didn't return "
				"password, or it was NULL",
				get_log_prefix(auth_request));
		}

		/* make sure there was only one row returned */
		if (sql_result_next_row(result) > 0) {
			i_error("sql(%s): Password query returned multiple "
				"matches", get_log_prefix(auth_request));
			password = NULL;
		}
	}

	scheme = password_get_scheme(&password);
	if (scheme == NULL) {
		scheme = passdb_sql_conn->set.default_pass_scheme;
		i_assert(scheme != NULL);
	}

	if (sql_request->credentials != -1) {
		passdb_handle_credentials(sql_request->credentials,
			user, password, scheme,
			sql_request->callback.lookup_credentials,
			auth_request);
		return;
	}

	/* verify plain */
	if (password == NULL) {
		sql_request->callback.verify_plain(PASSDB_RESULT_USER_UNKNOWN,
						   auth_request);
		return;
	}

	ret = password_verify(sql_request->password, password, scheme, user);
	if (ret < 0) {
		i_error("sql(%s): Unknown password scheme %s",
			get_log_prefix(auth_request), scheme);
	} else if (ret == 0) {
		if (verbose) {
			i_info("sql(%s): Password mismatch",
			       get_log_prefix(auth_request));
		}
	}

	sql_request->callback.verify_plain(ret > 0 ? PASSDB_RESULT_OK :
					     PASSDB_RESULT_PASSWORD_MISMATCH,
					     auth_request);
}

static void sql_lookup_pass(struct passdb_sql_request *sql_request)
{
	string_t *query;

	query = t_str_new(512);
	var_expand(query, passdb_sql_conn->set.password_query,
		   auth_request_get_var_expand_table(sql_request->auth_request,
						     str_escape));

	sql_query(passdb_sql_conn->db, str_c(query),
		  sql_query_callback, sql_request);
}

static void sql_verify_plain(struct auth_request *request, const char *password,
			     verify_plain_callback_t *callback)
{
	struct passdb_sql_request *sql_request;

	sql_request = i_malloc(sizeof(struct passdb_sql_request) +
			       strlen(password));
	sql_request->auth_request = request;
	sql_request->credentials = -1;
	sql_request->callback.verify_plain = callback;
	strcpy(sql_request->password, password);

	sql_lookup_pass(sql_request);
}

static void sql_lookup_credentials(struct auth_request *request,
				   enum passdb_credentials credentials,
				   lookup_credentials_callback_t *callback)
{
	struct passdb_sql_request *sql_request;

	sql_request = i_new(struct passdb_sql_request, 1);
	sql_request->auth_request = request;
	sql_request->credentials = credentials;
	sql_request->callback.lookup_credentials = callback;

        sql_lookup_pass(sql_request);
}

static void passdb_sql_preinit(const char *args)
{
	passdb_sql_conn = db_sql_init(args);
}

static void passdb_sql_init(const char *args __attr_unused__)
{
	db_sql_connect(passdb_sql_conn);
}

static void passdb_sql_deinit(void)
{
	db_sql_unref(passdb_sql_conn);
}

struct passdb_module passdb_sql = {
	passdb_sql_preinit,
	passdb_sql_init,
	passdb_sql_deinit,

	sql_verify_plain,
	sql_lookup_credentials
};

#endif
