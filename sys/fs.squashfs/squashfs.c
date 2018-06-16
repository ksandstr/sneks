
#define SQUASHFSIMPL_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <alloca.h>
#include <ccan/opt/opt.h>
#include <ccan/endian/endian.h>
#include <ccan/array_size/array_size.h>
#include <ccan/likely/likely.h>
#include <ccan/str/str.h>
#include <ccan/htable/htable.h>
#include <ccan/container_of/container_of.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include <sneks/mm.h>
#include <sneks/hash.h>
#include <sneks/lz4.h>
#include <sneks/bitops.h>
#include <ukernel/rangealloc.h>

#include "muidl.h"
#include "defs.h"
#include "squashfs_fs.h"
#include "squashfs-impl-defs.h"


/* for 32k processes, this is 2 FDs each. perhaps not enough, but unlikely to
 * be hit in practice.
 */
#define MAX_FD ((1 << 16) - 1)


/* cached decompressed block. addresses are relative to start of image, and
 * indicate where the compressed data starts.
 *
 * TODO: add list link for replacement.
 */
struct blk
{
	uint64_t block;
	unsigned length;
	uint64_t next_block;
	uint8_t data[];	/* [length] */
};


/* contained within a per-fs inode info structure. */
struct inode
{
	unsigned long ino;	/* int64_hash() */
	/* TODO: add basic stat(2) output's fields here */
};


struct squashfs_file
{
	struct inode *i;
	size_t pos;
	int refs;
};


struct inode_ext
{
	union squashfs_inode X;	/* TODO: split this one up a bit? */
	/* block+offset pair for data that follows the on-image inode, such as
	 * block_list[] for regular files.
	 */
	uint64_t rest_start;
	int offset;
	struct inode fs_inode;
};


/* TODO: define ->flags. */
struct fd
{
	struct squashfs_file *file;
	uint16_t owner, flags;
};


static size_t rehash_blk(const void *key, void *priv);
static size_t rehash_inode(const void *key, void *priv);


static L4_ThreadId_t boot_tid;

/* mount data */
static void *fs_image;
static size_t fs_length;
static int fs_block_size_log2;

static const struct squashfs_super_block *fs_super;
static struct htable blk_cache = HTABLE_INITIALIZER(
		blk_cache, &rehash_blk, NULL),
	inode_cache = HTABLE_INITIALIZER(inode_cache, &rehash_inode, NULL);

static struct rangealloc *fd_ra;	/* <struct fd>, never 0. */


static size_t rehash_blk(const void *key, void *priv) {
	const struct blk *b = key;
	return int64_hash(b->block);
}

static bool cmp_blk_addr(const void *cand, void *key) {
	const struct blk *b = cand;
	return b->block == *(const uint64_t *)key;
}


static size_t rehash_inode(const void *key, void *priv) {
	const struct inode *i = key;
	return int64_hash(i->ino);
}

static bool cmp_inode_ino(const void *cand, void *key) {
	const struct inode *i = cand;
	return i->ino == *(const unsigned long *)key;
}


static inline struct inode_ext *squashfs_i(struct inode *n) {
	return container_of(n, struct inode_ext, fs_inode);
}


/* FIXME: remove this in favour of a proper pidof() which actually tells
 * between userspace processes, systasks, and the forbidden low range.
 */
static unsigned pidof_NP(L4_ThreadId_t tid)
{
	if(likely(L4_IsGlobalId(tid))) return 1;
	else {
		/* it's a local TID, so return our local PID. no biggie. */
		return pidof_NP(L4_MyGlobalId());
	}
}


/* read block at @pos in the filesystem image. @length is as to cache_get(),
 * @output must have enough space for the kind of block being read. return
 * value is the number of bytes decompressed. *@next_block_p will be filled in
 * with the position of the next compressed block.
 */
