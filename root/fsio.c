
/* fopen() on top of sys/fs.idl, using fopencookie(). */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <ccan/minmax/minmax.h>
#include <ccan/str/str.h>

#include <sneks/sys/info-defs.h>
#include <sneks/api/path-defs.h>
#include <sneks/api/file-defs.h>
#include <sneks/api/io-defs.h>

#include <l4/types.h>
#include <l4/thread.h>


struct sfdpriv {
	L4_ThreadId_t server;
	int handle;
};


/* NOTE: duplicated in user/crt/private.h, albeit inline */
static int idl2errno(int n)
{
	if(n == 0) return 0;
	else {
		errno = n < 0 ? -n : EIO;
		return -1;
	}
}


L4_ThreadId_t __get_rootfs(void)
{
	struct sneks_rootfs_info ri;
	int n = __info_rootfs_block(L4_Pager(), &ri);
	if(n == 0) return (L4_ThreadId_t){ .raw = ri.service };
	else {
		idl2errno(n);
		return L4_nilthread;
	}
}


L4_ThreadId_t fserver_NP(FILE *f) {
	struct sfdpriv *priv = f->cookie;
	return priv->server;
}


int fhandle_NP(FILE *f) {
	struct sfdpriv *priv = f->cookie;
	return priv->handle;
}


static ssize_t sfd_read(void *cookie, char *buf, size_t size)
{
	struct sfdpriv *priv = cookie;
	unsigned len = min_t(size_t, INT_MAX, size);
	int n = __io_read(priv->server, priv->handle, len, -1,
		(uint8_t *)buf, &len);
	return n == 0 ? len : idl2errno(n);
}


static ssize_t sfd_write(void *cookie, const char *buf, size_t size)
{
	errno = ENOSYS;
	return -1;
}


static int sfd_seek(void *cookie, off64_t *offset, int whence)
{
	struct sfdpriv *priv = cookie;
	off_t off = *offset;
	int n = __file_seek(priv->server, priv->handle, &off, whence);
	*offset = off;
	return idl2errno(n);
}


static int sfd_close(void *cookie)
{
	struct sfdpriv *priv = cookie;
	int n = __io_close(priv->server, priv->handle);
	free(priv);
	return idl2errno(n);
}


FILE *sfdopen_NP(L4_ThreadId_t server, L4_Word_t handle, const char *mode)
{
	struct sfdpriv *priv = malloc(sizeof *priv);
	if(priv == NULL) return NULL;
	*priv = (struct sfdpriv){ .server = server, .handle = handle };
	FILE *f = fopencookie(priv, mode, (cookie_io_functions_t){
		.read = &sfd_read, .write = &sfd_write,
		.seek = &sfd_seek, .close = &sfd_close,
	});
	if(f == NULL) free(priv);
	return f;
}


FILE *fopen(const char *path, const char *modestr)
{
	L4_ThreadId_t fs = __get_rootfs();
	if(L4_IsNilThread(fs)) return NULL;

	/* TODO: translate @mode to Sneks::{Path,File} mode & flags */
	mode_t mode = 0;
	int flags = O_RDONLY;
	if((flags & O_ACCMODE) != O_RDONLY || path == NULL || path[0] != '/') {
		errno = -EINVAL;
		return NULL;
	}

	/* TODO: this should only apply before actual rootfs has been mounted.
	 * it maps absolute paths on /initrd/ to the initrd filesystem which
	 * should appear as both /initrd and as /.
	 */
	if(strstarts(path, "/initrd/")) path += 8;
	while(path[0] == '/') path++;

	unsigned object;
	L4_ThreadId_t server;
	L4_Word_t cookie;
	int ifmt, n = __path_resolve(fs, &object, &server.raw,
		&ifmt, &cookie, 0, path, flags | mode);
	if(n != 0) {
		idl2errno(n);
		return NULL;
	}
	if((ifmt & SNEKS_PATH_S_IFMT) != SNEKS_PATH_S_IFREG) {
		errno = EBADF;	/* bizarre, but works */
		return NULL;
	}

	L4_Set_VirtualSender(L4_nilthread);
	assert(L4_IsNilThread(L4_ActualSender()));
	L4_ThreadId_t actual = L4_nilthread;
	int handle;
	n = __file_open(server, &handle, object, cookie, flags);
	if(n != 0) {
		idl2errno(n);
		return NULL;
	}
	actual = L4_ActualSender();
	if(!L4_IsNilThread(actual)) server = actual;

	FILE *f = sfdopen_NP(fs, handle, modestr);
	if(f != NULL) return f;
	else {
		/* dodgy, but unavoidable */
		n = __io_close(fs, handle);
		if(n > 0) {
			fprintf(stderr, "%s: cleanup can't reach server, n=%d\n",
				__func__, n);
		}
		return NULL;
	}
}


#ifdef BUILD_SELFTEST

#include <sneks/test.h>
#include <sneks/systask.h>


START_TEST(fopen_trivial)
{
	plan(7);

	FILE *f = fopen("/initrd/systest/root/fsiohello.txt", "rb");
	if(f == NULL) diag("fopen errno=%d", errno);
	skip_start(!ok(f != NULL, "fopen"), 6, "no file") {
		char str[100] = "";
		ok(fgets(str, sizeof str, f) != NULL, "fgets");
		ok(strcmp(str, "hello, root:fsio test\n") == 0, "line data");
		/* TODO: ok1(feof(f)) */

		ok(fseek(f, -5, SEEK_CUR) == 0, "fseek");
		memset(str, 0, sizeof str);
		ok(fgets(str, sizeof str, f) != NULL, "fgets");
		if(!ok(strcmp(str, "test\n") == 0, "short line data")) {
			diag("str=`%s'", str);
		}
		/* TODO: ok1(feof(f)) */

		if(!ok(fclose(f) == 0, "fclose")) diag("fclose errno=%d", errno);
	} skip_end;
}
END_TEST

SYSTASK_SELFTEST("root:fsio", fopen_trivial);

#endif
