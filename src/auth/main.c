/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "ioloop.h"
#include "network.h"
#include "lib-signals.h"
#include "restrict-access.h"
#include "fd-close-on-exec.h"
#include "randgen.h"
#include "password-scheme.h"
#include "mech.h"
#include "auth.h"
#include "auth-request-handler.h"
#include "auth-worker-server.h"
#include "auth-worker-client.h"
#include "auth-master-interface.h"
#include "auth-master-listener.h"
#include "auth-master-connection.h"
#include "auth-client-connection.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>

struct ioloop *ioloop;
int standalone = FALSE, worker = FALSE;
time_t process_start_time;

static struct auth *auth;
static struct auth_worker_client *worker_client;

static void sig_die(int signo, void *context __attr_unused__)
{
	/* warn about being killed because of some signal, except SIGINT (^C)
	   which is too common at least while testing :) */
	if (signo != SIGINT)
		i_warning("Killed with signal %d", signo);
	io_loop_stop(ioloop);
}

static void open_logfile(void)
{
	if (getenv("LOG_TO_MASTER") != NULL) {
		i_set_failure_internal();
		return;
	}

	if (getenv("USE_SYSLOG") != NULL)
		i_set_failure_syslog("dovecot-auth", LOG_NDELAY, LOG_MAIL);
	else {
		/* log to file or stderr */
		i_set_failure_file(getenv("LOGFILE"), "dovecot-auth");
	}

	if (getenv("INFOLOGFILE") != NULL)
		i_set_info_file(getenv("INFOLOGFILE"));

	i_set_failure_timestamp_format(getenv("LOGSTAMP"));
}

static uid_t get_uid(const char *user)
{
	struct passwd *pw;

	if (user == NULL)
		return (uid_t)-1;

	if ((pw = getpwnam(user)) == NULL)
		i_fatal("User doesn't exist: %s", user);
	return pw->pw_uid;
}

static gid_t get_gid(const char *group)
{
	struct group *gr;

	if (group == NULL)
		return (gid_t)-1;

	if ((gr = getgrnam(group)) == NULL)
		i_fatal("Group doesn't exist: %s", group);
	return gr->gr_gid;
}

static int create_unix_listener(const char *env, int backlog)
{
	const char *path, *mode, *user, *group;
	mode_t old_umask;
	unsigned int mask;
	uid_t uid;
	gid_t gid;
	int fd, i;

	path = getenv(env);
	if (path == NULL)
		return -1;

	mode = getenv(t_strdup_printf("%s_MODE", env));
	if (mode == NULL)
		mask = 0177; /* default to 0600 */
	else {
		if (sscanf(mode, "%o", &mask) != 1)
			i_fatal("%s: Invalid mode %s", env, mode);
		mask = (mask ^ 0777) & 0777;
	}

	old_umask = umask(mask);
	for (i = 0; i < 5; i++) {
		fd = net_listen_unix(path, backlog);
		if (fd != -1)
			break;

		if (errno != EADDRINUSE)
			i_fatal("net_listen_unix(%s) failed: %m", path);

		/* see if it really exists */
		if (net_connect_unix(path) != -1 || errno != ECONNREFUSED)
			i_fatal("Socket already exists: %s", path);

		/* delete and try again */
		if (unlink(path) < 0)
			i_fatal("unlink(%s) failed: %m", path);
	}
	umask(old_umask);

	user = getenv(t_strdup_printf("%s_USER", env));
	group = getenv(t_strdup_printf("%s_GROUP", env));

	uid = get_uid(user); gid = get_gid(group);
	if (chown(path, uid, gid) < 0) {
		i_fatal("chown(%s, %s, %s) failed: %m",
			path, dec2str(uid), dec2str(gid));
	}

	return fd;
}

