
#define SQUASHFSIMPL_IMPL_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <alloca.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ccan/opt/opt.h>
#include <ccan/endian/endian.h>
#include <ccan/array_size/array_size.h>
#include <ccan/likely/likely.h>
#include <ccan/str/str.h>
#include <ccan/htable/htable.h>
#include <ccan/container_of/container_of.h>
#include <ccan/minmax/minmax.h>
#include <ccan/hash/hash.h>
#include <ccan/darray/darray.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include <sneks/mm.h>
#include <sneks/hash.h>
#include <sneks/lz4.h>
#include <sneks/bitops.h>
#include <sneks/process.h>
#include <sneks/systask.h>
#include <sneks/devcookie.h>
#include <sneks/rollback.h>
#include <sneks/io.h>
#include <sneks/api/path-defs.h>
#include <sneks/api/file-defs.h>
#include <sneks/api/directory-defs.h>
#include <sneks/api/io-defs.h>

#include "muidl.h"
#include "defs.h"
#include "squashfs_fs.h"
#include "squashfs-impl-defs.h"


#define CALLER_PID pidof_NP(muidl_get_sender())


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
	ino_t ino;	/* int_hash() */
	/* TODO: add basic stat(2) output's fields here */
	union {
		struct {
			int max_index;
		} dir;
	};
};


/* directory entry. indexed in dentry_index_hash by dir_ino and index, and
 * dentry_name_hash by dir_ino and name.
 *
 * TODO: it's reasonable to handle these in a cache and replace them as some
 * space estimation high watermark is hit. that'll be unlikely to happen until
 * libsneks-fs.a rules the skies.
 */
struct dentry {
	ino_t ino, dir_ino;
	uint32_t index;
	uint8_t type;	/* SNEKS_DIRECTORY_DT_* */
	unsigned short name_len; /* up to SQUASHFS_NAME_LEN */
	char name[]; /* null terminated for comfort */
};


struct io_file_impl
{
	struct inode *i;
	size_t pos;
	int refs;
	union {
		struct {
			/* bits for reading the next directory entry, for speeding
			 * getdents() up a tad.
			 */
			struct squashfs_dir_header hdr;
			uint64_t block;
			int offset, cur_index, cur_hdr, last_fetch;
			size_t bytes_read;	/* increases towards ->file_size */
		} dir;
	};
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


static size_t rehash_blk(const void *key, void *priv);
static size_t rehash_inode(const void *key, void *priv);
static size_t rehash_dev(const void *key, void *priv);
static size_t rehash_dentry_by_index(const void *key, void *priv);
static size_t rehash_dentry_by_dir_ino_and_name(const void *key, void *priv);
static size_t rehash_dentry_by_ino(const void *key, void *priv);
static void rollback_open(L4_Word_t x, iof_t *f);


static char sfs_type_table[] = {
	[SQUASHFS_DIR_TYPE] = DT_DIR,
	[SQUASHFS_REG_TYPE] = DT_REG,
	[SQUASHFS_SYMLINK_TYPE] = DT_LNK,
	[SQUASHFS_BLKDEV_TYPE] = DT_BLK,
	[SQUASHFS_CHRDEV_TYPE] = DT_CHR,
	[SQUASHFS_FIFO_TYPE] = DT_FIFO,
	[SQUASHFS_SOCKET_TYPE] = DT_SOCK,
};

static L4_ThreadId_t boot_tid, dev_tid;

static bool device_nodes_enabled;
static ino_t dev_ino = -1;

/* mount data */
static void *fs_image;
static size_t fs_length;
static int fs_block_size_log2;

static const struct squashfs_super_block *fs_super;
static struct htable blk_cache = HTABLE_INITIALIZER(
		blk_cache, &rehash_blk, NULL),
	inode_cache = HTABLE_INITIALIZER(inode_cache, &rehash_inode, NULL),
	device_nodes = HTABLE_INITIALIZER(device_nodes, &rehash_dev, NULL),
	dentry_index_hash = HTABLE_INITIALIZER(dentry_index_hash,
		&rehash_dentry_by_index, NULL),
	dentry_name_hash = HTABLE_INITIALIZER(dentry_name_hash,
		&rehash_dentry_by_dir_ino_and_name, NULL),
	dentry_ino_hash = HTABLE_INITIALIZER(dentry_ino_hash,
		&rehash_dentry_by_ino, NULL);

/* ~0u for "not yet seen" */
static unsigned *ino_to_blkoffset = NULL;	/* [fs_super->inodes + 1] */
static ino_t root_ino;

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
	return int_hash(i->ino);
}

static bool cmp_inode_ino(const void *cand, void *key) {
	const struct inode *i = cand;
	return i->ino == *(const ino_t *)key;
}


