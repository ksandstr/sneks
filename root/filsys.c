/* TODO: should respond to sysmsg wrt filesystem systasks crashing. */
#define ROOTPATH_IMPL_SOURCE
#define WANT_SNEKS_PATH_LABELS

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <ccan/array_size/array_size.h>
#include <ccan/htable/htable.h>
#include <ccan/list/list.h>
#include <ccan/minmax/minmax.h>
#include <ccan/str/str.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/syscall.h>
#include <sneks/hash.h>
#include <sneks/ipc.h>
#include <sneks/process.h>
#include <sneks/api/path-defs.h>
#include <sneks/api/namespace-defs.h>
#include <sneks/sys/msg-defs.h>
#include <sneks/sys/filesystem-defs.h>

#include "root-impl-defs.h"
#include "muidl.h"
#include "defs.h"

struct mount_info {
	L4_ThreadId_t fs, super;
	unsigned super_join;
	struct list_node link;	/* in (super)->subs */
	struct list_head subs;
};

static size_t rehash_mount_info(const void *, void *);

static L4_ThreadId_t rootfs_tid, filsys_tid;
static mtx_t mountinfo_lock;
static unsigned mountinfo_gen = 0;
static struct htable mountinfo_fs = HTABLE_INITIALIZER(mountinfo_fs, &rehash_mount_info, NULL);
static thrd_t async = 0;
static _Atomic bool async_running;

static size_t rehash_mount_info(const void *ptr, void *priv) {
	const struct mount_info *mi = ptr;
	return int_hash(pidof_NP(mi->fs));
}

static bool cmp_mount_info_to_fs_pid(const void *cand, void *key) {
	const struct mount_info *mi = cand;
	return pidof_NP(mi->fs) == *(const pid_t *)key;
}

static const char *deroot(const char *s) { while(*s == '/') s++; return s; }

L4_ThreadId_t __get_rootfs(void) { return rootfs_tid; } /* skip root resolver for root's fopen() etc. */

static struct mount_info *get_mount_info(pid_t pid) {
	return htable_get(&mountinfo_fs, int_hash(pid), &cmp_mount_info_to_fs_pid, &pid);
}

static int locked_add_mi(L4_ThreadId_t fs, struct mount_info *mi, struct mount_info *s)
{
	if(!htable_add(&mountinfo_fs, rehash_mount_info(mi, NULL), mi)) { free(mi); return -ENOMEM; }
	if(s != NULL) list_add_tail(&s->subs, &mi->link); else list_node_init(&mi->link);
	return 0;
}

/* together with rm_mount_info(), manage mountinfo_fs and its broadcast sync. */
static int add_mount_info(L4_ThreadId_t fs, L4_ThreadId_t super, unsigned join)
{
	pid_t super_pid = pidof_NP(super);
	struct mount_info *mi = malloc(sizeof *mi);
	if(mi == NULL) return -ENOMEM;
	*mi = (struct mount_info){ .fs = fs, .super_join = join, .super = super };
	list_head_init(&mi->subs);
	mtx_lock(&mountinfo_lock);
	int n = locked_add_mi(fs, mi, get_mount_info(super_pid));
	if(n == 0) mountinfo_gen++;
	mtx_unlock(&mountinfo_lock);
	if(n != 0) return n;
	n = __sysmsg_broadcast(sysmsg_tid, &(bool){ false }, 1 << SNEKS_NAMESPACE_MOUNTED_BIT, 0, (L4_Word_t[]){ super_pid, 0, fs.raw, join }, 4);
	if(n != 0 && !L4_IsNilThread(sysmsg_tid)) {
		printf("filsys: mount broadcast failed: %s\n", stripcerr(n));
		mtx_lock(&mountinfo_lock);
		htable_del(&mountinfo_fs, rehash_mount_info(mi, NULL), mi); list_del(&mi->link); free(mi);
		mtx_unlock(&mountinfo_lock);
		return n > 0 ? -EIO : n;
	}
	return 0;
}

