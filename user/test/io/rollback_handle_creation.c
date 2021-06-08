
#ifdef __sneks__

#define WANT_SNEKS_IO_LABELS
#define WANT_SNEKS_PIPE_LABELS
#define WANT_SNEKS_DEVICE_NODE_LABELS
#define WANT_SNEKS_DIRECTORY_LABELS
#define WANT_SNEKS_FILE_LABELS

/* tests on rollbacks of handle-creating IDL calls.
 *
 * right now these aren't as strong tests as one would hope, because the way
 * they search for erroneously-remaining handles can fail to discover one in
 * the presence of a bastard handle assignment method. there is some effort
 * toward self-validation but until Sneks::IO gains an interface for
 * enumerating all handles belonging to a given process, these tests are able
 * to show green even when rollbacks are handled poorly. caveat lector.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ccan/darray/darray.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/api/io-defs.h>
#include <sneks/api/path-defs.h>
#include <sneks/api/pipe-defs.h>
#include <sneks/api/dev-defs.h>
#include <sneks/api/file-defs.h>
#include <sneks/api/directory-defs.h>
#include <sneks/process.h>
#include <sneks/crtprivate.h>
#include <sneks/test.h>


#define A_SHORT_NAP L4_TimePeriod(20 * 1000)
#define SEARCH_MIN 16


typedef darray(int) handles;

/* calls to different file descriptor constructors. when @send_only, IPC does
 * not have a receive phase to induce rollback. returns 0 on success, positive
 * L4_ErrorCode() or negative errno on failure.
 */
typedef int (*handle_ctor_fn)(handles *dst, bool send_only);


static char *testfile_path = TESTDIR "user/test/io/reg/testfile";
static char *testdir_path = TESTDIR "user/test";


static int cmp_int(const void *, const void *);
static int pipe_ctor(handles *, bool);
static int pipe_dup_ctor(handles *, bool);
static int pipe_dup_to_ctor(handles *, bool);
static int devnull_dup_ctor(handles *, bool);
static int devnull_dup_to_ctor(handles *, bool);
static int reg_ctor(handles *, bool);
static int reg_dup_ctor(handles *, bool);
static int reg_dup_to_ctor(handles *, bool);
static int dir_ctor(handles *, bool);
static int dir_dup_ctor(handles *, bool);
static int dir_dup_to_ctor(handles *, bool);


static const struct {
	const char *name;
	handle_ctor_fn fn;
} handle_ctors[] = {
	{ "pipe", &pipe_ctor },
	{ "pipe/dup", &pipe_dup_ctor },
	{ "pipe/dup_to", &pipe_dup_to_ctor },
#if 0
	/* this constructor is useless by itself because omitting a receive phase
	 * breaks propagation from UAPI to nullserv, causing nullserv to execute
	 * the open() call on UAPI's behalf and, ideally, rolling it back when the
	 * reply fails. consequently its correct behaviour cannot be tested by
	 * this method.
	 */
	{ "devnull", &devnull_ctor },
#endif
	{ "devnull/dup", &devnull_dup_ctor },
	{ "devnull/dup_to", &devnull_dup_to_ctor },
	{ "reg", &reg_ctor },
	{ "reg/dup", &reg_dup_ctor },
	{ "reg/dup_to", &reg_dup_to_ctor },
	{ "dir", &dir_ctor },
	{ "dir/dup", &dir_dup_ctor },
	{ "dir/dup_to", &dir_dup_to_ctor },
};


