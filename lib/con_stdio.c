
/* plugins for con_putstr() in conjunction with stdio_portable.c . */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sneks/console.h>


long sneks_con_write(void *cookie, const char *buf, size_t size)
{
	/* worst possible. revise con_putstr() to something like con_write(),
	 * where it'll take a size parameter.
	 */
	char copy[size + 1];
	memcpy(copy, buf, size);
	copy[size] = '\0';
	con_putstr(copy);

	return size;
}


int sneks_setup_console_stdio(void)
{
	FILE *out = fopencookie(NULL, "wb",
		(cookie_io_functions_t){ .write = &sneks_con_write });
	if(out == NULL) return -ENOMEM;

	if(stdin != NULL) fclose(stdin);
	stdin = out;	/* yeah! that's right */
	if(stdout != NULL) fclose(stdout);
	stdout = out;
	if(stderr != NULL) fclose(stderr);
	stderr = out;

	return 0;
}
