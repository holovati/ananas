/*
 * Ananas ext2fs filesystem driver.
 *
 * ext2 filesystems consist of a set of 'block groups', which have the following
 * format for block group 'g':
 *
 * - superblock (*)
 * - block group descriptor table (*)
 * - block bitmap
 * - inode bitmap
 * - inode table
 * - data blocks
 *
 * Fields marked with a (*) are only present if the SPARSE_BLOCKGROUP
 * filesystem flag is set and the blockgroup number is a power of
 * 2, 3, 5 or 7. They are always present if the flag is absent.
 *
 * The location of the first superblock is always fixed, at byte offset 1024.
 * The superblock 'sb' at this location will be used by ext2_mount(), where we
 * read all blockgroups. This allows us to locate any inode on the disk.
 *
 * The main reference material used is "The Second Extended File System:
 * Internal Layout" by Dave Poirier.
 */
#include <sys/types.h>
#include <sys/bio.h>
#include <sys/vfs.h>
#include <sys/lib.h>
#include <sys/mm.h>
#include <ext2.h>


#define EXT2_TO_LE16(x) (x)
#define EXT2_TO_LE32(x) (x)

struct EXT2_FS_PRIVDATA {
	struct EXT2_SUPERBLOCK sb;

	unsigned int num_blockgroups;
	struct EXT2_BLOCKGROUP* blockgroup;
};

struct EXT2_INODE_PRIVDATA {
	block_t block[EXT2_INODE_BLOCKS];
};

/*
 * This will read a filesystem block; it will not skip any reserved blocks.
 */
static struct BIO*
ext2_bread(struct VFS_MOUNTED_FS* fs, block_t block)
{
	struct EXT2_FS_PRIVDATA* privdata = (struct EXT2_FS_PRIVDATA*)fs->privdata;
	block *= (fs->block_size / 512);
	return vfs_bread(fs, block, fs->block_size);
}

static void
ext2_conv_superblock(struct EXT2_SUPERBLOCK* sb)
{
	/* XXX implement me */
	sb->s_magic = EXT2_TO_LE16(sb->s_magic);
}

static struct VFS_INODE*
ext2_alloc_inode(struct VFS_MOUNTED_FS* fs)
{
	struct VFS_INODE* inode = vfs_make_inode(fs);
	if (inode == NULL)
		return NULL;
	struct EXT2_INODE_PRIVDATA* privdata = kmalloc(sizeof(struct EXT2_INODE_PRIVDATA));
	memset(privdata, 0, sizeof(struct EXT2_INODE_PRIVDATA));
	inode->privdata = privdata;
	return inode;
}

static void
ext2_free_inode(struct VFS_INODE* inode)
{
	kfree(inode->privdata);
	vfs_destroy_inode(inode);
}

static void	
ext2_dump_inode(struct EXT2_INODE* inode)
{
	kprintf("mode    = 0x%x\n", inode->i_mode);
	kprintf("uid/gid = %u:%u\n", inode->i_uid, inode->i_gid);
	kprintf("size    = %u\n", inode->i_size);
	kprintf("blocks  =");
	for(int n = 0; n < 15; n++) {
		kprintf(" %u", inode->i_block[n]);
	}
	kprintf("\n");
}

/*
 * Retrieves the disk block for a given file block. In ext2, the first 12 blocks
 * are direct blocks. Block 13 is the first indirect block and contains pointers to
 * blocks 13 - 13 + X - 1 (where X = blocksize / 4). Thus, for an 1KB block
 * size, blocks 13 - 268 can be located by reading the first indirect block.
 *
 * Block 14 contains the doubly-indirect block, which is a block-pointer to
 * another block in the same format as block 13. Block 14 contains X pointers,
 * and each block therein contains X pointers as well, so with a 1KB blocksize,
 * X * X = 65536 blocks can be stored.
 *
 * Block 15 is the triply-indirect block, which contains a block-pointer to
 * an doubly-indirect block. With an 1KB blocksize, each doubly-indirect block
 * contains X * X blocks, so we can store X * X * X = 16777216 blocks.
 */
