/*
 * https endpoints for logging into LastPass
 *
 * Copyright (C) 2014-2018 LastPass.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 *
 * See LICENSE.OpenSSL for more details regarding this exception.
 */
#include "endpoints.h"
#include "http.h"
#include "xml.h"
#include "password.h"
#include "config.h"
#include "util.h"
#include "upload-queue.h"
#include "version.h"
#include "terminal.h"
#include <sys/utsname.h>
#include <string.h>

struct multifactor_type {
    const char *name;
    const char *error_str;
    const char *error_failure_str;
    const char *post_var;
};
static struct multifactor_type multifactor_types[] = {
	{
		.name = "Google Authenticator Code",
		.error_str = "googleauthrequired",
		.error_failure_str = "googleauthfailed",
		.post_var = "otp"
	},
	{
		.name = "YubiKey OTP",
		.error_str = "otprequired",
		.error_failure_str = "otpfailed",
		.post_var = "otp"
	},
	{
		.name = "Sesame OTP",
		.error_str = "sesameotprequired",
		.error_failure_str = "sesameotpfailed",
		.post_var = "sesameotp"
	},
	{
		.name = "Out-of-Band OTP",
		.error_str = "outofbandrequired",
		.error_failure_str = "multifactorresponsefailed",
		.post_var = "otp"
	},
    {
        .name = "Microsoft Authenticator Code",
        .error_str = "microsoftauthrequired",
        .error_failure_str = "microsoftauthfailed",
        .post_var = "otp"
    }
};

static void filter_error_message(char *message)
{
	char *nullit;

	nullit = strstr(message, " Upgrade your browser extension so you can enter it.");
	if (nullit)
		*nullit = '\0';
}

static inline void append_post(char **args, const char *name, const char *val)
{
	char **last = args;
	while (*last && strcmp(*last, name))
		++last;
	*last = (char *)name;
	*(last + 1) = (char *)val;
}

static char *calculate_trust_id(bool force)
{
	char *trusted_id;

	trusted_id = config_read_string("trusted_id");
	if (force && !trusted_id) {
		trusted_id = xcalloc(33, 1);
		for (size_t i = 0; i < 32; ++i)
			trusted_id[i] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890!@#$"[range_rand(0, 66)];
		config_write_string("trusted_id", trusted_id);
	}

	return trusted_id;
}

static char *calculate_trust_label(void)
{
	char *trusted_label;
	struct utsname utsname;

	if (uname(&utsname) < 0)
		die_errno("Failed to determine uname.");

	xasprintf(&trusted_label, "%s - %s %s", utsname.nodename, utsname.sysname, utsname.release);

	return trusted_label;
}

static bool error_post(char **message, struct session **session)
{
	*session = NULL;
	if (message)
		*message = xstrdup("Unable to post login request.");
	return true;
}


static bool error_other(char **message, struct session **session, const char *txt)
{
	*session = NULL;
	if (message)
		*message = xstrdup(txt);
	return true;
}

static bool error_message(char **message, struct session **session, const char *reply)
{
	*session = NULL;
	if (message) {
		*message = xml_error_cause(reply, "message");
		if (*message)
			filter_error_message(*message);
		else
			*message = xstrdup("Could not parse error message to login request.");

	}
	return true;
}

static bool ordinary_login(const char *login_server, const unsigned char key[KDF_HASH_LEN], char **args, char **cause, char **message, char **reply, struct session **session,
			   char **ret_login_server)
{
	char *server;

	free(*reply);
	*reply = http_post_lastpass_v(login_server, "login.php", NULL, NULL, args);
	if (!*reply)
		return error_post(message, session);

	*session = xml_ok_session(*reply, key);
	if (*session) {
		(*session)->server = xstrdup(login_server);
		return true;
	}

	/* handle server redirection if requested for lastpass.eu */
	server = xml_error_cause(*reply, "server");
	if (server && strcmp(server, "lastpass.eu") == 0)
		return ordinary_login(server, key, args, cause, message, reply, session, ret_login_server);

	*cause = xml_error_cause(*reply, "cause");
	if (!*cause)
		return error_other(message, session, "Unable to determine login failure cause.");

	*ret_login_server = xstrdup(login_server);
	return false;
}

static inline bool has_capabilities(const char *capabilities, const char *capability)
{
	_cleanup_free_ char *caps = xstrdup(capabilities);
	char *token;

	for (token = strtok(caps, ","); token; token = strtok(NULL, ",")) {
		if (!strcmp(capability, token))
			return true;
	}

	return false;
}