static int read_block(
	void *output, uint64_t *next_block_p,
	size_t pos, int length)
{
	int n, sz, bufmax, compressed;
	if(length == 0) {
		/* metadata block read. these have an information word up front, which
		 * we'll decode.
		 */
		int lenword = *(uint8_t *)(fs_image + pos)
			| (int)*(uint8_t *)(fs_image + pos + 1) << 8;
		pos += 2;
		sz = SQUASHFS_COMPRESSED_SIZE(lenword);
		bufmax = SQUASHFS_METADATA_SIZE;
		compressed = SQUASHFS_COMPRESSED(lenword);
	} else {
		/* ordinary data, length supplied externally (possibly in a different
		 * format).
		 */
		sz = SQUASHFS_COMPRESSED_SIZE_BLOCK(length);
		compressed = SQUASHFS_COMPRESSED_BLOCK(length);
		bufmax = fs_super->block_size;
	}
	if(next_block_p != NULL) *next_block_p = pos + sz;

	if(compressed) {
		assert(fs_super->compression == LZ4_COMPRESSION);
		n = LZ4_decompress_safe_partial(fs_image + pos, output,
			sz, bufmax, bufmax);
		if(n < 0) {
			printf("LZ4 decompression failed, n=%d\n", n);
			abort();
		}
		// printf("decompressed %d -> %d bytes\n", (int)sz, n);
	} else {
		n = min(sz, bufmax);
		memcpy(output, fs_image + pos, n);
	}

	return n;
}


/* @length is 0 for metadata blocks and the compressed length otherwise. */
static struct blk *cache_get(uint64_t block, int length)
{
	size_t hash = int64_hash(block);
	struct blk *b = htable_get(&blk_cache, hash, &cmp_blk_addr, &block);
	if(b != NULL) return b;

	size_t max_size = length == 0 ? SQUASHFS_METADATA_SIZE
		: fs_super->block_size;
	b = calloc(sizeof *b + max_size, sizeof(uint8_t));
	if(b == NULL) return NULL;
	b->block = block;
	b->length = read_block(b->data, &b->next_block, block, length);
	struct blk *nb = realloc(b, sizeof *b + b->length);
	if(nb != NULL) b = nb;	/* otherwise, waste a bit of memory. */

	bool ok = htable_add(&blk_cache, hash, b);
	if(!ok) {
		free(b);
		b = NULL;
	}
#if 0
	else {
		printf("%s: inserted block=%#x; length=%u, next=%#x\n", __func__,
			(unsigned)block, b->length, (unsigned)b->next_block);
	}
#endif

	return b;
}


/* read @length bytes of metadata from *@block:*@offset. skip through metadata
 * blocks and update *@block and *@offset through. to optimize lookups, pass a
 * cache_get() result in *@blk_p, or leave @blk_p NULL when it doesn't matter.
 */
static int read_metadata(
	void *output, struct blk **blk_p, uint64_t *block, int *offset, int length)
{
	int done = 0;
	while(done < length) {
		if(*block >= fs_length) break;
		struct blk *b = blk_p != NULL ? *blk_p : NULL;
		if(b == NULL || b->block != *block) {
			b = cache_get(*block, 0);
			if(b == NULL) return -ENOMEM;
			if(blk_p != NULL) *blk_p = b;
		}
		int bytes = min_t(int, b->length - *offset, length - done);
		memcpy(output + done, &b->data[*offset], bytes);
		done += bytes;
		if(done < length) {
			assert(*offset + bytes == b->length);
			*block = b->next_block;
			*offset = 0;
		} else {
			*offset += bytes;
		}
	}
	return done;
}


static int inode_size(int type)
{
	static const uint8_t sizes[] = {
		[SQUASHFS_DIR_TYPE] = sizeof(struct squashfs_dir_inode),
		[SQUASHFS_REG_TYPE] = sizeof(struct squashfs_reg_inode),
	};

	return type < 0 || type >= ARRAY_SIZE(sizes) ? 0 : sizes[type];
}


static struct inode *read_inode(unsigned long ino)
{
	assert(!SQUASHFS_UNCOMPRESSED_INODES(fs_super->flags));

	struct inode_ext *nod_ext = malloc(sizeof *nod_ext);
	if(nod_ext == NULL) return NULL;	/* TODO: pop ENOMEM */
	struct inode *nod = &nod_ext->fs_inode;
	nod->ino = ino;