static block_t
ext2_find_block(struct VFS_FILE* file, block_t block)
{
	struct VFS_MOUNTED_FS* fs = file->inode->fs;
	struct EXT2_FS_PRIVDATA* fs_privdata = (struct EXT2_FS_PRIVDATA*)file->inode->fs->privdata;
	struct EXT2_INODE_PRIVDATA* in_privdata = (struct EXT2_INODE_PRIVDATA*)file->inode->privdata;

	/*
	 * We need to figure out whether we have to look up the block in the single,
	 * double or triply-linked block. From the comments above, we know that:
	 *
	 * (a) The first 12 blocks (0 .. 11) can directly be accessed.
	 * (a) The first indirect block contains blocks 12 .. 12 + block_size / 4.
	 * (b) The double-indirect block contains blocks 12 + block_size / 4 to
	 *     13 + (block_size / 4)^2
	 * (c) The triple-indirect block contains everything else.
	 */

	/* (a) Direct blocks are easy */
	if (block < 12)
		return in_privdata->block[block];

	if (block < 12 + fs->block_size / 4) {
		/*
		 * (b) Need to look up the block the first indirect block, 13.
		 */
		struct BIO* bio = ext2_bread(file->inode->fs, in_privdata->block[12]);
		uint32_t iblock = EXT2_TO_LE32(*(uint32_t*)(BIO_DATA(bio) + (block - 12) * sizeof(uint32_t)));
		bio_free(bio);
		return iblock;
	}

	panic("ext2_find_block() needs support for doubly/triple indirect blocks!");
}

static size_t
ext2_read(struct VFS_FILE* file, void* buf, size_t len)
{
	struct VFS_MOUNTED_FS* fs = file->inode->fs;
	struct EXT2_FS_PRIVDATA* privdata = (struct EXT2_FS_PRIVDATA*)file->inode->fs->privdata;
	block_t blocknum = (block_t)file->offset / (block_t)fs->block_size;
	uint32_t offset = file->offset % fs->block_size;
	size_t numread = 0;

	/* Normalize len so that it cannot expand beyond the file size */
	if (file->offset + len > file->inode->length)
		len = file->inode->length - file->offset;

	while(len > 0) {
		block_t block = ext2_find_block(file, blocknum);
		if (block == 0) {
			/* We've run out of blocks; this shouldn't happen... */
			panic("ext2_read(): no block found, maybe sparse file?");
			break;
		}

		/*
		 * Fetch the block and copy what we have so far.
		 */
		struct BIO* bio = ext2_bread(file->inode->fs, block);
		size_t chunklen = (fs->block_size < len ? fs->block_size : len);
		if (chunklen + offset > fs->block_size)
			chunklen = fs->block_size - offset;
		memcpy(buf, (void*)(BIO_DATA(bio) + offset), chunklen);
		buf += chunklen; numread += chunklen; len -= chunklen;
		bio_free(bio);

		/* Update pointers */
		offset = (offset + chunklen) % fs->block_size;
		file->offset += chunklen;
		blocknum++;
	}

	return numread;
}

static size_t
ext2_readdir(struct VFS_FILE* file, void* dirents, size_t entsize)
{
	struct VFS_MOUNTED_FS* fs = file->inode->fs;
	struct EXT2_FS_PRIVDATA* privdata = (struct EXT2_FS_PRIVDATA*)file->inode->fs->privdata;
	block_t blocknum = (block_t)file->offset / (block_t)fs->block_size;
	uint32_t offset = file->offset % fs->block_size;
	size_t written = 0;

	struct BIO* bio = NULL;
	block_t curblock = 0;
	while(entsize > 0) {
		block_t block = ext2_find_block(file, blocknum);
		if (block == 0) {
			/*
			 * We've run out of blocks. Need to stop here.
			 */
			break;
		}
		if(curblock != block) {
			if (bio != NULL) bio_free(bio);
			bio = ext2_bread(file->inode->fs, block);
			/* XXX handle error */
			curblock = block;
		}

		/*
		 * ext2 directories contain variable-length records, which is why we have
		 * this loop. Each record tells us how long it is. The 'offset'-part
		 * always refers to the record we have to parse.
	 	 *
		 * We assume records will never cross a block, which is in line with the
		 * specification.
		 */
		struct EXT2_DIRENTRY* ext2de = (struct EXT2_DIRENTRY*)(void*)(BIO_DATA(bio) + offset);
		uint32_t inum = EXT2_TO_LE32(ext2de->inode);
		if (inum > 0) {
			/*
			 * Inode number values of zero indicate the entry is not used; this entry
			 * works and we mustreturn it.
			 */
			int filled = vfs_filldirent(&dirents, &entsize, (const void*)&inum, file->inode->fs->fsop_size, ext2de->name, ext2de->name_len);
			if (!filled) {
				/* out of space! */
				break;
			}
			written += filled;
		}

		/*
		 * Update the offsets; this ensures we will read the correct block next
		 * time.
		 */
		file->offset += EXT2_TO_LE16(ext2de->rec_len);
		offset += EXT2_TO_LE16(ext2de->rec_len);
		if (offset >= fs->block_size) {
			offset -= fs->block_size;
			blocknum++;
		}
	}
	if (bio != NULL) bio_free(bio);
	return written;
}

