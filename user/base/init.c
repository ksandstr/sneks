
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ccan/minmax/minmax.h>
#include <ccan/darray/darray.h>
#include <ccan/array_size/array_size.h>
#include <ccan/opt/opt.h>


#define S_BIT 10
#define A_BIT 11
#define B_BIT 12
#define C_BIT 13


enum action_e {
	ACT_INITDEFAULT,
	ACT_WAIT,
};


struct inittab
{
	char *id, *process;	/* coallocated */
	short runlevels;	/* mask for 0..9 and [SABC]_BIT. */
	short action;
	short line;
};


static int inittab_linenum;
static darray(struct inittab *) inittab = darray_new();
static darray(char *) setenvs = darray_new();	/* X=Y */


static void PRINTF_FMT(2, 3) noreturn syntax(
	const struct inittab *t,
	const char *fmt, ...)
{
	char msgbuf[200];
	va_list al;
	va_start(al, fmt);
	int n = vsnprintf(msgbuf, sizeof msgbuf, fmt, al);
	if(n < 0) {
		fprintf(stderr, "%s: vsnprintf() failed, n=%d (fmt=`%s')\n",
			__func__, n, fmt);
		abort();
	}
	va_end(al);
	assert(msgbuf[n] == '\0');
	while(n > 0 && msgbuf[n - 1] == '\n') msgbuf[--n] = '\0';
	fprintf(stderr, "line %d: syntax error: %s\n",
		t != NULL ? t->line : inittab_linenum, msgbuf);
	abort();
}


static short parse_runlevel_spec(const char *str)
{
	short acc = 0;
	while(*str != '\0') {
		if(*str >= '0' && *str <= '9') acc |= 1 << (*str - '0');
		else if(*str == 'S') acc |= 1 << S_BIT;
		else if(*str >= 'A' && *str <= 'C') acc |= 1 << (A_BIT + *str - 'A');
		else syntax(NULL, "unrecognized `%c' in runlevel spec", *str);
		str++;
	}
	return acc;
}


static short parse_action_spec(const char *act)
{
	static const struct { const char *s; short a; } maps[] = {
		{ "initdefault", ACT_INITDEFAULT },
		{ "wait", ACT_WAIT },
	};
	/* brute force is appropriate for boot time. */
	for(int i=0; i < ARRAY_SIZE(maps); i++) {
		if(strcmp(maps[i].s, act) == 0) return maps[i].a;
	}
	syntax(NULL, "unrecognized `%s' in action spec", act);
}


static struct inittab *parse_inittab_line(char *line, size_t length)
{
	/* comment lines start with '#'. nowhere else does a '#' indicate
	 * a comment.
	 */
	if(line[0] == '#') return NULL;
	while(line[0] == ' ' || line[0] == '\t') line++;
	if(line[0] == '\0') return NULL;

	/* separate the four components: id, runlevels, action, and process. */
	char *bits[4], *cur = line;
	for(int i=0; i < ARRAY_SIZE(bits); i++) {
		bits[i] = cur;
		if(i < ARRAY_SIZE(bits) - 1) {
			cur = strchr(cur, ':');
			if(cur == NULL) {
				syntax(NULL, "invalid separators in `%s'", line);
			}
			*(cur++) = '\0';
		}
	}

	int idlen = strlen(bits[0]), proclen = strlen(bits[3]);
	struct inittab *res = malloc(sizeof *res + idlen + proclen + 2);
	res->id = (char *)&res[1];
	res->process = res->id + idlen + 1;
	memcpy(res->id, bits[0], idlen + 1);
	memcpy(res->process, bits[3], proclen + 1);
	res->runlevels = parse_runlevel_spec(bits[1]);
	res->action = parse_action_spec(bits[2]);
	res->line = inittab_linenum;

	return res;
}


static void parse_inittab(void)
{
	/* parse inittab. unfortunately since there's no file I/O in userspace,
	 * we'll use a built-in inittab instead.
	 */
	extern char _binary_etc_inittab_start, // _binary_etc_inittab_end,
		_binary_etc_inittab_size;
	FILE *stream = fmemopen(&_binary_etc_inittab_start,
		(unsigned long)&_binary_etc_inittab_size, "r");
	inittab_linenum = 1;
	char linebuf[256];
	while(fgets(linebuf, sizeof linebuf, stream) != NULL) {
		int len = strlen(linebuf);
		while(len > 0 && linebuf[len - 1] == '\n') linebuf[--len] = '\0';
		struct inittab *t = parse_inittab_line(linebuf, len);
		if(t != NULL) darray_push(inittab, t);
	}
	fclose(stream);
}