static bool oob_login(const char *login_server, const unsigned char key[KDF_HASH_LEN], char **args, char **message, char **reply, char **oob_name, struct session **session)
{
	_cleanup_free_ char *oob_capabilities = NULL;
	_cleanup_free_ char *cause = NULL;
	_cleanup_free_ char *retryid = NULL;
	bool can_do_passcode;
	bool ret;

	*oob_name = xml_error_cause(*reply, "outofbandname");
	oob_capabilities = xml_error_cause(*reply, "capabilities");
	if (!*oob_name || !oob_capabilities)
		return error_other(message, session, "Could not determine out-of-band type.");
	can_do_passcode = has_capabilities(oob_capabilities, "passcode");
	if (can_do_passcode && !has_capabilities(oob_capabilities, "outofband")) {
		xstrappend(oob_name, " OTP");
		goto failure;
	}

	terminal_fprintf(stderr, TERMINAL_FG_YELLOW TERMINAL_BOLD "Waiting for approval of out-of-band %s login%s" TERMINAL_NO_BOLD "...", *oob_name, can_do_passcode ? ", or press Ctrl+C to enter a passcode" : "");
	append_post(args, "outofbandrequest", "1");
	for (;;) {
		free(*reply);
		*reply = http_post_lastpass_v(login_server, "login.php", NULL, NULL, args);
		if (!*reply) {
			if (can_do_passcode) {
				append_post(args, "outofbandrequest", "0");
				append_post(args, "outofbandretry", "0");
				append_post(args, "outofbandretryid", "");
				xstrappend(oob_name, " OTP");
				goto failure;
			} else {
				error_post(message, session);
				goto success;
			}
		}
		*session = xml_ok_session(*reply, key);
		if (*session) {
			(*session)->server = xstrdup(login_server);
			goto success;
		}

		free(cause);
		cause = xml_error_cause(*reply, "cause");
		if (cause && !strcmp(cause, "outofbandrequired")) {
			free(retryid);
			retryid = xml_error_cause(*reply, "retryid");
			append_post(args, "outofbandretry", "1");
			append_post(args, "outofbandretryid", retryid);
			fprintf(stderr, ".");
			continue;
		}
		error_message(message, session, *reply);
		goto success;
	}

success:
	ret = true;
	goto out;
failure:
	ret = false;
	goto out;
out:
	terminal_fprintf(stderr, TERMINAL_RESET "\n" TERMINAL_UP_CURSOR(1) TERMINAL_CLEAR_DOWN);
	return ret;
}

static bool otp_login(const char *login_server, const unsigned char key[KDF_HASH_LEN], char **args, char **message, char **reply, const char *otp_name, const char *cause, const char *username, struct session **session)
{
	struct multifactor_type *replied_multifactor = NULL;
	_cleanup_free_ char *multifactor = NULL;
	_cleanup_free_ char *next_cause = NULL;
	char *multifactor_error = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(multifactor_types); ++i) {
		if (!strcmp(multifactor_types[i].error_str, cause)) {
			replied_multifactor = &multifactor_types[i];
			break;
		}
	}
	if (!replied_multifactor)
		return error_message(message, session, *reply);

	for (;;) {
		free(multifactor);
		multifactor = password_prompt("Code", multifactor_error, "Please enter your %s for <%s>.", otp_name ? otp_name : replied_multifactor->name, username);
		if (!multifactor)
			return error_other(message, session, "Aborted multifactor authentication.");
		append_post(args, replied_multifactor->post_var, multifactor);

		free(*reply);
		*reply = http_post_lastpass_v(login_server, "login.php", NULL, NULL, args);
		if (!*reply)
			return error_post(message, session);

		*session = xml_ok_session(*reply, key);
		if (*session) {
			(*session)->server = xstrdup(login_server);
			return true;
		}

		free(next_cause);
		next_cause = xml_error_cause(*reply, "cause");
		if (next_cause && !strcmp(next_cause, replied_multifactor->error_failure_str))
			multifactor_error = "Invalid multifactor code; please try again.";
		else
			return error_message(message, session, *reply);
	}
}

struct session *lastpass_login(const char *username, const char *fragment, const char hash[KDF_HEX_LEN], const unsigned char key[KDF_HASH_LEN], int iterations, char **error_message, bool trust)
{
	char *args[33];
	_cleanup_free_ char *user_lower = NULL;
	_cleanup_free_ char *iters = NULL;
	_cleanup_free_ char *trusted_id = NULL;
	_cleanup_free_ char *trusted_label = NULL;
	_cleanup_free_ char *cause = NULL;
	_cleanup_free_ char *reply = NULL;
	_cleanup_free_ char *otp_name = NULL;
	_cleanup_free_ char *login_server = NULL;
	struct session *session = NULL;

	iters = xultostr(iterations);
	user_lower = xstrlower(username);
	trusted_id = calculate_trust_id(trust);

	memset(args, 0, sizeof(args));
	append_post(args, "xml", "2");
	append_post(args, "username", user_lower);
	append_post(args, "hash", hash);
	append_post(args, "iterations", iters);
	append_post(args, "includeprivatekeyenc", "1");
	append_post(args, "method", "cli");
	append_post(args, "outofbandsupported", "1");
  if (fragment) {
    append_post(args, "alpfragmentid", fragment);
    append_post(args, "calculatedfragmentid", fragment);
  }
	if (trusted_id)
		append_post(args, "uuid", trusted_id);

	if (ordinary_login(LASTPASS_SERVER, key, args, &cause, error_message, &reply, &session, &login_server))
		return session;

	if (trust) {
		trusted_label = calculate_trust_label();
		append_post(args, "trustlabel", trusted_label);
	}

	if (cause && !strcmp(cause, "outofbandrequired") &&
	    oob_login(login_server, key, args, error_message, &reply, &otp_name, &session)) {
		if (trust)
			http_post_lastpass("trust.php", session, NULL, "token", session->token, "uuid", trusted_id, "trustlabel", trusted_label, NULL);
		return session;
	}

	if (otp_login(login_server, key, args, error_message, &reply, otp_name, cause, user_lower, &session))
		return session;

	error_other(error_message, &session, "An unspecified error occurred.");
	return NULL;
}