struct VFS_INODE*
ext2_lookup(struct VFS_INODE* dirinode, const char* dentry)
{
	struct VFS_FILE dir;
	char tmp[1024]; /* XXX */
	char* tmp_ptr = tmp;

	/*
	 * XXX This is a very naive implementation which does not use the
	 * possible directory index.
	 */
	dir.offset = 0;
	dir.inode = dirinode;
	while (1) {
		size_t left = dirinode->iops->readdir(&dir, tmp, sizeof(tmp));
		if (left <= 0)
			break;

		while (left > 0) {
			struct VFS_DIRENT* de = (struct VFS_DIRENT*)tmp_ptr;
			left -= DE_LENGTH(de); tmp_ptr += DE_LENGTH(de);
			
			if (strcmp(de->de_fsop + de->de_fsop_length, dentry) != 0)
				continue;

			/* Found it! */
			struct VFS_INODE* inode = vfs_get_inode(dirinode->fs, de->de_fsop);
			if (inode == NULL)
				return NULL;
			return inode;
		}
	}

	/* Not found */
	return NULL;
}

static struct VFS_INODE_OPS ext2_file_ops = {
	.read = ext2_read,
};

static struct VFS_INODE_OPS ext2_dir_ops = {
	.readdir = ext2_readdir,
	.lookup = ext2_lookup
};

/*
 * Reads a filesystem inode and fills a corresponding inode structure.
 */
static int
ext2_read_inode(struct VFS_INODE* inode, void* fsop)
{
	struct VFS_MOUNTED_FS* fs = inode->fs;
	struct EXT2_FS_PRIVDATA* privdata = (struct EXT2_FS_PRIVDATA*)inode->fs->privdata;

	/*
	 * Inode number zero does not exists within ext2 (or Linux for that matter),
	 * but it is considered wasteful to ignore an inode, so inode 1 maps to the
	 * first inode entry on disk...
	 */
	uint32_t inum = (*(uint32_t*)fsop) - 1;
	KASSERT(inum < privdata->sb.s_inodes_count, "inode out of range");

	/*
	 * Every block group has a fixed number of inodes, so we can find the
	 * blockgroup number and corresponding inode index within this blockgroup by
	 * simple divide and modulo operations. These two are combined to figure out
	 * the block we have to read.
	 */
	uint32_t bgroup = inum / privdata->sb.s_inodes_per_group;
	uint32_t iindex = inum % privdata->sb.s_inodes_per_group;
	block_t block = privdata->blockgroup[bgroup].bg_inode_table + (iindex * privdata->sb.s_inode_size) / fs->block_size;

	/* Fetch the block and make a pointer to the inode */
	struct BIO* bio = ext2_bread(inode->fs, block); /* XXX error handling */
	unsigned int idx = (iindex * privdata->sb.s_inode_size) % fs->block_size;
	struct EXT2_INODE* ext2inode = (struct EXT2_INODE*)((void*)BIO_DATA(bio) + idx);
	inode->length = EXT2_TO_LE32(ext2inode->i_size);

	/*
	 * Copy the block pointers to the private inode structure; we need them for
	 * any read/write operation.
	 */
	struct EXT2_INODE_PRIVDATA* iprivdata = (struct EXT2_INODE_PRIVDATA*)inode->privdata;
	for (unsigned int i = 0; i < EXT2_INODE_BLOCKS; i++)
		iprivdata->block[i] = EXT2_TO_LE32(ext2inode->i_block[i]);

	/* Fill out the inode operations - this depends on the inode type */
	uint16_t imode = EXT2_TO_LE16(ext2inode->i_mode);
	switch(imode & 0xf000) {
		case EXT2_S_IFREG:
			inode->iops = &ext2_file_ops;
			break;
		case EXT2_S_IFDIR:
			inode->iops = &ext2_dir_ops;
			break;
	}
	bio_free(bio);

	return 1;
}