START_LOOP_TEST(rollback_handle_creation,
	iter, 0, ARRAY_SIZE(handle_ctors) * 2 - 1)
{
	const bool send_only = !!(iter & 1);
	const int ctor = iter >> 1;
	assert(ctor < ARRAY_SIZE(handle_ctors));
	diag("send_only=%s, ctor[%d]=`%s'",
		btos(send_only), ctor, handle_ctors[ctor].name);
	plan_tests(10);

	handles refs = darray_new();
	/* create first set of reference handles. */
	int n = (*handle_ctors[ctor].fn)(&refs, false);
	if(!ok(n == 0, "ctor (1st ref)")) diag("n=%d", n);
	/* possibly create second set. */
	handles chaff = darray_new();
	n = (*handle_ctors[ctor].fn)(&chaff, send_only);
	if(!ok(n == 0, "ctor (chaff)")) diag("n=%d", n);
	/* create third set. */
	n = (*handle_ctors[ctor].fn)(&refs, false);
	if(!ok(n == 0, "ctor (2nd ref)")) diag("n=%d", n);

	skip_start(!ok1(refs.size > 0), 4, "no reference handles") {
		/* hunt for handles in a reasonable range around the reference
		 * handles.
		 */
		handles raw = darray_new();
		int *it;
		L4_ThreadId_t server;
		darray_foreach(it, refs) {
			assert(*it > 0);
			struct fd_bits *bits = __fdbits(*it);
			server = bits->server;
			darray_push(raw, bits->handle);
		}
		qsort(raw.item, raw.size, sizeof raw.item[0], &cmp_int);
		int range = max(SEARCH_MIN, 2 * (raw.item[raw.size - 1] - raw.item[0] + 1)),
			low = max(1, raw.item[0] - range),
			high = raw.item[raw.size - 1] + range;
		diag("distance=%d, range=[%d..%d]", range, low, high);
		int n_closed = 0, n_unexpected = 0;
		bool no_error = true;
		struct fd_bits *cwd_bits = __fdbits(__cwd_fd);
		for(int i = low; i <= high; i++) {
			if(cwd_bits != NULL && cwd_bits->handle == i
				&& cwd_bits->server.raw == server.raw)
			{
				diag("avoided __cwd_fd's handle=%d", cwd_bits->handle);
				continue;
			}
			n = __io_close(server, i);
			if(n != 0) {
				if(n == -EBADF) continue;
				diag("Sneks::IO/close failed on i=%d, n=%d", i, n);
				no_error = false;
				break;
			}

			n_closed++;
			int *rawp = bsearch(&i, raw.item, raw.size, sizeof raw.item[0], &cmp_int);
			if(rawp != NULL) {
				assert(rawp >= &raw.item[0] && rawp < &raw.item[raw.size]);
				darray_remove(raw, rawp - &raw.item[0]);
			} else {
				bool was_chaff = false;
				darray_foreach(it, chaff) {
					struct fd_bits *bits = __fdbits(*it);
					assert(bits != NULL);
					if(bits->server.raw == server.raw && bits->handle == i) {
						diag("chaff *it=%d hit", *it);
						was_chaff = true;
						break;
					}
				}
				if(!was_chaff) {
					diag("unexpectedly closed i=%d", i);
					n_unexpected++;
				}
			}
		}
		ok1(no_error);
		darray_free(raw);

		bool fail = !imply_ok1(!send_only, n_closed == refs.size + chaff.size);
		fail = !imply_ok1(send_only, n_closed == refs.size) || fail;
		fail = !ok1(n_unexpected == 0) || fail;
		if(fail) diag("n_closed=%d, n_unexpected=%d", n_closed, n_unexpected);
	} skip_end;

	/* clean up. */
	int *it;
	bool no_error = true;
	darray_foreach(it, refs) {
		n = close(*it);
		if(n == 0 || errno != EBADF) {
			diag("close(2) ref handle *it=%d, errno=%d", *it, errno);
			no_error = false;
		}
	}
	darray_free(refs);
	bool chaff_closed = true; /* self-validation: chaff should've been hit. */
	darray_foreach(it, chaff) {
		n = close(*it);
		if(n == 0) {
			diag("chaff handle *it=%d was closed", *it);
			chaff_closed = false;
		} else if(errno != EBADF) {
			diag("close(2) chaff handle *it=%d failed, errno=%d", *it, errno);
			no_error = false;
		}
	}
	darray_free(chaff);

	ok1(chaff_closed);
	ok(no_error, "cleanup");
}
END_TEST

DECLARE_TEST("io:rollback", rollback_handle_creation);


static int cmp_int(const void *a, const void *b) {
	return *(const int *)a - *(const int *)b;
}


static int dup_ctor(handle_ctor_fn base, handles *result, bool send_only)
{
	handles tmp = darray_new();
	int *it, n = (*base)(&tmp, false);
	if(n != 0) {
		diag("%s: base call n=%d", __func__, n);
		goto end;
	}

	struct fd_bits *bits = tmp.size > 0 ? __fdbits(tmp.item[0]) : NULL;
	if(bits == NULL) {
		diag("%s: base returned no descriptors?", __func__);
		n = -ENOENT;
		goto end;
	}
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2, .X.label = SNEKS_IO_DUP_LABEL }.raw);
	L4_LoadMR(1, SNEKS_IO_DUP_SUBLABEL);
	L4_LoadMR(2, bits->handle);
	L4_MsgTag_t tag = send_only ? L4_Send_Timeout(bits->server, A_SHORT_NAP)
		: L4_Call_Timeouts(bits->server, A_SHORT_NAP, A_SHORT_NAP);
	if(L4_IpcFailed(tag)) return L4_ErrorCode();

	if(!send_only && L4_Label(tag) == 1) {
		L4_Word_t error; L4_StoreMR(1, &error);
		n = -(int)error;
	} else if(!send_only && L4_UntypedWords(tag) == 1) {
		L4_Word_t handle; L4_StoreMR(1, &handle);
		int fd = __create_fd(-1, bits->server, handle, 0);
		if(fd < 0) {
			n = fd;
			diag("%s: __create_fd failed, n=%d", __func__, n);
			__io_close(bits->server, handle);
		} else {
			darray_push(*result, fd);
			n = 0;
		}
	} else if(!send_only) {
		diag("%s: unknown reply tag=%#08lx from server=%lu:%lu", __func__,
			tag.raw, L4_ThreadNo(bits->server), L4_Version(bits->server));
		n = -666;	/* rock on! */
	}

