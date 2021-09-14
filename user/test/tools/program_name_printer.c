#include <stdio.h>
#include <errno.h>
#define PRINT(x) printf(#x "=%s\n", (x) != NULL ? (x) : "(null)")
int main(int argc, char *argv[]) {
	PRINT(argv[0]);
	PRINT(program_invocation_name);
	PRINT(program_invocation_short_name);
	return 0;
}
