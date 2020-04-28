
#include <stdbool.h>
#include <l4/types.h>
#include <sneks/rollback.h>


void set_rollback(rollback_fn_t fn, L4_Word_t param, void *priv)
{
}


void set_confirm(rollback_fn_t fn, L4_Word_t param, void *priv)
{
}


void sync_confirm(void)
{
}


bool check_rollback(L4_Word_t dispatch_status)
{
	return false;
}