end:
	darray_foreach(it, tmp) {
		close(*it);
	}
	darray_free(tmp);
	return n;
}


static int dup_to_ctor(handle_ctor_fn base, handles *result, bool send_only)
{
	handles tmp = darray_new();

	L4_ThreadId_t parent_tid = L4_MyGlobalId();
	int n, oth_pid = fork();
	if(oth_pid == 0) {
		L4_LoadMR(0, 0);
		L4_MsgTag_t tag = L4_Send_Timeout(parent_tid, L4_TimePeriod(20 * 1000));
		exit(L4_IpcSucceeded(tag) ? EXIT_SUCCESS : EXIT_FAILURE);
	} else if(oth_pid < 0) {
		n = -errno;
		diag("%s: stoolie fork failed, errno=%d", __func__, errno);
		goto end;
	}

	n = (*base)(&tmp, false);
	if(n != 0) {
		diag("%s: base call n=%d", __func__, n);
		goto end;
	}

	struct fd_bits *bits = tmp.size > 0 ? __fdbits(tmp.item[0]) : NULL;
	if(bits == NULL) {
		diag("%s: base returned no descriptors?", __func__);
		n = -ENOENT;
		goto end;
	}
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2, .X.label = SNEKS_IO_DUP_TO_LABEL }.raw);
	L4_LoadMR(1, SNEKS_IO_DUP_TO_SUBLABEL);
	L4_LoadMR(2, bits->handle);
	L4_LoadMR(3, oth_pid);
	L4_MsgTag_t tag = send_only ? L4_Send_Timeout(bits->server, A_SHORT_NAP)
		: L4_Call_Timeouts(bits->server, A_SHORT_NAP, A_SHORT_NAP);
	if(L4_IpcFailed(tag)) return L4_ErrorCode();

	if(!send_only && L4_Label(tag) == 1) {
		L4_Word_t error; L4_StoreMR(1, &error);
		n = -(int)error;
	} else if(!send_only && L4_UntypedWords(tag) == 1) {
		L4_Word_t handle; L4_StoreMR(1, &handle);
		int fd = __create_fd(-1, bits->server, handle, 0);
		if(fd < 0) {
			n = fd;
			diag("%s: __create_fd failed, n=%d", __func__, n);
			__io_close(bits->server, handle);
		} else {
			darray_push(*result, fd);
			n = 0;
		}
	} else if(!send_only) {
		diag("%s: unknown reply tag=%#08lx from server=%lu:%lu", __func__,
			tag.raw, L4_ThreadNo(bits->server), L4_Version(bits->server));
		n = -666;	/* dicks out for harambe */
	}

end:
	if(oth_pid > 0) {
		L4_Sleep(L4_TimePeriod(5 * 1000));
		L4_ThreadId_t sender;
		L4_MsgTag_t tag = L4_Wait_Timeout(A_SHORT_NAP, &sender);
		if(L4_IpcFailed(tag) || pidof_NP(sender) != oth_pid) {
			diag("%s: child process didn't sync, ec=%lu", L4_ErrorCode());
		} else {
			int st, dead = wait(&st);
			if(dead != oth_pid || !WIFEXITED(st) || WEXITSTATUS(st) != EXIT_SUCCESS) {
				diag("%s: child process wait; dead=%d, st=%#x", __func__, dead, st);
			}
		}
	}
	int *it;
	darray_foreach(it, tmp) {
		close(*it);
	}
	darray_free(tmp);
	return n;
}


