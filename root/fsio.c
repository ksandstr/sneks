
/* fopen() on top of sys/fs.idl, using fopencookie(). */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sneks/sys/fs-defs.h>
#include <sneks/sys/info-defs.h>

#include <l4/types.h>
#include <l4/thread.h>


struct sfdpriv {
	L4_ThreadId_t server;
	L4_Word_t handle;
	ssize_t position;	/* negative is end-relative */
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


L4_Word_t fhandle_NP(FILE *f) {
	struct sfdpriv *priv = f->cookie;
	return priv->handle;
}


static ssize_t sfd_read(void *cookie, char *buf, size_t size)
{
	struct sfdpriv *priv = cookie;
	/* FIXME: is this how it works? even when size < SNEKS_FS_READ_MAX? */
	unsigned len = size;
	int n = __fs_read(priv->server, (uint8_t *)buf, &len,
		priv->handle, priv->position, size);
	if(n == 0) {
		priv->position += len;
		return len;
	} else {
		return idl2errno(n);
	}
}


static ssize_t sfd_write(void *cookie, const char *buf, size_t size)
{
	errno = ENOSYS;
	return -1;
}


static int sfd_seek(void *cookie, off64_t *offset, int whence)
{
	struct sfdpriv *priv = cookie;
	switch(whence) {
		case SEEK_SET: priv->position = *offset; break;
		case SEEK_CUR: priv->position += *offset; break;
		case SEEK_END: priv->position = -(int64_t)*offset; break;
	}
	if(priv->position < 0) *offset = 0;		/* FIXME: fstat and subtract */
	else *offset = priv->position;

	return 0;
}


static int sfd_close(void *cookie)
{
	struct sfdpriv *priv = cookie;
	int n = __fs_close(priv->server, priv->handle);
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


FILE *fopen(const char *path, const char *mode)
{
	L4_ThreadId_t fs = __get_rootfs();
	if(L4_IsNilThread(fs)) return NULL;
	L4_Word_t handle;
	/* FIXME: translate @mode to Sneks::Fs mode & flags */
	int n = __fs_openat(fs, &handle, 0, path, 0, 0);
	if(n != 0) {
		idl2errno(n);
		return NULL;
	}
	FILE *f = sfdopen_NP(fs, handle, mode);
	if(f != NULL) return f;
	else {
		/* dodgy, but unavoidable */
		n = __fs_close(fs, handle);
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
