
#ifndef __SNEKS_CONSOLE_H__
#define __SNEKS_CONSOLE_H__


extern void con_putstr(const char *string);	/* runtime provides */


/* set stdin, stdout, and stderr up to interact with a console over
 * con_putstr(), which typically goes to the serial port.
 */
extern int sneks_setup_console_stdio(void);

#endif