static size_t rehash_dev(const void *key, void *priv) {
	const struct dev *dev = key;
	return hash_string(dev->name);
}

static bool cmp_dev_name(const void *cand, void *key) {
	const struct dev *dev = key;
	return streq(dev->name, (const char *)key);
}


static size_t rehash_dentry_by_index(const void *key, void *priv) {
	const struct dentry *d = key;
	return int_hash(d->dir_ino) ^ int_hash(d->index);
}

static size_t rehash_dentry_by_dir_ino_and_name(const void *key, void *priv) {
	const struct dentry *d = key;
	return int_hash(d->dir_ino) ^ hash_string(d->name);
}

static size_t rehash_dentry_by_ino(const void *key, void *priv) {
	const struct dentry *d = key;
	return int_hash(d->ino);
}


static bool cmp_dentry_by_ino(const void *cand, void *key) {
	const struct dentry *dent = cand;
	return dent->ino == *(ino_t *)key;
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
			/* FIXME: return an error code and fail the caller */
			log_crit("LZ4 decompression failed, n=%d", n);
			abort();
		}
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

	return b;
}


/* read @length bytes of metadata from *@block:*@offset. updates *@block and
 * *@offset to reflect the next position. to optimize lookups, pass a
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


static struct inode *read_inode(unsigned blkoffs)
{
	assert(!SQUASHFS_UNCOMPRESSED_INODES(fs_super->flags));

	struct inode_ext *nod_ext = malloc(sizeof *nod_ext);
	if(nod_ext == NULL) return NULL;	/* TODO: pop ENOMEM */
	struct inode *nod = &nod_ext->fs_inode;
	/* TODO: switch according to inode type once there's more than one thing
	 * in the union.
	 */
	nod->dir.max_index = INT_MAX;

	uint64_t block = fs_super->inode_table_start + SQUASHFS_INODE_BLK(blkoffs);
	int offset = SQUASHFS_INODE_OFFSET(blkoffs);
	struct squashfs_base_inode *base = &squashfs_i(nod)->X.base;
	struct blk *blk = NULL;
	int n = read_metadata(base, &blk, &block, &offset, sizeof *base);
	if(n < sizeof *base) goto Eio;

	int sz = inode_size(base->inode_type);
	if(sz > 0) {
		n = read_metadata(base + 1, &blk, &block, &offset, sz - sizeof *base);
		if(n < sz - sizeof *base) goto Eio;
		squashfs_i(nod)->rest_start = block;
		squashfs_i(nod)->offset = offset;
	} else {
		log_err("unknown inode type %d", (int)base->inode_type);
		free(nod);
		return NULL;	/* TODO: -EINVAL */
	}

	return nod;

Eio:
	free(nod);
	return NULL;		/* TODO: -EIO */
}


/* fetch @ino from cache, or read it from the filesystem, or fail. */
static struct inode *get_inode(ino_t ino)
{
	size_t hash = int_hash(ino);
	struct inode *nod = htable_get(&inode_cache, hash, &cmp_inode_ino, &ino);
	if(nod == NULL) {
		if(ino > fs_super->inodes) {
			log_err("ino=%u out of range (max=%u)", ino, fs_super->inodes);
			return NULL;
		} else if(ino_to_blkoffset[ino] == ~0u) {
			log_crit("location of ino=%u not known", ino);
			return NULL;
		}
		nod = read_inode(ino_to_blkoffset[ino]);
		if(nod == NULL) return NULL;	/* TODO: propagate w/ IS_ERR() */
		nod->ino = ino;
		bool ok = htable_add(&inode_cache, hash, nod);
		if(!ok) {
			free(nod);
			return NULL;	/* TODO: ENOMEM */
		}
	}

	return nod;
}


static void rewind_directory(iof_t *file,
	const struct squashfs_dir_inode *dir)
{
	/* TODO: handle LDIRs as well? (that dir_index stuff.) */
	assert(dir->inode_type == SQUASHFS_DIR_TYPE);

	file->dir.cur_index = -1;
	file->dir.bytes_read = 0;
	file->dir.hdr.count = 0;
	file->dir.cur_hdr = 1;
	file->dir.last_fetch = 0;
	file->dir.block = fs_super->directory_table_start + dir->start_block;
	file->dir.offset = dir->offset;
}


static struct dentry *read_dentry(iof_t *file,
	struct blk **blk_p, int index, int *err_p)
{
	assert(index >= 0);