/* also validates and aborts if poorly-formed or duplicated. */
static int get_initdefault(void)
{
	int level = -1;
	struct inittab **i;
	darray_foreach(i, inittab) {
		struct inittab *t = *i;
		if(t->action != ACT_INITDEFAULT) continue;
		if(level > 0) syntax(t, "duplicated initdefault");
		level = ffsl(t->runlevels);
		if(level == 0 || level == S_BIT + 1) {
			syntax(t, "invalid runlevel spec in initdefault");
		}
	}
	return level - 1;	/* per ffsl() */
}


static void a_wait(const struct inittab *t)
{
	char *argp[] = { t->process, NULL };
	darray_push(setenvs, NULL);
	int pid = spawn_NP(t->process, argp, setenvs.item);
	if(pid < 0) {
		fprintf(stderr, "can't spawn `%s': errno=%d\n", t->process, errno);
		abort();
	}
	char *nil = darray_pop(setenvs);
	assert(nil == NULL);

	// fprintf(stderr, "waiting on pid=%d...\n", pid);
	int st, dead;
	do {
		dead = waitpid(pid, &st, 0);
	} while(dead < 0 && errno == EINTR);
	if(dead < 0 && errno != EINTR) {
		fprintf(stderr, "%s: error %d!\n", __func__, errno);
	}
}


static void a_ignore(const struct inittab *t) {
	/* read the label on the tin, it's not for anyone */
}


static void enter(int runlevel)
{
	static void (*const actions[])(const struct inittab *t) = {
		[ACT_INITDEFAULT] = &a_ignore,
		[ACT_WAIT] = &a_wait,
	};
	fprintf(stderr, "entering runlevel %d...\n", runlevel);
	struct inittab **it;
	darray_foreach(it, inittab) {
		const struct inittab *t = *it;
		if((t->runlevels & (1 << runlevel)) == 0) continue;
		assert(t->action >= 0 && t->action < ARRAY_SIZE(actions));
		(*actions[t->action])(t);
	}
}


static void chld_handler(int signum)
{
	for(;;) {
		int st, dead = waitpid(-1, &st, WNOHANG);
		if(dead < 0 && errno == ECHILD) break;

		/* TODO: find the child PID and remove its tracking stuff, or process
		 * a restart, or something.
		 */
		printf("init:%s: st=%d, dead=%d\n", __func__, st, dead);
	}
}


static noreturn void init_main(void)
{
	struct sigaction act = { .sa_handler = &chld_handler };
	int n = sigaction(SIGCHLD, &act, NULL);
	if(n < 0) {
		fprintf(stderr, "init: sigaction failed\n");
		abort();
	}

	for(;;) {
		int st, dead = waitpid(-1, &st, 0);
		if(dead < 0 && errno == ECHILD) {
			/* no immediate children. sleep until a signal does something. */
			sleep(60 * 60);
			continue;
		}

		printf("init:%s: st=%d, dead=%d\n", __func__, st, dead);
	}
}


static char *add_setenv_opt(const char *value, void *priv)
{
	char *eq = strchr(value, '=');
	if(eq == NULL) {
		char buf[200];
		snprintf(buf, sizeof buf,
			"setenv arg `%s' has no equals sign", value);
		return strdup(buf);
	}

	darray_push(setenvs, strdup(value));
	return NULL;
}


static void ignore_opt_error(const char *fmt, ...) {
	/* the art of not giving a fuck */
}


static const struct opt_table opts[] = {
	OPT_WITH_ARG("--setenv|-s", &add_setenv_opt, NULL, NULL,
		"set environment variable for inittab processes"),
	OPT_ENDTABLE
};


int main(int argc, char *argv[])
{
	const char *my_name;
	if((my_name = strrchr(argv[0], '/')) != NULL) my_name++;
	else my_name = argv[0];

	/* TODO: umask(022); */
	if(geteuid() != 0) {
		fprintf(stderr, "%s: must be superuser.\n", my_name);
		return 1;
	}

	opt_register_table(opts, NULL);
	if(!opt_parse(&argc, argv, &ignore_opt_error)) {
		fprintf(stderr, "%s: option parsing failed!\n", my_name);
#ifndef NDEBUG
		for(int i=0; i < argc; i++) {
			fprintf(stderr, "%s: argv[%d]=`%s'\n", my_name, i, argv[i]);
		}
#endif
		return 1;
	}

	parse_inittab();

	int runlevel = get_initdefault();
	if(argc > 1) {
		char *end = NULL;
		int val = strtol(argv[1], &end, 10);
		if(end > argv[1] && *end == '\0') runlevel = val;
		else {
			fprintf(stderr, "%s: not a runlevel argument `%s'\n",
				my_name, argv[1]);
			/* ignored. */
		}
	} else {
		fprintf(stderr, "%s: default runlevel is %d\n", my_name, runlevel);
	}
	enter(runlevel);
	init_main();

	return 0;
}