static int rm_mount_info(L4_ThreadId_t fs)
{
	pid_t fs_pid = pidof_NP(fs);
	mtx_lock(&mountinfo_lock);
	struct mount_info *mi = get_mount_info(fs_pid);
	if(mi != NULL) {
		if(!list_empty(&mi->subs)) { mtx_unlock(&mountinfo_lock); return -EBUSY; }
		htable_del(&mountinfo_fs, int_hash(fs_pid), mi);
		list_del(&mi->link);
		mountinfo_gen++;
	}
	mtx_unlock(&mountinfo_lock);
	if(mi != NULL) {
		int n = __sysmsg_broadcast(sysmsg_tid, &(bool){ false }, 1 << SNEKS_NAMESPACE_MOUNTED_BIT, 0,
			(L4_Word_t[]){ pidof_NP(mi->super), SNEKS_NAMESPACE_M_UNMOUNT, fs.raw, mi->super_join }, 4);
		if(n != 0) {
			printf("filsys: umount broadcast failed: %s\n", stripcerr(n));
			/* shows up as -ESRCH from resolve into "dead" filesystem.
			 * TODO: use log_warn() when available.
			 */
		}
		free(mi);
	}
	return 0;
}

static int locked_get_fs_free(unsigned *gen_p, L4_Word_t *tid_raw_p, unsigned *join_p, int pid, int ix)
{
	struct mount_info *mi = get_mount_info(pid);
	if(mi == NULL) return -ENOENT;
	if(ix < -1) return -EINVAL;
	if(ix == -1) {
		*tid_raw_p = mi->super.raw;
		*join_p = mi->super_join;
	} else {
		struct mount_info *cur;
		list_for_each(&mi->subs, cur, link) {
			if(--ix < 0) {
				*tid_raw_p = cur->fs.raw;
				*join_p = cur->super_join;
				break;
			}
		}
	}
	*gen_p = mountinfo_gen;
	return ix >= 0 ? -ENOENT : 0;
}

static int filsys_get_fs_tree(unsigned *gen_p, L4_Word_t *tid_raw_p, unsigned *join_p, int pid, int ix)
{
	if(!IS_SYSTASK(pidof_NP(muidl_get_sender()))) return -EPERM;
	mtx_lock(&mountinfo_lock);
	int n = locked_get_fs_free(gen_p, tid_raw_p, join_p, pid, ix);
	mtx_unlock(&mountinfo_lock);
	return n;
}

static int async_spawn(int (*fn)(void *), void *p) {
	async_running = true;
	if(thrd_create(&async, fn, p) != thrd_success) { free(p); async_running = false; return -ENOMEM; }
	muidl_raise_no_reply();
	return 0;
}

static bool async_completed(void) {
	if(async_running) return false;
	if(async != 0) { thrd_join(async, NULL); async = 0; }
	return true;
}

static int async_reply(int n, L4_ThreadId_t server, L4_ThreadId_t client, void *p)
{
	L4_MsgTag_t tag = { };
	if(n != 0) tag = (L4_MsgTag_t){ .X.u = 1, .X.label = 1 };
	L4_Set_Propagation(&tag); L4_Set_VirtualSender(server);
	L4_LoadMR(0, tag.raw); L4_LoadMR(1, -n);
	if(L4_IpcFailed(L4_Reply(client))) {
		printf("filsys: async_reply to %lu:%lu failed: %s\n", L4_ThreadNo(client), L4_Version(client), stripcerr(L4_ErrorCode()));
	}
	async_running = false;
	free(p);
	return n;
}

/* encode @src as 6 groups of 1-4 base64 characters separated by colons. an
 * alternate encoding would pad short groups with '.' to always 4 bytes, or
 * omit separators when the length is already 4, but that's hardly useful.
 */
