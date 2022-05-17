#include <stdbool.h>
#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/kip.h>
#include <sneks/spin.h>

void __thrd_spin(spinner_t *s)
{
	bool nap = __the_kip->ProcessorInfo.X.processors == 0;
	if(!nap && s->count < 100) { s->count++; return; }
	if(!nap && s->count < 200) { s->count++; asm volatile("pause"); return; }
	L4_Sleep(L4_SchedulePrecision(__the_kip));
	if(!nap) {
		L4_Clock_t now = L4_SystemClock();
		if(s->last.raw != 0) s->count -= (now.raw - s->last.raw) / 16;
		s->last = now;
	}
}
