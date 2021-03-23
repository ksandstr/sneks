
/* TODO:
 *   - client lifecycle tracking (fork, exec, exit)
 */

#define SQUASHFSIMPL_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
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
#include <ccan/siphash/siphash.h>
#include <ccan/minmax/minmax.h>
#include <ccan/hash/hash.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include <sneks/mm.h>
#include <sneks/hash.h>
#include <sneks/lz4.h>
#include <sneks/bitops.h>
#include <sneks/process.h>
#include <sneks/devcookie.h>
#include <sneks/api/path-defs.h>
#include <sneks/api/file-defs.h>
#include <sneks/api/io-defs.h>
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
struct blk {
	uint64_t block;
	unsigned length;
	uint64_t next_block;
	uint8_t data[];	/* [length] */
};


/* contained within a per-fs inode info structure. */
struct inode {
	unsigned long ino;	/* int64_hash() */
	/* TODO: add basic stat(2) output's fields here */
};


struct squashfs_file {
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


/* an entry in /dev/.device-nodes, representing a synthetic device node
 * because mksquashfs isn't as good as mkcramfs used to be.
 */
struct dev {
	char name[16];
	uint16_t major, minor;
	bool is_chr;
};


/* TODO: define ->flags. */
struct fd {
	struct squashfs_file *file;
	uint16_t owner, flags;
};


static size_t rehash_blk(const void *key, void *priv);
static size_t rehash_inode(const void *key, void *priv);
static size_t rehash_dev(const void *key, void *priv);
static size_t rehash_fd_owner(const void *key, void *priv);


static L4_ThreadId_t boot_tid, dev_tid;

static bool device_nodes_enabled;
static int64_t dev_ino = -1;

/* mount data */
static void *fs_image;
static size_t fs_length;
static int fs_block_size_log2;

static const struct squashfs_super_block *fs_super;
static struct htable blk_cache = HTABLE_INITIALIZER(
		blk_cache, &rehash_blk, NULL),
	inode_cache = HTABLE_INITIALIZER(inode_cache, &rehash_inode, NULL),
	device_nodes = HTABLE_INITIALIZER(device_nodes, &rehash_dev, NULL),
	fd_by_owner_hash = HTABLE_INITIALIZER(fd_by_owner_hash,
		&rehash_fd_owner, NULL);

static struct rangealloc *fd_ra;	/* <struct fd>, never 0. */

static const struct cookie_key device_cookie_key;


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


static size_t rehash_dev(const void *key, void *priv) {
	const struct dev *dev = key;
	return hash(dev->name, strlen(dev->name), 0);
}

static size_t rehash_fd_owner(const void *key, void *priv) {
	const struct fd *fd = key;
	return int_hash(fd->owner);
}

static bool cmp_dev_name(const void *cand, void *key) {
	const struct dev *dev = key;
	return streq(dev->name, (const char *)key);
}


static inline struct inode_ext *squashfs_i(struct inode *n) {
	return container_of(n, struct inode_ext, fs_inode);
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


/* resolve file descriptor for current client. */
static struct fd *get_fd(int param_fd)
{
	if(unlikely(param_fd > MAX_FD)) return NULL;
	struct fd *fd = ra_id2ptr(fd_ra, param_fd & MAX_FD);
#if 0
	printf("%s: param_fd=%d, fd=%p, ->file=%p, ->owner=%u, ->flags=%#x\n",
		__func__, param_fd & MAX_FD, fd, fd->file, fd->owner, fd->flags);
#endif
	if(unlikely(fd->file == NULL)) return NULL;
	int caller = pidof_NP(muidl_get_sender());
	if(unlikely(fd->owner != caller && caller < SNEKS_MIN_SYSID)) return NULL;

	return fd;
}


/* FIXME: this doesn't handle trailing slashes very well. what should happen
 * in those cases? test them first.
 */
static int squashfs_resolve(
	unsigned *object_ptr, L4_Word_t *server_ptr,
	int *ifmt_ptr, L4_Word_t *cookie_ptr,
	L4_Word_t dirfd, const char *path, int flags)
{
	if(dirfd != 0) return -ENOSYS;	/* TODO */

	uint64_t dir_ino = 0;
	int64_t final_ino = -ENOENT;
	int type = -1;
	L4_ThreadId_t server = L4_MyGlobalId();
	*cookie_ptr = 0;

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
				/* TODO: hit this in a test with double and triple and
				 * quadruple etc. slashes in a pathspec.
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

		if(dir_ino == 0) {
			if(!streq(part, "initrd")) {
				/* first component must be initrd. */
				break;
			}
			dir_ino = fs_super->root_inode;
			assert(dir_ino != 0);
		} else if(device_nodes_enabled && !L4_IsNilThread(dev_tid)
			&& dir_ino == dev_ino && !streq(part, ".device-nodes"))
		{
			size_t hash = hash(part, strlen(part), 0);
			struct dev *dev = htable_get(&device_nodes, hash,
				&cmp_dev_name, part);
			if(dev == NULL) {
				/* NOTE: should we support subdirectories of /dev? currently
				 * this does not.
				 */
				return -ENOENT;
			}
			printf("%s: found device node `%s': %c-%u-%u\n", __func__,
				dev->name, dev->is_chr ? 'c' : 'b', dev->major, dev->minor);
			server = dev_tid;
			final_ino = (dev->is_chr ? 0x80000000 : 0)
				| (dev->major & 0x7fff) << 15 | (dev->minor & 0x7fff);
			type = dev->is_chr ? SQUASHFS_CHRDEV_TYPE : SQUASHFS_BLKDEV_TYPE;
			*cookie_ptr = gen_cookie(&device_cookie_key, L4_SystemClock(),
				final_ino, pidof_NP(muidl_get_sender()));
			break;
		} else {
			int64_t ino = lookup(&type, dir_ino, part);
			if(ino < 0) return ino;
			else if(type == SQUASHFS_DIR_TYPE) dir_ino = ino;
			else if(path[0] != '\0') {
				printf("%s: type=%d isn't a directory? but path=`%s'\n",
					__func__, type, path);
				return -ENOTDIR;
			} else {
				/* TODO: use a different algorithm and a different cookie key
				 * here once actual access control comes about. filesystem
				 * cookies should have properties that cross-systask device
				 * cookies don't, such as a limited number of uses (per
				 * process), anti-bruteforcing measures (forced epoch steps on
				 * failure), and so forth.
				 */
				*cookie_ptr = gen_cookie(&device_cookie_key, L4_SystemClock(),
					ino, pidof_NP(muidl_get_sender()));
				final_ino = ino;
				break;
			}
		}
	}

	if(final_ino < 0) return final_ino;

	static uint16_t sfs_type_table[] = {
		[SQUASHFS_DIR_TYPE] = SNEKS_PATH_S_IFDIR,
		[SQUASHFS_REG_TYPE] = SNEKS_PATH_S_IFREG,
		[SQUASHFS_SYMLINK_TYPE] = SNEKS_PATH_S_IFLNK,
		[SQUASHFS_BLKDEV_TYPE] = SNEKS_PATH_S_IFBLK,
		[SQUASHFS_CHRDEV_TYPE] = SNEKS_PATH_S_IFCHR,
		[SQUASHFS_FIFO_TYPE] = SNEKS_PATH_S_IFIFO,
		[SQUASHFS_SOCKET_TYPE] = SNEKS_PATH_S_IFSOCK,
	};
	if(type < 0 || type >= ARRAY_SIZE(sfs_type_table)
		|| sfs_type_table[type] == 0)
	{
		printf("%s: unknown inode type=%d\n", __func__, type);
		return -ENOSYS;
	}

	*object_ptr = final_ino;
	*server_ptr = server.raw;
	*ifmt_ptr = sfs_type_table[type];

	return 0;
}