	uint64_t block = fs_super->inode_table_start + SQUASHFS_INODE_BLK(ino);
	int offset = SQUASHFS_INODE_OFFSET(ino);
	struct squashfs_base_inode *base = &squashfs_i(nod)->X.base;
	struct blk *blk = NULL;
	int n = read_metadata(base, &blk, &block, &offset, sizeof *base);
	if(n < sizeof *base) goto Eio;

#if 0
	printf("%s: ino=%u, inode_type=%u\n", __func__, (unsigned)ino,
		base->inode_type);
#endif
	int sz = inode_size(base->inode_type);
	if(sz > 0) {
		n = read_metadata(base + 1, &blk, &block, &offset, sz - sizeof *base);
		if(n < sz - sizeof *base) goto Eio;
		squashfs_i(nod)->rest_start = block;
		squashfs_i(nod)->offset = offset;
		// printf("... block=%#x, offset=%d\n", (unsigned)block, offset);
	} else {
		printf("fs.squashfs: unknown inode type %d\n", (int)base->inode_type);
		free(nod);
		return NULL;	/* FIXME: -EINVAL */
	}

	return nod;

Eio:
	free(nod);
	return NULL;		/* FIXME: -EIO */
}


/* fetch @ino from cache, or read it from the filesystem, or fail. */
static struct inode *get_inode(unsigned long ino)
{
	size_t hash = int64_hash(ino);
	struct inode *nod = htable_get(&inode_cache, hash, &cmp_inode_ino, &ino);
	if(nod == NULL) {
		nod = read_inode(ino);
		if(nod == NULL) return NULL;	/* TODO: propagate w/ IS_ERR() */
		bool ok = htable_add(&inode_cache, hash, nod);
		if(!ok) {
			free(nod);
			return NULL;	/* TODO: ENOMEM */
		}
	}
	// printf("%s: ino=%u, nod=%p\n", __func__, (unsigned)ino, nod);

	return nod;
}


static int64_t lookup(int *type, uint64_t dir_ino, const char *name)
{
	assert(type != NULL);
	assert(name[0] != '\0');

	struct inode *nod = get_inode(dir_ino);
	if(nod == NULL) return -EIO;	/* FIXME: translate error pointer */
	struct squashfs_dir_inode *dir = &squashfs_i(nod)->X.dir;

#if 0
	printf("%s: dir_ino=%u, inode_number=%u, start_block=%#x, offset=%u\n",
		__func__, (unsigned)dir_ino, dir->inode_number,
		dir->start_block, dir->offset);
#endif

	uint64_t block = dir->start_block + fs_super->directory_table_start;
	int offset = dir->offset;

	/* TODO: handle LDIRs as well. (that dir_index stuff.) */
	assert(dir->inode_type == SQUASHFS_DIR_TYPE);

	struct blk *blk = NULL;

	int namelen = strlen(name);
	int64_t ino = -ENOENT;
	struct squashfs_dir_entry *dent = alloca(sizeof *dent
		+ SQUASHFS_NAME_LEN + 1);
	int n_read = 0;
	while(n_read < dir->file_size) {
		struct squashfs_dir_header hdr;
		int n = read_metadata(&hdr, &blk, &block, &offset, sizeof hdr);
		if(n < sizeof hdr) return -EIO;
		n_read += n;

		for(int i=0; i <= hdr.count; i++) {
			n = read_metadata(dent, &blk, &block, &offset, sizeof *dent);
			if(n < sizeof *dent) return -EIO;
			n_read += n;
			int size = dent->size + 1;
			n = read_metadata(dent->name, &blk, &block, &offset, size);
			if(n < size) return -EIO;
			n_read += n;
			if(name[0] < dent->name[0]) goto done;	/* sorted order. */
			dent->name[size] = '\0';
			if(size == namelen && streq(dent->name, name)) {
				ino = SQUASHFS_MKINODE(hdr.start_block, dent->offset);
				*type = dent->type;
				goto done;
			}
		}
#if 0
		printf("%s: at end of loop, n_read=%d, file_size=%u\n",
			__func__, n_read, dir->file_size);
#endif
	}

done:
	return ino;
}


static void fs_initialize(void)
{
	assert(__builtin_popcount(MAX_FD + 1) == 1);
	fd_ra = RA_NEW(struct fd, MAX_FD + 1);
	ra_disable_id_0(fd_ra);
}


/* resolve file descriptor for current client. */
static struct fd *get_fd(L4_Word_t param_fd)
{
	if(unlikely(param_fd > MAX_FD)) return NULL;
	struct fd *fd = ra_id2ptr(fd_ra, param_fd & MAX_FD);
#if 0
	printf("%s: param_fd=%lu, fd=%p, ->file=%p, ->owner=%u, ->flags=%#x\n",
		__func__, param_fd, fd, fd->file, fd->owner, fd->flags);
#endif
	if(unlikely(fd->file == NULL)) return NULL;
	if(unlikely(fd->owner != pidof_NP(muidl_get_sender()))) return NULL;