static void add_extra_listeners(void)
{
	struct auth_master_listener *listener;
	const char *str, *client_path, *master_path;
	int client_fd, master_fd;
	unsigned int i;

	for (i = 1;; i++) {
		t_push();
		client_path = getenv(t_strdup_printf("AUTH_%u", i));
		master_path = getenv(t_strdup_printf("AUTH_%u_MASTER", i));
		if (client_path == NULL && master_path == NULL) {
			t_pop();
			break;
		}

		str = t_strdup_printf("AUTH_%u", i);
		client_fd = create_unix_listener(str, 64);
		str = t_strdup_printf("AUTH_%u_MASTER", i);
		master_fd = create_unix_listener(str, 64);

		listener = auth_master_listener_create(auth);
		if (master_fd != -1) {
			auth_master_listener_add(listener, master_fd,
						 master_path, LISTENER_MASTER);
		}
		if (client_fd != -1) {
			auth_master_listener_add(listener, client_fd,
						 client_path, LISTENER_CLIENT);
		}
		t_pop();
	}
}

static void drop_privileges(void)
{
	open_logfile();

	/* Open /dev/urandom before chrooting */
	random_init();

	/* Initialize databases so their configuration files can be readable
	   only by root. Also load all modules here. */
	auth = auth_preinit();
        password_schemes_init();

	auth_master_listeners_init();
	if (!worker)
		add_extra_listeners();

	/* Password lookups etc. may require roots, allow it. */
	restrict_access_by_env(FALSE);
}

static void main_init(int nodaemon)
{
	struct auth_master_listener *listener;

        process_start_time = ioloop_time;

	lib_signals_init();
        lib_signals_set_handler(SIGINT, TRUE, sig_die, NULL);
        lib_signals_set_handler(SIGTERM, TRUE, sig_die, NULL);
        lib_signals_set_handler(SIGPIPE, FALSE, NULL, NULL);

	/* If auth caches aren't used, just ignore these signals */
	lib_signals_set_handler(SIGHUP, FALSE, NULL, NULL);
	lib_signals_set_handler(SIGUSR2, FALSE, NULL, NULL);

	mech_init();
	auth_init(auth);
	auth_request_handler_init();

	if (worker) {
		worker_client =
			auth_worker_client_create(auth, WORKER_SERVER_FD);
		return;
	}

	standalone = getenv("DOVECOT_MASTER") == NULL;
	if (standalone) {
		/* starting standalone */
		if (getenv("AUTH_1") == NULL) {
			i_fatal("dovecot-auth is usually started through "
				"dovecot master process. If you wish to run "
				"it standalone, you'll need to set AUTH_* "
				"environment variables (AUTH_1 isn't set).");
		}

		if (!nodaemon) {
			switch (fork()) {
			case -1:
				i_fatal("fork() failed: %m");
			case 0:
				break;
			default:
				exit(0);
			}

			if (setsid() < 0)
				i_fatal("setsid() failed: %m");

			if (chdir("/") < 0)
				i_fatal("chdir(/) failed: %m");
		}
	} else {
		listener = auth_master_listener_create(auth);
		(void)auth_master_connection_create(listener, MASTER_SOCKET_FD);
		auth_master_listener_add(listener, CLIENT_LISTEN_FD,
					 NULL, LISTENER_CLIENT);
	}

	/* everything initialized, notify masters that all is well */
	auth_master_listeners_send_handshake();
}

static void main_deinit(void)
{
	if (worker_client != NULL)
		auth_worker_client_unref(worker_client);
	else
		auth_request_handler_flush_failures();

        auth_worker_server_deinit();
	auth_master_listeners_deinit();
	auth_request_handler_deinit();
	auth_deinit(auth);
	mech_deinit();

        password_schemes_deinit();
	random_deinit();

	lib_signals_deinit();
	closelog();
}

int main(int argc __attr_unused__, char *argv[])
{
	int foreground = FALSE;

#ifdef DEBUG
	if (getenv("GDB") == NULL)
		fd_debug_verify_leaks(WORKER_SERVER_FD + 1, 1024);
#endif
	/* NOTE: we start rooted, so keep the code minimal until
	   restrict_access_by_env() is called */
	lib_init();
	ioloop = io_loop_create(system_pool);

	while (argv[1] != NULL) {
		if (strcmp(argv[1], "-F") == 0)
			foreground = TRUE;
		else if (strcmp(argv[1], "-w") == 0)
			worker = TRUE;
		argv++;
	}

	drop_privileges();

	main_init(foreground);
        io_loop_run(ioloop);
	main_deinit();

	io_loop_destroy(ioloop);
	lib_deinit();

        return 0;
}