static int squashfs_open(int *handle_p,
	unsigned object, L4_Word_t cookie, int flags)
{
	/* TODO: same thing as the big comment above gen_cookie() call in
	 * squashfs_resolve(), for same reasons.
	 */
	if(!validate_cookie(cookie, &device_cookie_key, L4_SystemClock(),
		object, pidof_NP(muidl_get_sender())))
	{
		return -EINVAL;
	}

	struct inode *nod = get_inode(object);
	if(nod == NULL) return -EINVAL;

	struct squashfs_file *f = malloc(sizeof *f);
	*f = (struct squashfs_file){ .i = nod, .pos = 0, .refs = 1 };
	struct fd *fd = ra_alloc(fd_ra, -1);
	*fd = (struct fd){ .owner = pidof_NP(muidl_get_sender()), .file = f };
	bool ok = htable_add(&fd_by_owner_hash, rehash_fd_owner(fd, NULL), fd);
	if(!ok) {
		ra_free(fd_ra, fd);
		free(f);
		return -ENOMEM;
	}

	*handle_p = ra_ptr2id(fd_ra, fd);
	return 0;
}


static int squashfs_seek(int handle, int *offset_p, int whence)
{
	struct fd *fd = get_fd(handle);
	if(fd == NULL) return -EBADF;

	const struct squashfs_reg_inode *reg = &squashfs_i(fd->file->i)->X.reg;
	switch(whence) {
		default: return -EINVAL;
		case SNEKS_FILE_SEEK_CUR:
			if((*offset_p >= 0 && reg->file_size - fd->file->pos < *offset_p)
				|| (*offset_p < 0 && -*offset_p > fd->file->pos))
			{
				return -EINVAL;
			}
			assert((int64_t)fd->file->pos + *offset_p <= reg->file_size
				&& (int64_t)fd->file->pos + *offset_p >= 0);
			fd->file->pos += *offset_p;
			break;
		case SNEKS_FILE_SEEK_SET:
			if(*offset_p < 0 || *offset_p > reg->file_size) return -EINVAL;
			fd->file->pos = *offset_p;
			break;
		case SNEKS_FILE_SEEK_END:
			if(*offset_p < 0 || *offset_p > reg->file_size) return -EINVAL;
			fd->file->pos = reg->file_size - *offset_p;
			break;
	}

	*offset_p = fd->file->pos;
	return 0;
}