	return fd;
}


/* FIXME: this doesn't handle trailing slashes very well. what should happen
 * in those cases? test them first.
 */
static int squashfs_openat(
	L4_Word_t *fd_p,
	L4_Word_t dirfd, const char *path, int32_t flags, uint32_t mode)
{
	// printf("squashfs: openat path=`%s'\n", path);

	if(dirfd != 0) return -ENOSYS;	/* FIXME */

	uint64_t dir_ino = 0;
	int64_t final_ino = -ENOENT;
	int type = -1;

	/* resolve @path one component at a time. */
	char comp[SQUASHFS_NAME_LEN + 1];
	while(*path != '\0') {
		char *slash = strchr(path, '/');
		const char *part;
		if(slash == NULL) {
			/* last part. */
			part = path;
			path += strlen(path);
		} else {
			/* piece in the middle. */
			int comp_len = slash - path;
			if(unlikely(comp_len > SQUASHFS_NAME_LEN)) return -ENOENT;
			else if(comp_len == 0) {
				/* FIXME: hit this in a test with double and triple and quadruple
				 * etc. slashes in a pathspec.
				 */
				assert(slash == path);
				path++;
				continue;
			}

			memcpy(comp, path, comp_len);
			comp[comp_len] = '\0';
			path = slash + 1;
			part = comp;
		}

		// printf("path component: `%s'\n", part);

		if(dir_ino == 0) {
			if(!streq(part, "initrd")) {
				/* first component must be initrd. */
				break;
			}
			dir_ino = fs_super->root_inode;
			assert(dir_ino != 0);
		} else {
			int64_t ino = lookup(&type, dir_ino, part);
			if(ino < 0) return ino;
			else if(type == SQUASHFS_DIR_TYPE) dir_ino = ino;
			else if(path[0] != '\0') {
				printf("%s: type=%d isn't a directory? but path=`%s'\n",
					__func__, type, path);
				return -ENOTDIR;
			} else {
				final_ino = ino;
				break;
			}
		}
	}

#if 0
	printf("%s: final_ino=%u, type=%d\n", __func__,
		(unsigned)final_ino, type);
#endif

	if(final_ino < 0) return final_ino;

	struct squashfs_file *f = malloc(sizeof *f);
	*f = (struct squashfs_file){
		.i = get_inode(final_ino), .pos = 0, .refs = 1,
	};
	struct fd *fd = ra_alloc(fd_ra, -1);
	*fd = (struct fd){ .owner = pidof_NP(muidl_get_sender()), .file = f };
#if 0
	printf("%s: allocated fd=%p (%d) for owner=%#x\n", __func__,
		fd, ra_ptr2id(fd_ra, fd), fd->owner);
#endif

	*fd_p = ra_ptr2id(fd_ra, fd);
	return 0;
}


static int squashfs_close(L4_Word_t param_fd)
{
	struct fd *fd = get_fd(param_fd);
	if(unlikely(fd == NULL)) return -EBADF;

	if(--fd->file->refs == 0) free(fd->file);
	fd->file = NULL;
	fd->owner = 0;
	ra_free(fd_ra, fd);

	return 0;
}


/* seek into block @target. so 0 for the first, etc. returns address of the
 * data block in question or negative errno. *@block and *@offset should start
 * at either the previous seek position, or a regular file's metadata start
 * and offset, and leave off after the next chunk of metadata.
 */
