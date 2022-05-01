/* plugins for con_putstr() in conjunction with stdio_portable.c . */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sneks/console.h>

ssize_t sneks_con_write(void *cookie, const char *buf, size_t size)
{
	char copy[size + 1]; /* TODO: fugly. replace w/ something that takes a length parameter. */
	memcpy(copy, buf, size);
	copy[size] = '\0';
	con_putstr(copy);
	return size;
}

/* set stdin, stdout, and stderr up to interact with a console over
 * con_putstr(), which typically goes to the serial port.
 */
int sneks_setup_console_stdio(void)
{
	FILE *out = fopencookie(NULL, "wb", (cookie_io_functions_t){ .write = &sneks_con_write });
	if(out == NULL) return -ENOMEM;

	if(stdin != NULL) fclose(stdin);
	stdin = out;	/* yeah! that's right */
	if(stdout != NULL) fclose(stdout);
	stdout = out;
	if(stderr != NULL) fclose(stderr);
	stderr = out;

	return 0;
}