static int squashfs_close(int param_fd)
{
	struct fd *fd = get_fd(param_fd);
	if(unlikely(fd == NULL)) return -EBADF;

	bool ok = htable_del(&fd_by_owner_hash, rehash_fd_owner(fd, NULL), fd);
	if(!ok) {
		/* FIXME: make this an error once squashfs_openat() etc. go away. */
		printf("%s: param_fd=%d not in by_owner_hash?\n", __func__, param_fd);
	}
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


static ssize_t read_from_inode(
	struct inode *nod, void *data_buf, size_t length, size_t read_pos)
{
	unsigned type = squashfs_i(nod)->X.base.inode_type;
	if(type == SQUASHFS_DIR_TYPE) return -EISDIR;
	else if(type != SQUASHFS_REG_TYPE) return -EBADF;

	struct squashfs_reg_inode *reg = &squashfs_i(nod)->X.reg;
	if(unlikely(reg->fragment != SQUASHFS_INVALID_FRAG)) {
		/* TODO: handle fragments, one day */
		printf("fs.squashfs: no fragment support\n");
		return -EIO;
	}

	uint32_t done = 0, pos = read_pos,
		bytes = min_t(uint32_t, length, reg->file_size - pos);
	if(bytes == 0) return 0;

#if 0
	printf("%s: pos=%u, bytes=%u, blksizelog2=%d\n", __func__, pos, bytes,
		(int)fs_block_size_log2);
#endif

	uint64_t block = squashfs_i(nod)->rest_start;
	int offset = squashfs_i(nod)->offset,
		seek = pos >> fs_block_size_log2;
	int64_t data_block = 0;
	if(seek > 1) {
		data_block = seek_block_list(&block, &offset, seek - 1);
		if(data_block < 0) return data_block;
		seek = 0;
	}
	data_block += reg->start_block;
#if 0
	printf("\tpre-loop data_block=%lu, block=%u, offset=%u\n",
		(unsigned long)data_block, (unsigned)block, offset);
#endif

	while(done < bytes) {
		uint32_t lenword;
		int n = read_metadata(&lenword, NULL, &block, &offset, sizeof lenword);
		if(n < 0) return n;
		lenword = LE32_TO_CPU(lenword);
		if(lenword == 0) continue;
#if 0
		printf("\tloop data_block=%lu, block=%u, offset=%u, seek=%d\n",
			(unsigned long)data_block, (unsigned)block, offset, seek);
#endif

		if(seek-- <= 0) {
			struct blk *b = cache_get(data_block, lenword);
			if(b == NULL) return -ENOMEM;	/* or translate an errptr */
			int seg = min_t(int, bytes - done, b->length);
			memcpy(data_buf + done,
				&b->data[pos & ((1 << fs_block_size_log2) - 1)], seg);
			done += seg;
			pos += seg;
		}
		data_block += SQUASHFS_COMPRESSED_SIZE_BLOCK(lenword);
	}

	return done;
}


static int squashfs_read(int param_fd, int count, off_t offset,
	uint8_t *data_buf, unsigned *data_len_p)
{
	struct fd *fd = get_fd(param_fd);
	if(fd == NULL) return -EBADF;
	if(count < 0) return -EINVAL;
	if(count == 0) return 0;

	ssize_t n = read_from_inode(fd->file->i, data_buf, count,
		offset < 0 ? fd->file->pos : offset);
	if(n >= 0) {
		if(offset < 0) fd->file->pos += n;
		*data_len_p = n;
		n = 0;
	}
	return n;
}


static int squashfs_set_flags(int *old_flags_ptr,
	int fd, int or_mask, int and_mask)
{
	return -ENOSYS;
}


static int squashfs_write(int a, off_t b, const uint8_t *c, unsigned d) {
	/* how teh cookie doth crümbel. */
	return -EROFS;
}


static void squashfs_ipc_loop(void *initrd_start, size_t initrd_size)
{
	static const struct squashfs_impl_vtable vtab = {
		/* Sneks::IO */
		.close = &squashfs_close,
		.read = &squashfs_read,
		.set_flags = &squashfs_set_flags,
		.write = &squashfs_write,

		/* Sneks::Path */
		.resolve = &squashfs_resolve,

		/* Sneks::File */
		.open = &squashfs_open,
		.seek = &squashfs_seek,
	};
	for(;;) {
		L4_Word_t status = _muidl_squashfs_impl_dispatch(&vtab);
		if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			if(status == MUIDL_UNKNOWN_LABEL) {
				L4_ThreadId_t sender = muidl_get_sender();
				printf("fs.squashfs: unknown label %#lx from %lu:%lu\n",
					L4_Label(muidl_get_tag()),
					L4_ThreadNo(sender), L4_Version(sender));
				L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 1, .X.u = 1 }.raw);
				L4_LoadMR(1, ENOSYS);
				L4_Reply(sender);
			} else {
				printf("fs.squashfs: dispatch status=%#lx (last tag %#lx)\n",
					status, muidl_get_tag().raw);
			}
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


static void parse_device_node(char *line, int length)
{
	char *comment = strchr(line, '#');
	if(comment != NULL) *comment = '\0';

	const char *name = line;
	char *type = strchr(line, '\t');
	if(type == NULL) goto malformed;
	*type++ = '\0';
	char *major = strchr(type, '\t');
	if(major == NULL) goto malformed;
	*major++ = '\0';
	char *minor = strchr(major, '\t');
	if(minor == NULL) goto malformed;
	*minor++ = '\0';
	char *trail = minor + 1;
	while(*trail != '\0' && !isspace(*trail)) trail++;
	*trail = '\0';

	struct dev *dev = malloc(sizeof *dev);
	memset(dev->name, 0, sizeof dev->name);
	memcpy(dev->name, name, min(strlen(name), sizeof dev->name - 1));
	switch(type[0]) {
		case 'c': dev->is_chr = true; break;
		case 'b': dev->is_chr = false; break;
		default: goto malformed;
	}
	dev->major = strtol(major, NULL, 10);
	dev->minor = strtol(minor, NULL, 10);

#if 0
	printf("%s: name=`%s', type=`%s', major=`%s', minor=`%s'\n",
		__func__, name, type, major, minor);
	printf("%s\tparsed to name=`%s', is_chr=%s, major=%u, minor=%u\n",
		__func__, dev->name, dev->is_chr ? "true" : "false",
		dev->major, dev->minor);
#endif

	htable_add(&device_nodes, rehash_dev(dev, NULL), dev);
	return;

malformed:
	printf("fs.squashfs: malformed device-nodes entry `%s'\n", line);
	abort();
}


static void read_device_nodes(unsigned long ino)
{
	struct inode *nod = get_inode(ino);
	if(nod == NULL) abort();
	char buf[200];
	int pos = 0, done = 0, n;
	do {
		n = read_from_inode(nod, buf + pos, sizeof buf - pos - 1, done);
		if(n < 0) {
			printf("fs.squashfs: %s: read error n=%d\n", __func__, n);
			abort();
		}
		done += n;
		pos += n;
		buf[pos] = '\0';
		int len;
		char *lf = strchr(buf, '\n');
		if(lf == NULL) len = strlen(buf);
		else {
			*lf = '\0';
			len = lf - buf;
			assert(len == strlen(buf));
		}
		parse_device_node(buf, len);
		if(len == pos) pos = 0;
		else {
			memmove(buf, buf + len + 1, pos - len - 1);
			pos -= len + 1;
		}
	} while(n > 0 || pos > 0);
	printf("%s: have %d device nodes\n", __func__, (int)htable_count(&device_nodes));
}


static void fs_initialize(void)
{
	assert(__builtin_popcount(MAX_FD + 1) == 1);
	fd_ra = RA_NEW(struct fd, MAX_FD + 1);
	ra_disable_id_0(fd_ra);

	if(device_nodes_enabled) {
		int typ = 0;
		int64_t ino = lookup(&typ, fs_super->root_inode, "dev");
		if(ino >= 0 && typ == SQUASHFS_DIR_TYPE) {
			dev_ino = ino;
			ino = lookup(&typ, dev_ino, ".device-nodes");
			if(ino >= 0 && typ == SQUASHFS_REG_TYPE) {
				read_device_nodes(ino);
			}
		}
	}
}


static void ignore_opt_error(const char *fmt, ...) {
	/* foo */
}


static char *set_tid(const char *optarg, void *tidptr)
{
	assert(tidptr == &boot_tid || tidptr == &dev_tid);
	char *copy = strdup(optarg), *sep = strchr(copy, ':');
	*sep++ = '\0';
	*(L4_ThreadId_t *)tidptr = L4_GlobalId(atoi(copy), atoi(sep));
	free(copy);
	return NULL;
}


static char *decode_l64a_16u8(const char *optarg, void *dest)
{
	char input[41];
	strncpy(input, optarg, sizeof input);
	input[sizeof input - 1] = '\0';
	assert(streq(optarg, input));

	int ip = 0, op = 0;
	do {
		char *sep = strchr(&input[ip], ':');
		if(sep != NULL) *sep = '\0';
		unsigned val = a64l(&input[ip]);
		assert(streq(l64a(val), &input[ip]));
		if(sep != NULL) ip += sep - &input[ip] + 1;
		for(int i=0; i < 3 && op < 16; i++, op++, val >>= 8) {
			*(uint8_t *)(dest + op) = val & 0xff;
		}
	} while(op < 16);
	for(int i = op; i < 16; i++) *(uint8_t *)(dest + i) = '\0';

	return NULL;
}


static const struct opt_table opts[] = {
	OPT_WITH_ARG("--boot-initrd", &set_tid, NULL, &boot_tid,
		"start up in initrd mode (not for general consumption)"),
	OPT_WITH_ARG("--device-cookie-key", &decode_l64a_16u8, NULL,
		&device_cookie_key.key, "cookie key (base64)"),
	OPT_WITH_ARG("--device-registry-tid", &set_tid, NULL, &dev_tid,
		"tid:version of the system device registry"),
	OPT_ENDTABLE
};


int main(int argc, char *argv[])
{
	opt_register_table(opts, NULL);
	if(!opt_parse(&argc, argv, &ignore_opt_error)) {
		printf("fs.squashfs: option parsing failed!\n");
		return EXIT_FAILURE;
	}

	if(L4_IsNilThread(boot_tid)) {
		printf("fs.squashfs: no boot_tid?\n");
		/* TODO: support mounting an image from a filesystem, or a block
		 * device
		 */
		return EXIT_FAILURE;
	}

	/* do init protocol. */
	const L4_Time_t timeout = L4_TimePeriod(2 * 1000 * 1000);
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
		device_nodes_enabled = pidof_NP(boot_tid) >= SNEKS_MIN_SYSID;
		fs_initialize();
		squashfs_ipc_loop(initrd_start, initrd_size);
	}

	return status;

ipcfail:
	printf("fs.squashfs: initrd protocol fail, tag=%#lx, ec=%lu\n",
		tag.raw, L4_ErrorCode());
	return EXIT_FAILURE;
}