static int64_t seek_block_list(uint64_t *block, int *offset, int target)
{
#if 0
	printf("%s: block=%#x, offset=%d, target=%d\n", __func__,
		(unsigned)*block, *offset, target);
#endif
	target++;
	int64_t res = 0;
	uint32_t *lens = malloc(PAGE_SIZE);
	if(lens == NULL) return -ENOMEM;
	struct blk *metablk = NULL;
	while(target > 0) {
		int blocks = min_t(int, target, PAGE_SIZE / 4);
		int n = read_metadata(lens, &metablk, block, offset, blocks * 4);
		if(n != blocks * 4) {
			printf("%s: can't read metadata, n=%d\n", __func__, n);
			if(n > 0) n = -EIO;
			res = n;
			goto end;
		}
		for(int i=0; i < blocks; i++) {
			uint32_t word = LE32_TO_CPU(lens[i]);
#if 0
			printf("blockptr[%d]=%#x (%u, %u)\n", i, word,
				SQUASHFS_COMPRESSED_BLOCK(word),
				SQUASHFS_COMPRESSED_SIZE_BLOCK(word));
#endif
			res += SQUASHFS_COMPRESSED_SIZE_BLOCK(word);
		}
		target -= blocks;
	}

end:
	free(lens);
	return res;
}


static int squashfs_read(
	uint8_t *data_buf, unsigned *data_len_p,
	L4_Word_t param_fd, uint32_t read_pos, uint32_t count)
{
	int n = 0;

	struct fd *fd = get_fd(param_fd);
	if(fd == NULL) return -EBADF;

	struct inode *nod = fd->file->i;
	unsigned type = squashfs_i(nod)->X.base.inode_type;
	if(type == SQUASHFS_DIR_TYPE) return -EISDIR;
	else if(type != SQUASHFS_REG_TYPE) return -EBADF;

	struct squashfs_reg_inode *reg = &squashfs_i(nod)->X.reg;
	if(unlikely(reg->fragment != SQUASHFS_INVALID_FRAG)) {
		/* TODO: handle fragments, one day */
		printf("fs.squashfs: no fragment support\n");
		return -EIO;
	}

	uint32_t done = 0, pos = read_pos == ~0u ? fd->file->pos : read_pos,
		bytes = min_t(uint32_t, count, reg->file_size - pos);
	if(bytes == 0) goto end;

	uint64_t block = squashfs_i(nod)->rest_start;
	int offset = squashfs_i(nod)->offset,
		skip = (pos >> fs_block_size_log2) - 1;
	int64_t data_block = 0;
	if(skip > 0) {
		data_block = seek_block_list(&block, &offset, skip);
		if(data_block < 0) {
			n = data_block;
			goto end;
		}
	}
	data_block += reg->start_block;

	while(done < bytes) {
		uint32_t lenword;
		int n = read_metadata(&lenword, NULL, &block, &offset, sizeof lenword);
		if(n < 0) return n;
		lenword = LE32_TO_CPU(lenword);
		if(lenword == 0) continue;

		struct blk *b = cache_get(data_block, lenword);
		if(b == NULL) return -ENOMEM;	/* or translate an errptr */
		int seg = min_t(int, bytes - done, b->length);
		memcpy(&data_buf[done],
			&b->data[pos & ((1 << fs_block_size_log2) - 1)], seg);
		done += seg;
		pos += seg;
		data_block += SQUASHFS_COMPRESSED_SIZE_BLOCK(lenword);
	}
	n = 0;

end:
	if(read_pos == ~0u) fd->file->pos = pos;
	*data_len_p = done;
	// printf("%s: pos'=%u, done=%d, n=%d\n", __func__, pos, done, n);
	return n;
}