static void encode_l64a_16u8(char *restrict dest, const uint8_t src[static restrict 16])
{
	for(int a = 0; a < 16;) {
		if(a > 0) *dest++ = ':';
		int val = 0, i; for(i = 0; i < 3 && a < 16; i++, a++) val |= src[a] << (i * 8);
		const char *restrict s = l64a(val); assert(a64l(s) == val);
		int l = strlen(s); memcpy(dest, s, l + 1); dest += l;
	}
}

static int do_mount(L4_ThreadId_t caller, char *restrict source, char *restrict target, char *restrict fstype, unsigned int mountflags, char *restrict data)
{
	/* get superior's bits for @target */
	L4_ThreadId_t super;
	unsigned join, parent;
	int n, ifmt, spawn_flags;
	if(streq(target, "/")) {
		printf("mounting root filesystem fstype=`%s' from source=`%s'\n", fstype, source);
		assert(L4_IsNilThread(rootfs_tid)); /* NB: doesn't support replacing e.g. initrd w/ read-write root */
		super = L4_nilthread; join = 0; parent = 0;
	} else {
		assert(!L4_IsNilThread(rootfs_tid)); /* initrd must activate on boot. */
		n = __path_resolve(rootfs_tid, &join, &super.raw, &ifmt, &(L4_Word_t){ 0 }, 0, deroot(target), 0);
		if(n < 0) return n; else if(n > 0) goto ipcfail;
		if(ifmt != SNEKS_PATH_S_IFDIR) return -ENOTDIR;
		char t_parent[strlen(target) + 4]; snprintf(t_parent, sizeof t_parent, "%s/..", target);
		n = __path_resolve(rootfs_tid, &parent, &(L4_Word_t){ 0 }, &ifmt, &(L4_Word_t){ 0 }, 0, deroot(t_parent), 0);
		if(n < 0) return n; else if(n > 0) goto ipcfail;
		assert(parent != join);
	}
	/* launch systask */
	char drvpath[40], mntflags[12], devs[32] = "", devcky[40] = "", pobj[20];
	snprintf(mntflags, sizeof mntflags, "%#x", mountflags);
	snprintf(pobj, sizeof pobj, "%u", parent);
	if(strends(fstype, "!bootmod")) {
		assert(strrchr(fstype, '!') - fstype == 8); fstype[strlen(fstype) - 8] = '\0';
		if(L4_IsGlobalId(caller) && pidof_NP(caller) != pidof_NP(L4_Myself())) return -EPERM;
		snprintf(drvpath, sizeof drvpath, "fs.%s", fstype);
		spawn_flags = SPAWN_BOOTMOD;
	} else {
		snprintf(drvpath, sizeof drvpath, "/lib/sneks-0.0p0/fs.%s", fstype);
		spawn_flags = 0;
	}
	if(~mountflags & MS_NODEV) { /* NB: this doesn't consider whether the superior has nodev set. */
		snprintf(devs, sizeof devs, "%lu:%lu", L4_ThreadNo(devices_tid), L4_Version(devices_tid));
		encode_l64a_16u8(devcky, device_cookie_key.key);
	}
	L4_ThreadId_t fs = spawn_systask(spawn_flags, drvpath, "--source", source, "--mount-flags", mntflags, "--data", data,
		"--parent-directory-object", pobj, "--device-registry-tid", devs, "--device-cookie-key", devcky, NULL);
	if(L4_IsNilThread(fs)) { printf("spawn_systask(`%s') failed\n", drvpath); return -errno; }
	assert(L4_IsNilThread(super) == L4_IsNilThread(rootfs_tid));
	if(L4_IsNilThread(super)) rootfs_tid = fs;
	if(n = add_mount_info(fs, super, join), n < 0) { printf("filsys: add_mount_info failed: %s\n", strerror(-n)); return n; }
	/* resolve fs root to sync mount */
	if(n = __path_resolve(fs, &(unsigned){ 0 }, &(L4_Word_t){ 0 }, &(int){ 0 }, &(L4_Word_t){ 0 }, 0, "", 0), n != 0) {
		printf("filsys: failed to resolve root of new fs: %s\n", stripcerr(n));
		abort();	/* FIXME: unfuck */
	}
	/* confirm mount result. */
	for(int i=0; i < 20; i++) {
		L4_ThreadId_t server;
		n = __path_resolve(__get_rootfs(), &(unsigned){ 0 }, &server.raw, &(int){ 0 }, &(L4_Word_t){ 0 }, 0, deroot(target), 0);
		if(n != 0 && n != -ESRCH) {
			printf("filsys: mount confirm resolve failed: %s\n", stripcerr(n));
			return n;
		} else if(n == 0) {
			if(L4_SameThreads(fs, server)) break; else L4_Sleep(L4_TimePeriod(2000)); /* TODO: add a spin_s() that sleeps right away */
		}
	}
	return 0;
ipcfail:
	printf("%s: IPC errorcode=%d converted to EIO\n", __func__, n);
	return -EIO;	/* -EIEIO, honestly */
}

