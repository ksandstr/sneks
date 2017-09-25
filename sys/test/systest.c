
/* system task testbench program.
 *
 * executes tests concerning system tasks (e.g. the general system task
 * runtime environment) in a system task.
 */

#include <stdio.h>
#include <threads.h>


static int thread_fn(void *param_ptr)
{
	printf("%s: executing! param_ptr=%p\n", __func__, param_ptr);
	return 666;
}


int main(void)
{
	printf("# hello, world!\n");
	printf("1..1\n");
	printf("ok 1\n");

	thrd_t t;
	int n = thrd_create(&t, &thread_fn, "piip buub");
	if(n != thrd_success) {
		printf("thrd_create failed, n=%d\n", n);
	} else {
		int res = 0;
		n = thrd_join(t, &res);
		if(n == thrd_success) {
			printf("thrd_join ok, res=%d\n", res);
		} else {
			printf("thrd_join failed, n=%d\n", n);
		}
	}

	return 0;
}