static int pipe_ctor(handles *result, bool send_only)
{
	const L4_ThreadId_t server = __the_sysinfo->posix.pipe;
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2, .X.label = SNEKS_PIPE_PIPE_LABEL }.raw);
	L4_LoadMR(1, SNEKS_PIPE_PIPE_SUBLABEL);
	L4_LoadMR(2, 0);	/* flags */
	L4_MsgTag_t tag = send_only ? L4_Send_Timeout(server, A_SHORT_NAP)
		: L4_Call_Timeouts(server, A_SHORT_NAP, A_SHORT_NAP);
	if(L4_IpcFailed(tag)) return L4_ErrorCode();

	if(!send_only && L4_Label(tag) == 1) {
		L4_Word_t error; L4_StoreMR(1, &error);
		return -(int)error;
	} else if(!send_only && L4_UntypedWords(tag) == 2) {
		L4_Word_t rdwr[2]; L4_StoreMRs(1, 2, rdwr);
		for(int i=0; i < 2; i++) {
			int fd = __create_fd(-1, server, rdwr[i], 0);
			if(fd < 0) {
				diag("%s: __create_fd failed, n=%d", __func__, fd);
				__io_close(server, rdwr[i]);
				continue;
			}
			darray_push(*result, fd);
		}
	} else if(!send_only) {
		diag("%s: unknown reply tag=%#08lx from server", __func__, tag.raw);
		return -666;	/* |,,| */
	}

	return 0;
}


static int pipe_dup_ctor(handles *result, bool send_only) {
	return dup_ctor(&pipe_ctor, result, send_only);
}


static int pipe_dup_to_ctor(handles *result, bool send_only) {
	return dup_to_ctor(&pipe_ctor, result, send_only);
}


static int devnull_ctor(handles *result, bool send_only)
{
	unsigned object;
	L4_ThreadId_t server;
	L4_Word_t cookie;
	int ifmt, n = __path_resolve(__the_sysinfo->api.rootfs, &object,
		&server.raw, &ifmt, &cookie, 0, "dev/null", 0);
	if(n != 0) {
		diag("%s: Sneks::Path/resolve failed, n=%d", __func__, n);
		return n;
	} else if(ifmt != SNEKS_PATH_S_IFCHR) {
		diag("%s: resolved ifmt=%d is not character special?", __func__, ifmt);
		return -ENODEV;
	}

	L4_Set_VirtualSender(L4_nilthread);
	assert(L4_IsNilThread(L4_ActualSender()));
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 4, .X.label = SNEKS_DEVICE_NODE_OPEN_LABEL }.raw);
	L4_LoadMR(1, SNEKS_DEVICE_NODE_OPEN_SUBLABEL);
	L4_LoadMR(2, object);
	L4_LoadMR(3, cookie);
	L4_LoadMR(4, 0);	/* flags */
	L4_MsgTag_t tag = send_only ? L4_Send_Timeout(server, A_SHORT_NAP)
		: L4_Call_Timeouts(server, A_SHORT_NAP, A_SHORT_NAP);
	if(L4_IpcFailed(tag)) return L4_ErrorCode();
	L4_ThreadId_t actual = L4_ActualSender();
	if(!L4_IsNilThread(actual)) server = actual;

	if(!send_only && L4_Label(tag) == 1) {
		L4_Word_t error; L4_StoreMR(1, &error);
		diag("%s: Sneks::DeviceNode/open failed, error=%lu", __func__, error);
		n = -(int)error;
	} else if(!send_only && L4_UntypedWords(tag) == 1) {
		L4_Word_t handle; L4_StoreMR(1, &handle);
		int fd = __create_fd(-1, server, handle, 0);
		if(fd < 0) {
			n = fd;
			diag("%s: __create_fd failed, n=%d", __func__, n);
			__io_close(server, handle);
		} else {
			darray_push(*result, fd);
			n = 0;
		}
	} else if(!send_only) {
		diag("%s: unknown reply tag=%#08lx from pipeserv", __func__, tag.raw);
		n = -666;	/* chicken noises */
	}

	return n;
}


static int devnull_dup_ctor(handles *result, bool send_only) {
	return dup_ctor(&devnull_ctor, result, send_only);
}


static int devnull_dup_to_ctor(handles *result, bool send_only) {
	return dup_to_ctor(&devnull_ctor, result, send_only);
}


