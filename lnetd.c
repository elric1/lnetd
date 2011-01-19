/* $Id$ */

/*-
 * Copyright 2011  Morgan Stanley and Co. Incorporated
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <stdarg.h>

#define CHECK_TIME	1
#define MAXLOGLEN	1024

struct lnetd_ctx;

int			die_now = 0;

struct lnetd_ctx {
	char		 *progname;
	int		  num_kids;
	int		  max_kids;
	int		  wait_service;
	int		  daemonise;
	int		  debug;
	int		  fd;
	char		 *sockfn;
	dev_t		  sock_dev;
	ino_t		  sock_ino;
	mode_t		  sockmode;
	uid_t		  sockuid;
	gid_t		  sockgid;
	char		 *kid_prognam;
	char		*const *kid_args;
};

static void
vlog(struct lnetd_ctx *ctx, int pri, const char *fmt, va_list ap)
{
	char	buf[MAXLOGLEN];

	if (ctx && !ctx->debug && pri == LOG_DEBUG)
		return;

	vsnprintf(buf, sizeof(buf), fmt, ap);

	/*
	 * Log to stderr (if we're daemoni5ed, this is /dev/null),
	 * and syslog it.
	 */

	fprintf(stderr, "%s\n", buf);
	syslog(pri, "%s", buf);
}

static void
lnetd_log(struct lnetd_ctx *ctx, int pri, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(ctx, pri, fmt, ap);
	va_end(ap);
}

static void
fatal(struct lnetd_ctx *ctx, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(ctx, LOG_ERR, fmt, ap);
	va_end(ap);
	exit(1);
}

static void
lnetd_usage(struct lnetd_ctx *ctx)
{

	fatal(ctx, "usage: %s [-dw] [-N max_kids] command [args]",
	    ctx->progname);
}

static void
sighndler(int sig)
{

	/*
	 * If we're hupped or termed, we set a global and the rest
	 * of the program will gracefully shutdown.  Parent will kill
	 * the offspring, etc.
	 */

	switch (sig) {
	case SIGTERM:
	case SIGHUP:
		die_now = 1;
		break;

	default:
		break;
	}
}

static uid_t
parse_gid(struct lnetd_ctx *ctx, const char *group)
{
	struct group	*gr;

	gr = getgrnam(group);

	if (gr)
		return gr->gr_gid;

	fatal(ctx, "group \"%s\" not found.", group);
	/* NOTREACHED */
	return (gid_t)-1;
}

static uid_t
parse_uid(struct lnetd_ctx *ctx, const char *user)
{
	struct passwd	*pw;

	pw = getpwnam(user);

	if (pw)
		return pw->pw_uid;

	fatal(ctx, "user \"%s\" not found.", user);
	/* NOTREACHED */
	return (uid_t)-1;
}

static int
lnetd_process_args(struct lnetd_ctx *ctx, int argc,
		     char * const *argv)
{
	int	ch;

	memset(ctx, 0, sizeof(*ctx));

	ctx->progname		= strdup(*argv);
	ctx->daemonise		= 1;
	ctx->max_kids		= 30;
	ctx->sockmode		= 0666;
	ctx->sockuid		= (uid_t)-1;
	ctx->sockgid		= (gid_t)-1;
	ctx->fd			= -1;

/*
 * On linux, you have to prepend + to optstring to cause sane argument
 * processing to occur.  We hardcode this here rather than rely on the
 * user to set POSIXLY_CORRECT because for programs with a syntax that
 * accepts another program which has arguments, the GNU convention is
 * particularly stupid.
 */
#ifdef linux
#define POS "+"
#else
#define POS
#endif

	/*
	 * XXXrcd: should we provide an option to daemoni5e?
	 *         Hmmm.  Maybe just always be a daemon?
	 */

	while ((ch = getopt(argc, argv, POS "?N:g:m:u:w")) != -1)
		switch (ch) {
		case 'N':
			ctx->max_kids = atoi(optarg);
			break;
		case 'd':
			ctx->debug = 1;
			break;
		case 'g':
			ctx->sockgid = parse_gid(ctx, optarg);
			break;
		case 'm':
			ctx->sockmode = strtoul(optarg, NULL, 8);
			/* XXXrcd: errors??? */
			break;
		case 'u':
			ctx->sockuid = parse_uid(ctx, optarg);
			break;
		case 'w':
			ctx->wait_service = 1;
			ctx->max_kids = 1;
			break;
		default:
		case '?':
			lnetd_usage(ctx);
			break;
		}

	argc -= optind;
	argv += optind;

	if (argc < 2)
		fatal(ctx, "not enough args.");

	ctx->sockfn = *argv++;
	ctx->kid_prognam = *argv;
	ctx->kid_args = argv;

	/* XXXrcd: sanity checking is required... */

	return 1;
}