	/* synthesize "." and ".." */
	if(index < 2) {
		struct dentry *out = malloc(sizeof *out + index + 2);
		if(out == NULL) {
			*err_p = -ENOMEM;
			return NULL;
		}
		*out = (struct dentry){
			.dir_ino = file->i->ino, .type = SNEKS_DIRECTORY_DT_DIR,
			.ino = index == 0 ? file->i->ino
				: squashfs_i(file->i)->X.dir.parent_inode,
			.name_len = index + 1,
			.index = index,
		};
		if(out->dir_ino == root_ino && out->ino == fs_super->inodes + 1) {
			/* special bodge for ".." on the root directory, which works silly
			 * like this
			 */
			out->ino = root_ino;
		}
		out->name[0] = '.';
		out->name[index] = '.';
		out->name[index + 1] = '\0';
		assert(strlen(out->name) == out->name_len);
		return out;
	} else {
		/* read actual directory contents */
		index -= 2;
	}

	if(index > file->i->dir.max_index) {
		return NULL; /* previously found EOD */
	}

	struct squashfs_dir_inode *dir = &squashfs_i(file->i)->X.dir;
	/* TODO: handle LDIRs as well. (that dir_index stuff.) */
	assert(dir->inode_type == SQUASHFS_DIR_TYPE);

	if(index <= file->dir.cur_index) {
		/* redo from start */
		rewind_directory(file, dir);
	}

	struct squashfs_dir_entry *dent = alloca(sizeof *dent
		+ SQUASHFS_NAME_LEN + 1);
	int name_len = 0;
	/* TODO: the dir->file_size condition is an inexact hacky bodge that'll
	 * break when the slop gets great enough. figure out what bytes this isn't
	 * accounting for by staring at the Linux squashfs source.
	 */
	while(file->dir.cur_index < index
		&& file->dir.bytes_read < dir->file_size - sizeof *dent)
	{
		if(file->dir.cur_hdr == file->dir.hdr.count + 1) {
			int n = read_metadata(&file->dir.hdr, blk_p,
				&file->dir.block, &file->dir.offset, sizeof file->dir.hdr);
			if(n < sizeof file->dir.hdr) goto Eio;
			file->dir.bytes_read += n;
			file->dir.cur_hdr = 0;
		}

		int n = read_metadata(dent, blk_p, &file->dir.block,
			&file->dir.offset, sizeof *dent);
		if(n < sizeof *dent) goto Eio;
		file->dir.bytes_read += n;

		/* stash the blkoffset now that we've seen it. */
		ino_t ino = file->dir.hdr.inode_number + dent->inode_number;
		if(ino > fs_super->inodes) {
			log_crit("invalid ino=%u (max=%u)", ino, fs_super->inodes);
			abort();
		} else if(ino_to_blkoffset[ino] == ~0u) {
			ino_to_blkoffset[ino] = SQUASHFS_MKINODE(
				file->dir.hdr.start_block, dent->offset);
		}
		assert(ino_to_blkoffset[ino] != ~0u);
		assert(ino_to_blkoffset[ino] == SQUASHFS_MKINODE(
			file->dir.hdr.start_block, dent->offset));

		name_len = dent->size + 1;
		if(name_len > SQUASHFS_NAME_LEN) goto Eio;
		n = read_metadata(dent->name, blk_p, &file->dir.block,
			&file->dir.offset, name_len);
		if(n < name_len) goto Eio;
		file->dir.bytes_read += n;
		dent->name[name_len] = '\0';
		file->dir.cur_index++;
		file->dir.cur_hdr++;
	}
	/* TODO: file_size condition needs to get exact, see comment above the
	 * while loop
	 */
	if(file->dir.bytes_read >= dir->file_size - sizeof *dent
		&& file->dir.cur_index < index)
	{
		/* end of directory. */
		file->i->dir.max_index = file->dir.cur_index;
		*err_p = 0;
		return NULL;
	}

	assert(name_len > 0);
	struct dentry *out = malloc(sizeof *out + name_len + 1);
	if(out == NULL) {
		*err_p = -ENOMEM;
		goto Error;
	}
	*out = (struct dentry){
		.dir_ino = file->i->ino,
		.ino = file->dir.hdr.inode_number + dent->inode_number,
		.type = dent->type >= ARRAY_SIZE(sfs_type_table) ? SNEKS_DIRECTORY_DT_UNKNOWN
			: sfs_type_table[dent->type],
		.name_len = name_len,
		.index = index + 2,
	};
	memcpy(out->name, dent->name, name_len);
	out->name[name_len] = '\0';
	assert(strlen(out->name) == out->name_len);
	return out;

Eio: *err_p = -EIO;
Error:
	file->dir.cur_index = INT_MAX;
	assert(*err_p < 0);
	return NULL;
}


