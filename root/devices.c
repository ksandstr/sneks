
#define ROOTDEVICES_IMPL_SOURCE
#define SNEKS_DEVICENODE_IMPL_SOURCE
#define WANT_SNEKS_DEVICE_NODE_LABELS

#include <stdlib.h>
#include <stdint.h>
#include <alloca.h>
#include <errno.h>
#include <muidl.h>
#include <ccan/htable/htable.h>
#include <ccan/str/str.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/hash.h>
#include <sneks/cookie.h>
#include <sneks/api/dev-defs.h>

#include "defs.h"
#include "root-impl-defs.h"


#define DEV_OBJECT(t, major, minor) \
	((((t) & 3) << 30) | ((major) & 0x7fff) << 15 | ((minor) & 0x7fff))


struct dev_entry {
	int type, major, minor;
	L4_ThreadId_t server;
	int timeouts;
	char name[];
};


static size_t rehash_entry_by_devno(const void *key, void *priv);
static size_t rehash_entry_by_name(const void *key, void *priv);


L4_ThreadId_t devices_tid;

static struct htable devno_ht = HTABLE_INITIALIZER(
		devno_ht, &rehash_entry_by_devno, NULL),
	devname_ht = HTABLE_INITIALIZER(
		devname_ht, &rehash_entry_by_name, NULL);


static size_t rehash_entry_by_devno(const void *key, void *priv) {
	const struct dev_entry *ent = key;
	return int_hash(ent->major << 15 | ent->minor);
}


static size_t rehash_entry_by_name(const void *key, void *priv) {
	const struct dev_entry *ent = key;
	return hash_string(ent->name);
}


static int resolve_device(char *sbuf, size_t sbuflen, uint32_t object)
{
	/* TODO: use a data structure for this and load its contents from
	 * somewhere at first use (or whenever initrd has become available)
	 */
	const char *serv;
	switch(object) {
		case DEV_OBJECT(2, 1, 3): /* null */
		case DEV_OBJECT(2, 1, 5): /* zero */
		case DEV_OBJECT(2, 1, 7): /* full */
			serv = "nullserv";
			break;
		default:
			return -ENODEV;
	}
	/* TODO: remove /initrd to be compatible with post-boot drivers */
	return snprintf(sbuf, sbuflen, "/initrd/lib/sneks-0.0p0/%s", serv);
}


static bool propagate_devnode_open(
	bool *live_p, struct dev_entry *cand,
	uint32_t object, L4_Word_t cookie, int flags)
{
	L4_MsgTag_t tag = { .X.label = SNEKS_DEVICE_NODE_OPEN_LABEL, .X.u = 4 };
	L4_Set_Propagation(&tag);
	L4_Set_VirtualSender(muidl_get_sender());
	L4_LoadMR(0, tag.raw);
	L4_LoadMR(1, SNEKS_DEVICE_NODE_OPEN_SUBLABEL);
	L4_LoadMR(2, object);
	L4_LoadMR(3, cookie);
	L4_LoadMR(4, flags);
	tag = L4_Send_Timeout(cand->server,
		L4_TimePeriod(COOKIE_PERIOD_US * 2));
	if(L4_IpcSucceeded(tag)) return true;

	bool dead = false;
	if(L4_ErrorCode() == 2) {
		/* send-phase timeout */
		if(++cand->timeouts >= 5) dead = true;
	} else if(L4_ErrorCode() == 4) {
		/* "non-existing partner" */
		dead = true;
	} else {
		dead = true;
	}
	if(!dead) *live_p = true;
	else {
		htable_del(&devno_ht, rehash_entry_by_devno(cand, NULL), cand);
		htable_del(&devname_ht, rehash_entry_by_name(cand, NULL), cand);
		free(cand);
	}
	return false;
}


/* TODO: the error handling logic in this function is completely untested. a
 * suitable test would have a "wedgedev" that never responds, and succeed when
 * the client receives an error code in finite time without becoming
 * indefinitely wedged.
 */
