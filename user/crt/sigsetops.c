
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <ccan/array_size/array_size.h>


#define LIMB_BITS(set) (8 * sizeof (set)->__bits[0])


static inline bool sig_valid(int signum) {
	return signum >= 1 && signum <= 64;
}


int sigemptyset(sigset_t *set) {
	for(int i=0; i < ARRAY_SIZE(set->__bits); i++) set->__bits[i] = 0;
	return 0;
}


int sigfillset(sigset_t *set) {
	for(int i=0; i < ARRAY_SIZE(set->__bits); i++) {
		set->__bits[i] = ~0ul;	/* all aboard, choo choo */
	}
	return 0;
}


int sigaddset(sigset_t *set, int signum)
{
	if(!sig_valid(signum)) {
		errno = EINVAL;
		return -1;
	}
	unsigned s = signum - 1;
	set->__bits[s / LIMB_BITS(set)] |= 1ul << s % LIMB_BITS(set);
	return 0;
}


int sigdelset(sigset_t *set, int signum)
{
	if(!sig_valid(signum)) {
		errno = EINVAL;
		return -1;
	}
	unsigned s = signum - 1;
	set->__bits[s / LIMB_BITS(set)] &= ~(1ul << s % LIMB_BITS(set));
	return 0;
}


int sigismember(sigset_t *set, int signum)
{
	if(!sig_valid(signum)) {
		errno = EINVAL;
		return -1;
	}
	unsigned s = signum - 1;
	return !!(set->__bits[s / LIMB_BITS(set)] & (1ul << s % LIMB_BITS(set)));
}