struct mount_param {
	L4_ThreadId_t caller;
	int mountflags;
	char *source, *target, *fstype, *data, strings[];
};

static int async_mount_fn(void *param_ptr) {
	struct mount_param *p = param_ptr;
	return async_reply(do_mount(p->caller, p->source, p->target, p->fstype, p->mountflags, p->data), filsys_tid, p->caller, p);
}

static int filsys_mount(const char *source, const char *target, const char *fstype, unsigned int mountflags, const char *data)
{
	if(target[0] != '/') return -EINVAL;
	if(!async_completed()) return -EBUSY;
	const struct {
		const char *src; int len; off_t dstoffs;
	} fields[] = {
#define F(n) { n, strlen(n), offsetof(struct mount_param, n) }
		F(source), F(target), F(fstype), F(data)
#undef F
	};
	size_t str_total = 0, spos = 0;
	for(int i=0; i < ARRAY_SIZE(fields); i++) str_total += fields[i].len + 1;
	struct mount_param *p = malloc(sizeof *p + str_total); if(p == NULL) return -ENOMEM;
	*p = (struct mount_param){ .mountflags = mountflags, .caller = muidl_get_sender() };
	for(int i=0; i < ARRAY_SIZE(fields); i++) {
		memcpy(p->strings + spos, fields[i].src, fields[i].len + 1);
		*(char **)((char *)p + fields[i].dstoffs) = p->strings + spos;
		spos += fields[i].len + 1;
	}
	return async_spawn(&async_mount_fn, p);
}

/* NB: could be more robust */
static int do_umount(char *restrict pathspec, int flags)
{
	/* get target path, confirm that it is the root path. */
	L4_ThreadId_t server, prev;
	unsigned target, root;
	int ifmt, n = __path_resolve(__get_rootfs(), &target, &server.raw, &ifmt, &(L4_Word_t){ 0 }, 0, deroot(pathspec), 0);
	if(n != 0) goto ipcfail;
	if(ifmt != SNEKS_PATH_S_IFDIR) return -ENOTDIR;
	if(n = __path_resolve(server, &root, &(L4_Word_t){ 0 }, &ifmt, &(L4_Word_t){ 0 }, 0, "", 0), n != 0) goto ipcfail;
	if(ifmt != SNEKS_PATH_S_IFDIR) return -ENOTDIR;
	if(root != target) return -EINVAL;
	/* shut down, remove path map, confirm effect */
	if(n = __fs_shutdown(server), n != 0) goto ipcfail;
	if(n = rm_mount_info(server), n < 0) return n;
	if(wait_until_gone(server, L4_Never) != 0) return -EIO;
	for(int i=0; i < 20; i++) {
		if(i > 0) L4_Sleep(L4_TimePeriod(2000)); /* TODO: use a sleeping spinner (see prev about spin_s()) */
		n = __path_resolve(__get_rootfs(), &(unsigned){ 0 }, &prev.raw, &ifmt, &(L4_Word_t){ 0 }, 0, deroot(pathspec), 0);
		if(n == 0 && !L4_SameThreads(prev, server)) break;
		else if(n != 0 && n != -ESRCH) {
			printf("filsys: post-shutdown resolve failed: %s\n", stripcerr(n));
			goto ipcfail;
		}
	}
	return 0;
ipcfail: if(n < 0) return n; else return -EIO;	/* TODO: translate `n'? */
}