static void squashfs_ipc_loop(void *initrd_start, size_t initrd_size)
{
	static const struct squashfs_impl_vtable vtab = {
		.openat = &squashfs_openat,
		.close = &squashfs_close,
		.read = &squashfs_read,
	};
	for(;;) {
		L4_Word_t status = _muidl_squashfs_impl_dispatch(&vtab);
		if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("fs.squashfs: dispatch status=%#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}
}


static void dump_root_inode(void)
{
	struct inode *nod = get_inode(fs_super->root_inode);
	union squashfs_inode r = squashfs_i(nod)->X;
	if(r.base.inode_type != SQUASHFS_DIR_TYPE) {
		printf("root inode isn't a directory?\n");
		abort();
	}

	printf("root inode=%u:\n"
		"   type=%u, mode=%#x, uid=%u, guid=%u, mtime=%#x, inode_number=%u\n"
		"   start_block=%u, nlink=%u, file_size=%u, offset=%u, parent=%u\n",
		(unsigned)fs_super->root_inode, r.dir.inode_type, r.dir.mode,
		r.dir.uid, r.dir.guid, r.dir.mtime,
		r.dir.inode_number, r.dir.start_block,
		r.dir.nlink, r.dir.file_size,
		r.dir.offset, r.dir.parent_inode);
}


static unsigned mount_squashfs_image(void *start, size_t sz)
{
	fs_image = start;
	fs_length = sz;
	fs_super = start + SQUASHFS_START;
	fs_block_size_log2 = size_to_shift(fs_super->block_size);
	if(1 << fs_block_size_log2 != fs_super->block_size) {
		printf("fs.squashfs: block size is not power of two?\n");
		return EINVAL;
	}
	if(memcmp(&fs_super->s_magic, "hsqs", 4) != 0) {
		printf("invalid squashfs superblock magic number %#08x (BE)\n",
			(unsigned)BE32_TO_CPU(fs_super->s_magic));
		return EINVAL;
	}

	static const char *c_modes[] = {
		[ZLIB_COMPRESSION] = "zlib",
		[LZMA_COMPRESSION] = "lzma",
		[LZO_COMPRESSION] = "lzo",
		[XZ_COMPRESSION] = "xz",
		[LZ4_COMPRESSION] = "lz4",
		[ZSTD_COMPRESSION] = "zstd",
	};
	const char *c_mode = fs_super->compression < ARRAY_SIZE(c_modes)
		? c_modes[fs_super->compression] : NULL;
	if(c_mode == NULL) c_mode = "[unknown]";
	printf("squashfs superblock:\n"
		"   inodes=%u, block_size=%u, fragments=%u, compression=%s\n"
		"   block_log=%u, flags=%#x, no_ids=%u, s_major=%u, s_minor=%u\n"
		"   root_inode=%u, bytes_used=%u\n",
		fs_super->inodes, fs_super->block_size, fs_super->fragments, c_mode,
		fs_super->block_log, fs_super->flags, fs_super->no_ids,
		fs_super->s_major, fs_super->s_minor, (unsigned)fs_super->root_inode,
		(unsigned)fs_super->bytes_used);

	switch(fs_super->compression) {
		case LZ4_COMPRESSION:
			break;
		default:
			printf("can't handle compression mode %d (%s)\n",
				fs_super->compression, c_mode);
			return EINVAL;
	}

	dump_root_inode();

	return 0;
}


static void ignore_opt_error(const char *fmt, ...) {
	/* foo */
}


static char *set_tid(const char *optarg, void *tidptr)
{
	char *copy = strdup(optarg), *sep = strchr(copy, ':');
	*(sep++) = '\0';
	boot_tid = L4_GlobalId(atoi(copy), atoi(sep));
	return NULL;
}


static const struct opt_table opts[] = {
	OPT_WITH_ARG("--boot-initrd", &set_tid, NULL, &boot_tid,
		"start up in initrd mode (not for general consumption)"),
	OPT_ENDTABLE
};


int main(int argc, char *argv[])
{
	opt_register_table(opts, NULL);
	if(!opt_parse(&argc, argv, &ignore_opt_error)) {
		printf("fs.squashfs: option parsing failed!\n");
		return 1;
	}

	/* do init protocol. */
	const L4_Time_t timeout = L4_TimePeriod(20 * 1000);
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_MsgTag_t tag = L4_Receive_Timeout(boot_tid, timeout);
	if(L4_IpcFailed(tag)) goto ipcfail;
	L4_Word_t initrd_size; L4_StoreMR(1, &initrd_size);

	void *initrd_start = aligned_alloc(PAGE_SIZE, initrd_size);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
	L4_LoadMR(1, (L4_Word_t)initrd_start);
	L4_ThreadId_t dum;
	/* ReplyReceive_Timeout */
	tag = L4_Ipc(boot_tid, boot_tid, L4_Timeouts(L4_ZeroTime, timeout), &dum);
	if(L4_IpcFailed(tag)) goto ipcfail;

	int status = mount_squashfs_image(initrd_start, initrd_size);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
	L4_LoadMR(1, status);
	tag = L4_Reply(boot_tid);
	if(L4_IpcFailed(tag)) goto ipcfail;

	if(status == 0) {
		fs_initialize();
		squashfs_ipc_loop(initrd_start, initrd_size);
	}

	return status;

ipcfail:
	printf("fs.squashfs: initrd protocol fail, tag=%#lx, ec=%lu\n",
		tag.raw, L4_ErrorCode());
	return 1;
}