static struct dentry *get_dentry(iof_t *file,
	struct blk **blk_p, int index, int *err_p)
{
	struct dentry key = { .dir_ino = file->i->ino, .index = index };
	size_t hash = rehash_dentry_by_index(&key, NULL);
	struct htable_iter it;
	for(struct dentry *cand = htable_firstval(&dentry_index_hash, &it, hash);
		cand != NULL; cand = htable_nextval(&dentry_index_hash, &it, hash))
	{
		if(cand->dir_ino == key.dir_ino && cand->index == key.index) return cand;
	}

	struct dentry *dent = read_dentry(file, blk_p, index, err_p);
	if(dent != NULL) {
		assert(hash == rehash_dentry_by_index(dent, NULL));
		bool ok = htable_add(&dentry_index_hash, hash, dent);
		ok = ok && htable_add(&dentry_name_hash, rehash_dentry_by_dir_ino_and_name(dent, NULL), dent);
		if(ok && index >= 2) {
			/* only add non-synthetic directories so that get_path doesn't get
			 * confused.
			 */
			ok = htable_add(&dentry_ino_hash, rehash_dentry_by_ino(dent, NULL), dent);
		} else {
			assert(!ok || streq(dent->name, ".") || streq(dent->name, ".."));
		}
		if(!ok) {
			*err_p = -ENOMEM;
			htable_del(&dentry_index_hash, hash, dent);
			htable_del(&dentry_name_hash, rehash_dentry_by_dir_ino_and_name(dent, NULL), dent);
			free(dent);
			dent = NULL;
		}
	}

	return dent;
}


static struct dentry *find_dentry(ino_t dir_ino, const char *name)
{
	size_t hash = int_hash(dir_ino) ^ hash_string(name);
	struct htable_iter it;
	for(struct dentry *cand = htable_firstval(&dentry_name_hash, &it, hash);
		cand != NULL; cand = htable_nextval(&dentry_name_hash, &it, hash))
	{
		if(cand->dir_ino == dir_ino && streq(name, cand->name)) {
			return cand;
		}
	}
	return NULL;
}


static int lookup(int *type, ino_t dir_ino, const char *name)
{
	assert(type != NULL);
	assert(name[0] != '\0');

	struct dentry *dent = find_dentry(dir_ino, name);
	if(dent != NULL) {
		*type = dent->type;
		return dent->ino;
	}

	struct inode *nod = get_inode(dir_ino);
	if(nod == NULL) return -EIO;	/* TODO: proper status return */

	struct squashfs_dir_inode *dir = &squashfs_i(nod)->X.dir;
	iof_t fake = { .i = nod, .pos = 0, .refs = 1 };
	rewind_directory(&fake, dir);
	struct blk *blk = NULL;
	int dix = 0, n = 0;
	while(dent = get_dentry(&fake, &blk, dix++, &n), dent != NULL) {
		int cmp = strcmp(name, dent->name);
		if(cmp < 0 && dent->index >= 2) {
			/* names after dix={0,1} are in strcmp() order allowing early
			 * return. but we must account for the first two entries are "."
			 * and ".." which might not be.
			 */
			break;
		}
		if(cmp == 0) {
			*type = dent->type;
			return dent->ino;
		}
	}

	return n == 0 ? -ENOENT : n;
}


