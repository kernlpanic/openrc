/*
 * plugin.c
 * Simple plugin handler
 */

/*
 * Copyright (c) 2007-2015 The OpenRC Authors.
 * See the Authors file at the top-level directory of this distribution and
 * https://github.com/OpenRC/openrc/blob/HEAD/AUTHORS
 *
 * This file is part of OpenRC. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution and at https://github.com/OpenRC/openrc/blob/HEAD/LICENSE
 * This file may not be copied, modified, propagated, or distributed
 *    except according to the terms contained in the LICENSE file.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "einfo.h"
#include "queue.h"
#include "rc.h"
#include "rc_exec.h"
#include "plugin.h"
#include "helpers.h"

#define RC_PLUGIN_HOOK "rc_plugin_hook"

bool rc_in_plugin = false;

typedef struct plugin
{
	char *name;
	void *handle;
	int (*hook)(RC_HOOK, const char *);
	TAILQ_ENTRY(plugin) entries;
} PLUGIN;
TAILQ_HEAD(, plugin) plugins;

void
rc_plugin_load(void)
{
	DIR *dp;
	struct dirent *d;
	PLUGIN *plugin;
	char *file = NULL;
	void *h;
	union {
		void *sym;
		int (*func)(RC_HOOK, const char *);
	} fptr;

	/* Don't load plugins if we're in one */
	if (rc_in_plugin)
		return;

	TAILQ_INIT(&plugins);

	if (!(dp = opendir(RC_PLUGINDIR)))
		return;

	while ((d = readdir(dp))) {
		if (d->d_name[0] == '.')
			continue;

		xasprintf(&file, RC_PLUGINDIR "/%s",  d->d_name);
		h = dlopen(file, RTLD_LAZY);
		free(file);
		if (h == NULL) {
			eerror("dlopen: %s", dlerror());
			continue;
		}

		fptr.sym = dlsym(h, RC_PLUGIN_HOOK);
		if (fptr.func == NULL) {
			eerror("%s: cannot find symbol `%s'",
			    d->d_name, RC_PLUGIN_HOOK);
			dlclose(h);
		} else {
			plugin = xmalloc(sizeof(*plugin));
			plugin->name = xstrdup(d->d_name);
			plugin->handle = h;
			plugin->hook = fptr.func;
			TAILQ_INSERT_TAIL(&plugins, plugin, entries);
		}
	}
	closedir(dp);
}

void
rc_plugin_run(RC_HOOK hook, const char *value)
{
	PLUGIN *plugin;
	struct sigaction sa;
	sigset_t empty;
	sigset_t full;
	sigset_t old;
	int pfd[2];
	pid_t pid;
	char *buffer;
	char *token;
	char *p;
	ssize_t nr;
	int retval;

	/* Don't run plugins if we're in one */
	if (rc_in_plugin)
		return;

	/* We need to block signals until we have forked */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	sigemptyset(&empty);
	sigfillset(&full);

	TAILQ_FOREACH(plugin, &plugins, entries) {
		/* We create a pipe so that plugins can affect our environment
		 * vars, which in turn influence our scripts.
		 * O_CLOEXEC to stop any scripts from inheriting us.
		 * This is actually quite important as without this, the splash
		 * plugin will probably hang when running in silent mode. */
		if (pipe2(pfd, O_CLOEXEC) == -1) {
			eerror("pipe2: %s", strerror(errno));
			return;
		}

		sigprocmask(SIG_SETMASK, &full, &old);

		/* We run the plugin in a new process so we never crash
		 * or otherwise affected by it */
		if ((pid = fork()) == -1) {
			eerror("fork: %s", strerror(errno));
			break;
		}

		if (pid == 0) {
			/* Restore default handlers */
			sigaction(SIGCHLD, &sa, NULL);
			sigaction(SIGHUP,  &sa, NULL);
			sigaction(SIGINT,  &sa, NULL);
			sigaction(SIGQUIT, &sa, NULL);
			sigaction(SIGTERM, &sa, NULL);
			sigaction(SIGUSR1, &sa, NULL);
			sigaction(SIGWINCH, &sa, NULL);
			sigprocmask(SIG_SETMASK, &old, NULL);

			rc_in_plugin = true;
			close(pfd[0]);
			rc_environ_fd = fdopen(pfd[1], "w");
			retval = plugin->hook(hook, value);
			fclose(rc_environ_fd);
			rc_environ_fd = NULL;

			/* Just in case the plugin sets this to false */
			rc_in_plugin = true;
			exit(retval);
		}

		sigprocmask(SIG_SETMASK, &old, NULL);
		close(pfd[1]);
		buffer = xmalloc(sizeof(char) * BUFSIZ);
		memset(buffer, 0, BUFSIZ);

		while ((nr = read(pfd[0], buffer, BUFSIZ)) > 0) {
			p = buffer;
			while (*p && p - buffer < nr) {
				token = strsep(&p, "=");
				if (token) {
					unsetenv(token);
					if (*p) {
						setenv(token, p, 1);
						p += strlen(p) + 1;
					} else
						p++;
				}
			}
		}

		free(buffer);
		close(pfd[0]);

		rc_waitpid(pid);
	}
}

void
rc_plugin_unload(void)
{
	PLUGIN *plugin = TAILQ_FIRST(&plugins);
	PLUGIN *next;

	while (plugin) {
		next = TAILQ_NEXT(plugin, entries);
		dlclose(plugin->handle);
		free(plugin->name);
		free(plugin);
		plugin = next;
	}
	TAILQ_INIT(&plugins);
}