static int
is_pow_of(unsigned int p, unsigned int val)
{
	int b = p;
	while (val > b)
		b *= p;
	return val == b;
}

static int
blockgroup_has_superblock(struct VFS_MOUNTED_FS* fs, block_t blocknum)
{
	struct EXT2_FS_PRIVDATA* privdata = (struct EXT2_FS_PRIVDATA*)fs->privdata;
	if (blocknum <= 1)
		return 1;
	if (!(privdata->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER))
		return 1;
	return is_pow_of(3, blocknum) || is_pow_of(5, blocknum) || is_pow_of(7, blocknum);
}

static int
ext2_mount(struct VFS_MOUNTED_FS* fs)
{
	struct BIO* bio = vfs_bread(fs, 2, 1024);
	/* XXX handle errors */
	struct EXT2_SUPERBLOCK* sb = (struct EXT2_SUPERBLOCK*)BIO_DATA(bio);
	ext2_conv_superblock(sb);
	if (sb->s_magic != EXT2_SUPER_MAGIC) {
		bio_free(bio);
		return 0;
	}

	/* Victory */
	struct EXT2_FS_PRIVDATA* privdata = (struct EXT2_FS_PRIVDATA*)kmalloc(sizeof *privdata);
	memcpy(&privdata->sb, sb, sizeof(*sb));
	fs->privdata = privdata;

	privdata->num_blockgroups = (sb->s_blocks_count - sb->s_first_data_block - 1) / sb->s_blocks_per_group + 1;
	privdata->blockgroup = (struct EXT2_BLOCKGROUP*)kmalloc(sizeof(struct EXT2_BLOCKGROUP) * privdata->num_blockgroups);

	/* Fill out filesystem fields */
	fs->block_size = 1024L << sb->s_log_block_size;
	fs->fsop_size = sizeof(uint32_t);

	/* Free the superblock */
	bio_free(bio);

	/*
	 * Read the block group descriptor table; we use the fact that this table
	 * will always exist at the very first block group, and it will always appear
	 * at the first data block.
	 */
	for (unsigned int n = 0; n < privdata->num_blockgroups; n++) {
		/*
		 * The +1 is because we need to skip the superblock, and the s_first_data_block
	 	 * increment is because we need to count from the superblock onwards...
		 */
		block_t blocknum = 1 + (n * sizeof(struct EXT2_BLOCKGROUP)) / fs->block_size;
		blocknum += privdata->sb.s_first_data_block;
		bio = ext2_bread(fs, blocknum);
		memcpy((void*)(privdata->blockgroup + n),
		       (void*)(BIO_DATA(bio) + ((n * sizeof(struct EXT2_BLOCKGROUP)) % fs->block_size)),
		       sizeof(struct EXT2_BLOCKGROUP));
		bio_free(bio);
	}

#if 0
	//for (unsigned int n = 0; n < privdata->num_blockgroups; n++) {
	for (unsigned int n = 19; n < 21; n++) {
		struct EXT2_BLOCKGROUP* bg = &privdata->blockgroup[n];
		kprintf("blockgroup %u: block=%u,inode=%u,table=%u,free_blocks=%u,free_inodes=%u,used_dirs=%u\n",
			n, bg->bg_block_bitmap, bg->bg_inode_bitmap, bg->bg_inode_table,
			bg->bg_free_blocks_count, bg->bg_free_inodes_count, bg->bg_used_dirs_count);
	}
#endif

	/* Read the root inode */
	fs->root_inode = vfs_alloc_inode(fs);
	uint32_t root_fsop = EXT2_ROOT_INO;
	if (!ext2_read_inode(fs->root_inode, &root_fsop))
		return 0;

	return 1;
}

struct VFS_FILESYSTEM_OPS fsops_ext2 = {
	.mount = ext2_mount,
	.alloc_inode = ext2_alloc_inode,
	.free_inode = ext2_free_inode,
	.read_inode = ext2_read_inode
};

/* vim:set ts=2 sw=2: */