/* FIXME: this doesn't handle trailing slashes well at all. */
static int squashfs_resolve(
	unsigned *object_ptr, L4_Word_t *server_ptr,
	int *ifmt_ptr, L4_Word_t *cookie_ptr,
	int dirfd, const char *path, int flags)
{
	sync_confirm();

	pid_t caller_pid = CALLER_PID;
	ino_t dir_ino;
	if(dirfd < 0) return -EBADF;
	else if(dirfd == 0) dir_ino = root_ino;
	else {
		iof_t *df = io_get_file(caller_pid, dirfd);
		if(df == NULL) return -EBADF;
		dir_ino = df->i->ino;
	}

	/* resolve @path one component at a time. */
	if(path[0] == '/') return -EINVAL;
	int type;
	ino_t final_ino;
	L4_ThreadId_t server;
	for(;;) {
		assert(*path != '\0');
		char *slash = strchr(path, '/'), comp[SQUASHFS_NAME_LEN + 1];
		const bool is_last = (slash == NULL);
		const char *part;
		if(is_last) {
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

		if(is_last && device_nodes_enabled && !L4_IsNilThread(dev_tid)
			&& dir_ino == dev_ino && !streq(part, ".device-nodes"))
		{
			struct dev *dev = htable_get(&device_nodes, hash_string(part),
				&cmp_dev_name, part);
			if(dev == NULL) {
				/* NOTE: should we support subdirectories of /dev? currently
				 * this does not.
				 */
				return -ENOENT;
			}
			server = dev_tid;
			final_ino = (dev->is_chr ? 0x80000000 : 0)
				| (dev->major & 0x7fff) << 15 | (dev->minor & 0x7fff);
			type = dev->is_chr ? DT_CHR : DT_BLK;
			break;
		} else {
			int ino = lookup(&type, dir_ino, part);
			if(ino < 0) return ino;
			if(!is_last) {
				if(type != DT_DIR) return -ENOTDIR;
				dir_ino = ino;
			} else {
				if((flags & O_DIRECTORY) && type != DT_DIR) {
					/* only directories, please. */
					return -ENOTDIR;
				}
				/* success! */
				final_ino = ino;
				server = L4_Myself();
				break;
			}
		}
	}

	if(true || type == DT_CHR || type == DT_BLK) {
		*cookie_ptr = gen_cookie(&device_cookie_key, L4_SystemClock(),
			final_ino, caller_pid);
	} else {
		/* TODO: use a different algorithm and a different cookie key for
		 * non-devices here once actual access control comes about. filesystem
		 * cookies should have properties that cross-systask device cookies
		 * don't, such as a limited number of uses per process,
		 * anti-bruteforcing, etc.
		 */
		*cookie_ptr = 0xdeadbeef;	/* plainly improper */
	}

	*object_ptr = final_ino;
	*server_ptr = server.raw;
	*ifmt_ptr = type << 12;	/* deliberate correspondence to S_IFMT */

	return 0;
}


/* TODO: catch ENOMEM from darray_push() */
static int squashfs_get_path_fragment(
	char *path, unsigned *obj_p, L4_Word_t *server_p, L4_Word_t *cookie_p,
	int fd)
{
	sync_confirm();

	pid_t caller = CALLER_PID;
	ino_t ino;
	if(fd > 0) {
		iof_t *file = io_get_file(caller, fd);
		if(file == NULL) return -EBADF;
		ino = file->i->ino;
	} else {
		/* TODO: use the fancier cookie validation algorithm, and not this one
		 * for device cookies, as detailed in squashfs_resolve().
		 */
		if(!validate_cookie(*cookie_p, &device_cookie_key,
			L4_SystemClock(), *obj_p, caller))
		{
			return -EINVAL;
		}
		ino = *obj_p;
	}

	*server_p = L4_nilthread.raw;	/* TODO: fall out to parent */
	darray(struct dentry *) parts = darray_new();
	int n, total_length = 0;
	while(ino != root_ino) {
		struct dentry *dent = htable_get(&dentry_ino_hash, int_hash(ino),
			&cmp_dentry_by_ino, &ino);
		if(dent == NULL) {
			log_err("dentry for ino=%u not found", ino);
			n = -ENOENT;
			goto end;
		}
		/* TODO: perform access checks on dent->dir_ino */
		int part_len = dent->name_len + (parts.size > 0 ? 1 : 0);
		if(total_length + part_len + 1 >= SNEKS_PATH_PATH_MAX) {
			*server_p = L4_Myself().raw;
			*obj_p = ino;
			/* TODO: use a fancier cookie algorithm, per above */
			*cookie_p = gen_cookie(&device_cookie_key, L4_SystemClock(),
				*obj_p, caller);
			break;
		}
		darray_push(parts, dent);
		total_length += part_len;
		ino = dent->dir_ino;
	}

	int pos = 0;
	struct dentry **i;
	darray_foreach_reverse(i, parts) {
		if(pos > 0) path[pos++] = '/';
		int len = strlen((*i)->name);
		assert(pos + len + 1 < SNEKS_PATH_PATH_MAX);
		memcpy(path + pos, (*i)->name, len);
		pos += len;
	}
	assert(pos < SNEKS_PATH_PATH_MAX);
	path[pos++] = '\0';
	n = 0;

end:
	darray_free(parts);
	return n;
}


static int squashfs_get_path(char *path, int fd, const char *suffix)
{
	sync_confirm();

	L4_Word_t server = 0xdeadbeef;
	int n = squashfs_get_path_fragment(path, &(unsigned){ 0 }, &server,
		&(L4_Word_t){ 0 }, fd);
	if(n != 0) return n;
	if(server == L4_MyGlobalId().raw) {
		/* our part was too long to fit in one segment. */
		return -ENAMETOOLONG;
	}

	size_t suffixlen = strlen(suffix);
	if(suffixlen > 0) {
		assert(suffix[0] != '/');
		size_t pathlen = strlen(path);
		if(pathlen + suffixlen + 1 >= SNEKS_PATH_PATH_MAX) return -ENAMETOOLONG;
		else {
			path[pathlen++] = '/';
			memcpy(path + pathlen, suffix, suffixlen);
			path[pathlen + suffixlen] = '\0';
		}
	}

	if(server != L4_nilthread.raw) {
		/* TODO: propagate @path as suffix to parent filesystem */
	}

	return 0;
}


static int squashfs_open(int *handle_p,
	unsigned object, L4_Word_t cookie, int flags)
{
	sync_confirm();

	pid_t caller_pid = CALLER_PID;
	/* TODO: same thing as the big comment above gen_cookie() call in
	 * squashfs_resolve() and squashfs_opendir(), for same reasons.
	 */
	if(!validate_cookie(cookie, &device_cookie_key, L4_SystemClock(),
		object, caller_pid))
	{
		return -EINVAL;
	}

	struct inode *nod = get_inode(object);
	if(nod == NULL) return -EINVAL;
	if(squashfs_i(nod)->X.base.inode_type != SQUASHFS_REG_TYPE) return -EINVAL;

	/* TODO: translate O_CLOEXEC to IOD_CLOEXEC for io_add_fd() */
	iof_t *f = iof_new(0);
	if(f == NULL) return -ENOMEM;
	*f = (iof_t){ .i = nod, .pos = 0, .refs = 1 };
	int n = io_add_fd(caller_pid, f, 0);
	if(n < 0) {
		iof_undo_new(f);
		return n;
	} else {
		set_rollback(&rollback_open, 0, f);
		*handle_p = n;
		return 0;
	}
}


static int squashfs_seek(int handle, int *offset_p, int whence)
{
	sync_confirm();

	iof_t *file = io_get_file(CALLER_PID, handle);
	if(file == NULL) return -EBADF;
	if(squashfs_i(file->i)->X.base.inode_type != SQUASHFS_REG_TYPE) {
		return -EINVAL;
	}

	const struct squashfs_reg_inode *reg = &squashfs_i(file->i)->X.reg;
	switch(whence) {
		default: return -EINVAL;
		case SNEKS_FILE_SEEK_CUR:
			if((*offset_p >= 0 && reg->file_size - file->pos < *offset_p)
				|| (*offset_p < 0 && -*offset_p > file->pos))
			{
				return -EINVAL;
			}
			assert((int64_t)file->pos + *offset_p <= reg->file_size
				&& (int64_t)file->pos + *offset_p >= 0);
			file->pos += *offset_p;
			break;
		case SNEKS_FILE_SEEK_SET:
			if(*offset_p < 0 || *offset_p > reg->file_size) return -EINVAL;
			file->pos = *offset_p;
			break;
		case SNEKS_FILE_SEEK_END:
			if(*offset_p < 0 || *offset_p > reg->file_size) return -EINVAL;
			file->pos = reg->file_size - *offset_p;
			break;
	}

	*offset_p = file->pos;
	return 0;
}


static int squashfs_io_close(iof_t *file)
{
	/* TODO: drop inode/dentry references */
	return 0;
}


static int squashfs_io_stat(iof_t *file, IO_STAT *st)
{
	struct squashfs_base_inode *base = &squashfs_i(file->i)->X.base;
	int access_bits = base->mode & 0777, t = base->inode_type,
		type = t < ARRAY_SIZE(sfs_type_table) ? sfs_type_table[t] << 12 : 0;
	*st = (IO_STAT){
		.st_mode = access_bits | type,
		/* TODO: the rest of the owl */
	};
	return 0;
}


/* NOTE: this is used from both squashfs_open() and squashfs_opendir() because
 * squashfs_io_close() is equally valid for results of both.
 */
static void rollback_open(L4_Word_t x, iof_t *f) {
	squashfs_io_close(f);
	iof_undo_new(f);
}


/* seek into block @target. so 0 for the first, etc. returns address of the
 * data block in question or negative errno. *@block and *@offset should start
 * at either the previous seek position, or a regular file's metadata start
 * and offset, and leave off after the next chunk of metadata.
 */
static int64_t seek_block_list(uint64_t *block, int *offset, int target)
{
	target++;
	int64_t res = 0;
	uint32_t *lens = malloc(PAGE_SIZE);
	if(lens == NULL) return -ENOMEM;
	struct blk *metablk = NULL;
	while(target > 0) {
		int blocks = min_t(int, target, PAGE_SIZE / 4);
		int n = read_metadata(lens, &metablk, block, offset, blocks * 4);
		if(n != blocks * 4) {
			log_err("can't read metadata, n=%d", n);
			if(n > 0) n = -EIO;
			res = n;
			goto end;
		}
		for(int i=0; i < blocks; i++) {
			uint32_t word = LE32_TO_CPU(lens[i]);
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
		log_err("no fragment support");
		return -EIO;
	}

	uint32_t done = 0, pos = read_pos,
		bytes = min_t(uint32_t, length, reg->file_size - pos);
	if(bytes == 0) return 0;

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

	while(done < bytes) {
		uint32_t lenword;
		int n = read_metadata(&lenword, NULL, &block, &offset, sizeof lenword);
		if(n < 0) return n;
		lenword = LE32_TO_CPU(lenword);
		if(lenword == 0) continue;

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


static void squashfs_wr_confirm(iof_t *file,
	unsigned count, off_t offset, bool writing)
{
	if(offset < 0) file->pos += count;
}


static int squashfs_io_read(iof_t *file,
	uint8_t *data_buf, unsigned count, off_t offset)
{
	if(count == 0) return 0;

	return read_from_inode(file->i, data_buf, count,
		offset < 0 ? file->pos : offset);
}


static int squashfs_io_write(iof_t *a, const uint8_t *b, unsigned c, off_t d) {
	/* how teh cookie doth crümbel. */
	return -EROFS;
}


static int squashfs_opendir(int *handle_p,
	unsigned object, L4_Word_t cookie, int flags)
{
	sync_confirm();

	pid_t caller_pid = CALLER_PID;
	/* TODO: same thing as the big comment above gen_cookie() call in
	 * squashfs_resolve() and squashfs_open(), for same reasons.
	 */
	if(!validate_cookie(cookie, &device_cookie_key, L4_SystemClock(),
		object, caller_pid))
	{
		return -EINVAL;
	}

	struct inode *nod = get_inode(object);
	if(nod == NULL) return -EINVAL;
	if(squashfs_i(nod)->X.base.inode_type != SQUASHFS_DIR_TYPE) {
		return -ENOTDIR;
	}
	const struct squashfs_dir_inode *dir = &squashfs_i(nod)->X.dir;

	/* FIXME: are @flags for file, or descriptor? */
	iof_t *f = iof_new(0);
	if(f == NULL) return -ENOMEM;
	*f = (iof_t){ .i = nod, .pos = 0, .refs = 1 };
	rewind_directory(f, dir);

	int fd = io_add_fd(caller_pid, f, 0);
	if(fd < 0) {
		iof_undo_new(f);
		return fd;
	} else {
		set_rollback(&rollback_open, 0, f);
		*handle_p = fd;
		return 0;
	}
}


static void confirm_seekdir(L4_Word_t newpos, iof_t *file) {
	file->pos = newpos;
	file->dir.last_fetch = 0;
}


static int squashfs_seekdir(int dirfd, int *position_ptr)
{
	sync_confirm();

	iof_t *file = io_get_file(CALLER_PID, dirfd);
	if(file == NULL) return -EBADF;
	/* (what a mouthful!) */
	if(squashfs_i(file->i)->X.base.inode_type != SQUASHFS_DIR_TYPE) {
		return -EBADF;
	}

	set_confirm(&confirm_seekdir, max(*position_ptr, 0), file);
	*position_ptr = file->pos;
	return 0;
}


static void rollback_getdents(L4_Word_t last_pos, iof_t *file) {
	assert(squashfs_i(file->i)->X.base.inode_type == SQUASHFS_DIR_TYPE);
	rewind_directory(file, &squashfs_i(file->i)->X.dir);
	file->pos = last_pos;
}


static int squashfs_getdents(int dirfd, int *offset_ptr, int *endpos_ptr,
	uint8_t *data_buf, unsigned *data_len_p)
{
	sync_confirm();

	iof_t *file = io_get_file(CALLER_PID, dirfd);
	if(file == NULL) return -EBADF;
	if(squashfs_i(file->i)->X.base.inode_type != SQUASHFS_DIR_TYPE) {
		return -EBADF;
	}

	int dix = *offset_ptr >= 0 ? *offset_ptr : file->pos,
		n = 0, got = 0, buf_pos = 0,
		limit = min(USHRT_MAX, max(2, file->dir.last_fetch * 2));
	*offset_ptr = file->pos;
	const struct dentry *dent;
	struct sneks_directory_dentry *out;
	struct blk *blk = NULL;
	while(got < limit
		&& buf_pos + sizeof *out < SNEKS_DIRECTORY_DENTSBUF_MAX
		&& (dent = get_dentry(file, &blk, dix, &n), dent != NULL))
	{
		out = (void *)data_buf + buf_pos;
		*out = (struct sneks_directory_dentry){
			.ino = dent->ino, .off = dix + 1,
			.reclen = sizeof *out + dent->name_len + 1,
			.type = dent->type,
			.namlen = dent->name_len,
		};
		if(buf_pos + out->reclen > SNEKS_DIRECTORY_DENTSBUF_MAX) break;
		memcpy(out + 1, dent->name, out->namlen + 1);
		buf_pos += out->reclen; got++; dix++;
	}
	if(n < 0) return n;

	*data_len_p = buf_pos;
	*endpos_ptr = file->pos = dix;
	set_rollback(&rollback_getdents, *offset_ptr, file);
	return got;
}


static int squashfs_ipc_loop(
	void *initrd_start, size_t initrd_size,
	int argc, char *argv[])
{
	struct squashfs_impl_vtable vtab = {
		/* Sneks::Path */
		.resolve = &squashfs_resolve,
		.get_path = &squashfs_get_path,
		.get_path_fragment = &squashfs_get_path_fragment,

		/* Sneks::File */
		.open = &squashfs_open,
		.seek = &squashfs_seek,

		/* Sneks::Directory */
		.opendir = &squashfs_opendir,
		.seekdir = &squashfs_seekdir,
		.getdents = &squashfs_getdents,
	};
	FILL_SNEKS_IO(&vtab);

	io_read_func(&squashfs_io_read);
	io_write_func(&squashfs_io_write);
	io_close_func(&squashfs_io_close);
	io_stat_func(&squashfs_io_stat);
	io_confirm_func(&squashfs_wr_confirm);

	io_dispatch_func(&_muidl_squashfs_impl_dispatch, &vtab);
	return io_run(sizeof(iof_t), argc, argv);
}


static unsigned mount_squashfs_image(void *start, size_t sz)
{
	fs_image = start;
	fs_length = sz;
	fs_super = start + SQUASHFS_START;
	fs_block_size_log2 = size_to_shift(fs_super->block_size);
	if(1 << fs_block_size_log2 != fs_super->block_size) {
		log_err("block size is not power of two?");
		return EINVAL;
	}
	if(memcmp(&fs_super->s_magic, "hsqs", 4) != 0) {
		log_err("invalid squashfs superblock magic number %#08x (BE)",
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

	switch(fs_super->compression) {
		case LZ4_COMPRESSION:
			break;
		default:
			log_err("can't handle compression mode %d (%s)",
				fs_super->compression, c_mode);
			return EINVAL;
	}

	ino_to_blkoffset = malloc((fs_super->inodes + 1) * sizeof *ino_to_blkoffset);
	if(ino_to_blkoffset == NULL) return ENOMEM;
	for(int i=0; i <= fs_super->inodes; i++) ino_to_blkoffset[i] = ~0u;
	struct inode *nod = read_inode(fs_super->root_inode);
	if(nod == NULL) return EIO;
	root_ino = squashfs_i(nod)->X.base.inode_number;
	ino_to_blkoffset[root_ino] = fs_super->root_inode;

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

	struct dev *dev = calloc(1, sizeof *dev);
	memcpy(dev->name, name, min(strlen(name), sizeof dev->name - 1));
	switch(type[0]) {
		case 'c': dev->is_chr = true; break;
		case 'b': dev->is_chr = false; break;
		default: goto malformed;
	}
	dev->major = strtol(major, NULL, 10);
	dev->minor = strtol(minor, NULL, 10);

	htable_add(&device_nodes, rehash_dev(dev, NULL), dev);
	return;

malformed:
	log_crit("malformed device-nodes entry `%s'", line);
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
			log_crit("read error n=%d", n);
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
	log_info("have %d device nodes", (int)htable_count(&device_nodes));
}


static void fs_initialize(void)
{
	if(device_nodes_enabled) {
		int typ = 0;
		ino_t ino = lookup(&typ, root_ino, "dev");
		if(ino >= 0 && typ == DT_DIR) {
			dev_ino = ino;
			ino = lookup(&typ, dev_ino, ".device-nodes");
			if(ino >= 0 && typ == DT_REG) {
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
		log_crit("option parsing failed");
		return EXIT_FAILURE;
	}

	if(L4_IsNilThread(boot_tid)) {
		log_crit("no boot_tid");
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
	/* ReplyReceive_Timeout */
	tag = L4_Ipc(boot_tid, boot_tid, L4_Timeouts(L4_ZeroTime, timeout),
		&(L4_ThreadId_t){ /* dummy */ });
	if(L4_IpcFailed(tag)) goto ipcfail;

	int status = mount_squashfs_image(initrd_start, initrd_size);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
	L4_LoadMR(1, status);
	tag = L4_Reply(boot_tid);
	if(L4_IpcFailed(tag)) goto ipcfail;

	if(status == 0) {
		device_nodes_enabled = pidof_NP(boot_tid) >= SNEKS_MIN_SYSID;
		fs_initialize();
		squashfs_ipc_loop(initrd_start, initrd_size, argc, argv);
	}

	return status;

ipcfail:
	log_crit("initrd protocol fail, tag=%#lx ec=%lu", tag.raw, L4_ErrorCode());
	return EXIT_FAILURE;
}
