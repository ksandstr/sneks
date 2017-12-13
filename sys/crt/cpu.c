
#include <sched.h>
#include <l4/thread.h>

int sched_getcpu(void)
{
	return L4_ProcessorNo();
}