static int devices_open(int *handle_p,
	uint32_t object, L4_Word_t cookie, int flags)
{
	int type = object >> 30, major = (object >> 15) & 0x7fff,
		minor = object & 0x7fff;
	if(type != 2 && type != 0) return -ENODEV;

	if(!validate_cookie(cookie, &device_cookie_key,
		L4_SystemClock(), object, pidof_NP(muidl_get_sender())))
	{
		/* nfg */
		return -EINVAL;
	}

	cookie = 0x12345678;	/* replace w/ obvious dummy */

	struct dev_entry key = { .type = type, .major = major, .minor = minor };
	size_t devno_hash = rehash_entry_by_devno(&key, NULL);
	struct htable_iter it;
	bool live = false;
	for(struct dev_entry *cand = htable_firstval(&devno_ht, &it, devno_hash);
		cand != NULL; cand = htable_nextval(&devno_ht, &it, devno_hash))
	{
		if(cand->type == key.type && cand->minor == key.minor
			&& cand->major == key.major)
		{
			if(propagate_devnode_open(&live, cand, object, cookie, flags)) {
				muidl_raise_no_reply();
				return 0;
			}
		}
	}
	/* lookup by name. */
	char sbuf[64], *spawnpath = sbuf;
	int n = resolve_device(sbuf, sizeof sbuf, object);
	if(n > sizeof sbuf - 1) {
		/* (GCC has a warning about alloca parameter being too large, when
		 * this certainly isn't. fuck them; remove the cast once sense has
		 * returned.)
		 */
		spawnpath = alloca((unsigned short)n + 1);
		int m = resolve_device(spawnpath, n + 1, object);
		assert(m <= n);
		n = m;
	}
	if(n < 0) return n;
	size_t name_hash = hash_string(spawnpath);
	for(struct dev_entry *cand = htable_firstval(&devname_ht, &it, name_hash);
		cand != NULL; cand = htable_nextval(&devname_ht, &it, name_hash))
	{
		if(!streq(cand->name, spawnpath)) continue;
		if(propagate_devnode_open(&live, cand, object, cookie, flags)) {
			muidl_raise_no_reply();
			return 0;
		}
	}
	if(live) {
		/* there were candidate devices, but none of them responded, which
		 * means they must be wedged somehow as devices are wont to do.
		 * suggest the client resolve its path again and give it another
		 * whirl, since the cookie would have aged off already anyway.
		 */
		return -ETIMEDOUT;
	}

	/* not found; start anew. */
	assert(n == strlen(spawnpath));
	struct dev_entry *ent = malloc(sizeof *ent + n + 1);
	if(ent == NULL) return -ENOMEM;
	*ent = (struct dev_entry){
		.type = type, .minor = minor, .major = major,
		.server = spawn_systask_from_initrd(spawnpath, NULL),
	};
	if(L4_IsNilThread(ent->server)) {
		free(ent);
		return -ENODEV;
	}
	assert(n == strlen(spawnpath));
	memcpy(ent->name, spawnpath, n + 1);

	assert(devno_hash == rehash_entry_by_devno(ent, NULL));
	bool ok = htable_add(&devno_ht, devno_hash, ent);
	if(ok) {
		assert(name_hash == rehash_entry_by_name(ent, NULL));
		ok = htable_add(&devname_ht, name_hash, ent);
	}
	if(!ok) {
		htable_del(&devno_ht, devno_hash, ent);
		free(ent);
		return -ENOMEM;
	}
	if(propagate_devnode_open(&live, ent, object, cookie, flags)) {
		muidl_raise_no_reply();
		return 0;
	} else {
		return -ETIMEDOUT;
	}
}


static int devices_enosys() {
	return -ENOSYS;
}


int devices_loop(void *param_ptr)
{
	devices_tid = L4_MyGlobalId();

	static const struct root_devices_vtable vtab = {
		.open = &devices_open,
		.ioctl_void = &devices_enosys,
		.ioctl_int = &devices_enosys,
	};
	for(;;) {
		L4_Word_t st = _muidl_root_devices_dispatch(&vtab);
		if(st == MUIDL_UNKNOWN_LABEL) {
			/* fie! */
		} else if(st != 0 && !MUIDL_IS_L4_ERROR(st)) {
			printf("%s: dispatch status %#lx (last tag %#lx)\n",
				__func__, st, muidl_get_tag().raw);
		}
	}

	return -1;
}


void devices_init(void)
{
	/* TODO: browse through initrd's /lib/sneks-whatever drivers and look for
	 * built-in driver matching data to initialize our database with.
	 */
}