struct umount_param {
	int flags;
	L4_ThreadId_t caller;
	char pathspec[];
};

static int async_umount_fn(void *param_ptr) {
	struct umount_param *p = param_ptr;
	return async_reply(do_umount(p->pathspec, p->flags), filsys_tid, p->caller, p);
}

static int filsys_umount(const char *pathspec, int flags)
{
	if(flags != 0 || pathspec[0] != '/') return -EINVAL;
	if(!async_completed()) return -EBUSY;
	size_t pslen = strlen(pathspec);
	struct umount_param *p = malloc(sizeof *p + pslen + 1); if(p == NULL) return -ENOMEM;
	*p = (struct umount_param){ .flags = flags, .caller = muidl_get_sender() };
	memcpy(p->pathspec, pathspec, pslen + 1);
	return async_spawn(&async_umount_fn, p);
}

static int rootfs_resolve(unsigned *object_p, L4_Word_t *server_raw_p, int *ifmt_p, L4_Word_t *cookie_p, int dirfd, const char *path, int flags)
{
	L4_ThreadId_t sender = L4_GlobalIdOf(muidl_get_sender());
	if(path[0] == '/' || dirfd != 0) return -EINVAL;
	L4_MsgTag_t tag = { .X.label = SNEKS_PATH_RESOLVE_LABEL, .X.u = 3, .X.t = 2 };
	L4_Set_Propagation(&tag); L4_Set_VirtualSender(sender);
	L4_LoadMR(0, tag.raw);
	L4_LoadMRs(1, 3, (L4_Word_t[]){ SNEKS_PATH_RESOLVE_SUBLABEL, 0, flags });
	L4_LoadMRs(4, 2, L4_StringItem(strlen(path) + 1, (void *)path).raw);
	tag = L4_Send(rootfs_tid);
	if(L4_IpcFailed(tag)) {
		printf("%s: failed to propagate: %s\n", __func__, stripcerr(L4_ErrorCode()));
		/* TODO: this should translate ErrorCode into ESRCH only when the
		 * error is that the root filesystem has snuffed it. transmission
		 * aborts and the like should be returned directly as EINTR.
		 */
		return -ESRCH;
	} else {
		muidl_raise_no_reply();
		return 0;
	}
}

static int enosys() { return -ENOSYS; }

int root_path_thread(void *param)
{
	assert(param == NULL);
	filsys_tid = L4_Myself();
	if(mtx_init(&mountinfo_lock, mtx_plain) != thrd_success) abort();
	static const struct root_path_vtable vtab = {
		/* Sneks::Path */
		.resolve = &rootfs_resolve,
		.get_path = &enosys, .get_path_fragment = &enosys, .stat_object = &enosys,
		/* Sneks::Namespace */
		.mount = &filsys_mount, .umount = &filsys_umount,
		.get_fs_tree = &filsys_get_fs_tree,
	};
	for(;;) {
		L4_Word_t st = _muidl_root_path_dispatch(&vtab);
		if(st == MUIDL_UNKNOWN_LABEL) {
			/* ignore it. see uapi_loop() for why. */
		} else if(st != 0 && !MUIDL_IS_L4_ERROR(st)) {
			printf("%s: dispatch status %#lx (last tag %#lx)\n", __func__, st, muidl_get_tag().raw);
		}
	}
}