static int
lnetd_setup(struct lnetd_ctx *ctx)
{
	struct sigaction sa;

	lnetd_log(ctx, LOG_DEBUG, "enter lnetd setup");

	memset(&sa, 0x0, sizeof(sa));
	sa.sa_handler = sighndler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGCHLD, &sa, NULL) < 0)
		fatal(ctx, "could not reset SIGCHLD handler");

	if (sigaction(SIGHUP, &sa, NULL) < 0)
		fatal(ctx, "could not reset SIGHUP handler");

	if (sigaction(SIGTERM, &sa, NULL) < 0)
		fatal(ctx, "could not reset SIGTERM handler");

	if (sigaction(SIGALRM, &sa, NULL) < 0)
		fatal(ctx, "could not reset SIGALRM handler");

	/*
	 * Now we've done all of our initial setup, we are good to
	 * go.  We can close up stderr as we're not expecting any
	 * other setup errors...
	 */

	if (ctx->daemonise)
		daemon(0, 0);

	return 0;
}

static void
start_kid(struct lnetd_ctx *ctx, int fd)
{
	int	  ret;

	if (fd != 0) {
		ret = dup2(fd, 0);
		if (ret == -1) {
			lnetd_log(ctx, LOG_ERR, "dup2 failed: %s",
			    strerror(errno));
			_exit(0);
		}
		close(fd);
	}

	dup2(0, 1);
	dup2(0, 2);

	/* XXXrcd: deal with close-on-exec flags? */

	lnetd_log(ctx, LOG_INFO, "starting %s", ctx->kid_prognam);
	execv(ctx->kid_prognam, ctx->kid_args);

	lnetd_log(ctx, LOG_ERR, "execv failed: %s", strerror(errno));
	_exit(0); /* XXXrcd: no real better error code... */
}

/*
 * make_kid returns the number of kids it created, always 0 or 1.
 */

static int
make_kid(struct lnetd_ctx *ctx, int fd)
{
	pid_t	pid;

	pid = fork();
	switch (pid) {
	case 0:
		start_kid(ctx, fd);
		exit(0);
	case -1:
		lnetd_log(ctx, LOG_ERR, "fork failed: %s", strerror(errno));
		sleep(1);	/* back off */
		return 0;	/* not much to do but continue... */
	default:
		break;
	}
	return 1;
}


static int
is_socket_mine(struct lnetd_ctx *ctx)
{
	struct stat	sb;
	int		ret;

	ret = stat(ctx->sockfn, &sb);
	if (ret == -1) {
		/* File may have been removed... */
		lnetd_log(ctx, LOG_ERR, "stat(\"%s\", &sb2) failed: %s",
		    ctx->sockfn, strerror(errno));
		return 0;
	}

	if (sb.st_dev != ctx->sock_dev || sb.st_ino != ctx->sock_ino) {
		lnetd_log(ctx, LOG_ERR, "socket has changed: (%ld, %ld) "
		    "!= (%ld, %ld)", sb.st_dev, sb.st_ino, ctx->sock_dev,
		    ctx->sock_ino);
		return 0;
	}

	return 1;
}