static int reg_ctor(handles *result, bool send_only)
{
	unsigned object;
	L4_ThreadId_t server;
	L4_Word_t cookie;
	while(testfile_path[0] == '/') testfile_path++;
	int ifmt, n = __path_resolve(__the_sysinfo->api.rootfs, &object,
		&server.raw, &ifmt, &cookie, 0, testfile_path, 0);
	if(n != 0) {
		diag("%s: Sneks::Path/resolve failed, n=%d", __func__, n);
		return n;
	} else if(ifmt != SNEKS_PATH_S_IFREG) {
		diag("%s: resolved ifmt=%d is not regular file?", __func__, ifmt);
		return -ENODEV;
	}

	L4_Set_VirtualSender(L4_nilthread);
	assert(L4_IsNilThread(L4_ActualSender()));
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 4, .X.label = SNEKS_FILE_OPEN_LABEL }.raw);
	L4_LoadMR(1, SNEKS_FILE_OPEN_SUBLABEL);
	L4_LoadMR(2, object);
	L4_LoadMR(3, cookie);
	L4_LoadMR(4, 0);	/* flags */
	L4_MsgTag_t tag = send_only ? L4_Send_Timeout(server, A_SHORT_NAP)
		: L4_Call_Timeouts(server, A_SHORT_NAP, A_SHORT_NAP);
	if(L4_IpcFailed(tag)) return L4_ErrorCode();
	L4_ThreadId_t actual = L4_ActualSender();
	if(!L4_IsNilThread(actual)) server = actual;

	if(!send_only && L4_Label(tag) == 1) {
		L4_Word_t error; L4_StoreMR(1, &error);
		diag("%s: Sneks::File/open failed, error=%lu", __func__, error);
		n = -(int)error;
	} else if(!send_only && L4_UntypedWords(tag) == 1) {
		L4_Word_t handle; L4_StoreMR(1, &handle);
		int fd = __create_fd(-1, server, handle, 0);
		if(fd < 0) {
			n = fd;
			diag("%s: __create_fd failed, n=%d", __func__, n);
			__io_close(server, handle);
		} else {
			darray_push(*result, fd);
			n = 0;
		}
	} else if(!send_only) {
		diag("%s: unknown reply tag=%#08lx from server", __func__, tag.raw);
		n = -666;	/* cluckity fuck */
	}

	return n;
}


static int reg_dup_ctor(handles *result, bool send_only) {
	return dup_ctor(&reg_ctor, result, send_only);
}


static int reg_dup_to_ctor(handles *result, bool send_only) {
	return dup_to_ctor(&reg_ctor, result, send_only);
}


static int dir_ctor(handles *result, bool send_only)
{
	unsigned object;
	L4_ThreadId_t server;
	L4_Word_t cookie;
	while(testdir_path[0] == '/') testdir_path++;
	int ifmt, n = __path_resolve(__the_sysinfo->api.rootfs, &object,
		&server.raw, &ifmt, &cookie, 0, testdir_path, 0);
	if(n != 0) {
		diag("%s: Sneks::Path/resolve failed, n=%d", __func__, n);
		return n;
	} else if(ifmt != SNEKS_PATH_S_IFDIR) {
		diag("%s: resolved ifmt=%d is not directory?", __func__, ifmt);
		return -ENODEV;
	}

	L4_Set_VirtualSender(L4_nilthread);
	assert(L4_IsNilThread(L4_ActualSender()));
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 4, .X.label = SNEKS_DIRECTORY_OPENDIR_LABEL }.raw);
	L4_LoadMR(1, SNEKS_DIRECTORY_OPENDIR_SUBLABEL);
	L4_LoadMR(2, object);
	L4_LoadMR(3, cookie);
	L4_LoadMR(4, 0);	/* flags */
	L4_MsgTag_t tag = send_only ? L4_Send_Timeout(server, A_SHORT_NAP)
		: L4_Call_Timeouts(server, A_SHORT_NAP, A_SHORT_NAP);
	if(L4_IpcFailed(tag)) return L4_ErrorCode();
	L4_ThreadId_t actual = L4_ActualSender();
	if(!L4_IsNilThread(actual)) server = actual;

	if(!send_only && L4_Label(tag) == 1) {
		L4_Word_t error; L4_StoreMR(1, &error);
		diag("%s: Sneks::Directory/opendir failed, error=%lu", __func__, error);
		n = -(int)error;
	} else if(!send_only && L4_UntypedWords(tag) == 1) {
		L4_Word_t handle; L4_StoreMR(1, &handle);
		int fd = __create_fd(-1, server, handle, 0);
		if(fd < 0) {
			n = fd;
			diag("%s: __create_fd failed, n=%d", __func__, n);
			__io_close(server, handle);
		} else {
			darray_push(*result, fd);
			n = 0;
		}
	} else if(!send_only) {
		diag("%s: unknown reply tag=%#08lx from server", __func__, tag.raw);
		n = -666;	/* a little fucky wucky */
	}

	return n;
}


static int dir_dup_ctor(handles *result, bool send_only) {
	return dup_ctor(&dir_ctor, result, send_only);
}


static int dir_dup_to_ctor(handles *result, bool send_only) {
	return dup_to_ctor(&dir_ctor, result, send_only);
}


#endif
