
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>

#include <sneks/systask.h>


struct hook log_hook = HOOK_INIT(log_hook, NULL);


void log_msgv(int level, const char *fmt, va_list args)
{
	char *str;
	int len = vasprintf(&str, fmt, args);
	if(len < 0) {
		fprintf(stderr, "%s[%d]: malloc failed (BULLSHIT IS AFOOT)\n",
			__func__, getpid());
		abort();
	}
	hook_call_front(&log_hook, &str, level);
	if(str != NULL) {
		int len = strlen(str);
		while(len > 0 && str[len - 1] == '\n') str[--len] = '\0';
		/* TODO: use an actual logging system */
		fprintf(stderr, "%s\n", str);
		free(str);
	}
}


int vasprintf(char **sptr, const char *fmt, va_list al)
{
	va_list copy; va_copy(copy, al);
	int len = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	*sptr = malloc(len + 1);
	if(*sptr == NULL) return -1;
	return vsnprintf(*sptr, len + 1, fmt, al);
}