static void
main_loop(struct lnetd_ctx *ctx)
{
	struct timeval	tv;
	fd_set		fds;
	int		fd = -1;
	int		status;
	int		ret;

	for (;;) {
		if (die_now) {			/* I've been told to die. */
			killpg(0, SIGHUP);	/* Kill the kids */
			break;			/* Go home */
		}

		/*
		 * If our socket has been replaced then no one can
		 * connect to us and we simply exit.
		 */
		if (!is_socket_mine(ctx))
			break;

		if (ctx->num_kids >= ctx->max_kids) {
			alarm(CHECK_TIME);
			if (waitpid(-1, &status, 0) > 0)
				ctx->num_kids--;
			alarm(0);
			continue;
		}

		while (waitpid(-1, &status, WNOHANG) > 0)
			ctx->num_kids--;

		if (ctx->wait_service) {
			FD_ZERO(&fds);
			FD_SET(ctx->fd, &fds);

			tv.tv_sec = CHECK_TIME;
			tv.tv_usec = 0;
			ret = select(ctx->fd+1, &fds, NULL, NULL, &tv);
		} else {
			alarm(CHECK_TIME);
			ret = fd = accept(ctx->fd, NULL, NULL);
			alarm(0);
		}

		switch (ret) {
		case -1:	/* ignore errors... */
		case 0:
			break;

		default:
			ctx->num_kids += make_kid(ctx, fd);
			break;
		}
	}
}


static int
setup_socket(struct lnetd_ctx *ctx)
{
	struct sockaddr_un	 un;
	struct stat		 sb;
	mode_t			 old_umask;
	int			 fd;
	int			 ret;
	char			*tmppath;

	lnetd_log(ctx, LOG_DEBUG, "setup_socket...");

	/*
	 * WARNING: Changing this malloc or any code until "END WARNING"
	 * WARNING: may result in a BUFFER OVERFLOW
	 */

	if ((tmppath = malloc(strlen(ctx->sockfn) + 2)) == NULL) {
	    lnetd_log(ctx, LOG_ERR, "malloc failure in setup_socket");
	    return -1;
	}

	/* XXXrcd: lame tmp name, isn't it? */
	strcpy(tmppath, ctx->sockfn);
	strcat(tmppath, "X");

	/* END WARNING */

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		return -1;
	}

	/* do the bind dance... */
	/* XXXrcd: ensure that it is small enough... */
#ifdef HAS_SUN_LEN
	un.sun_len = strlen(tmppath) + 1;
#endif
	un.sun_family = AF_UNIX;
	strcpy(un.sun_path, tmppath);

	old_umask = umask(0);
	ret = bind(fd, (struct sockaddr *)&un, sizeof(un));
	umask(old_umask);
	if (ret) {
		perror("bind");
		goto done;
	}

	/* Store the dev/ino for future comparison */
	ret = stat(tmppath, &sb);
	if (ret == -1) {
		/* XXXrcd: deal with this. */
		lnetd_log(ctx, LOG_ERR, "can't stat tmppath: %s",
		    strerror(errno));
	}

	ctx->sock_dev = sb.st_dev;
	ctx->sock_ino = sb.st_ino;

	ret = chmod(tmppath, ctx->sockmode);
	if (ret) {
		perror("chmod");
		goto done;
	}

	ret = chown(tmppath, ctx->sockuid, ctx->sockgid);
	if (ret) {
		perror("chown");
		goto done;
	}

	ret = rename(tmppath, ctx->sockfn);
	if (ret) {
		perror("rename");
		goto done;
	}

	/* and the listen dance... */
	ret = listen(fd, 15);
	if (ret) {
		perror("listen");
		goto done;
	}

done:
	if (ret == -1 && fd != -1)
		close(fd);
	if (ret == -1)
		return -1;
	return fd;
}

int
main(int argc, char **argv)
{
	struct lnetd_ctx	 the_ctx;
	struct lnetd_ctx	*ctx = &the_ctx;

	openlog(*argv, LOG_PID, LOG_DAEMON);

	if (!lnetd_process_args(ctx, argc, argv)) {
		/* the argument processing routine will emit errors */
		exit(1);
	}

	if ((ctx->fd = setup_socket(ctx)) < 0)
		fatal(ctx, "socket setup: %s", ctx->sockfn);

	if (ctx->fd == -1)
		exit(1);

	lnetd_setup(ctx);
	main_loop(ctx);

	lnetd_log(ctx, LOG_INFO, "exiting.");
	return 0;
}
