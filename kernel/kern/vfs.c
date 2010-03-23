#include <sys/types.h>
#include <sys/bio.h>
#include <sys/device.h>
#include <sys/lib.h>
#include <sys/mm.h>
#include <sys/vfs.h>

#define VFS_MOUNTED_FS_MAX	16

struct VFS_MOUNTED_FS mountedfs[VFS_MOUNTED_FS_MAX];

extern struct VFS_FILESYSTEM_OPS fsops_ext2;

void
vfs_init()
{
	memset(mountedfs, 0, sizeof(mountedfs));
}

static struct VFS_MOUNTED_FS*
vfs_get_availmountpoint()
{
	for (unsigned int n = 0; n < VFS_MOUNTED_FS_MAX; n++) {
		struct VFS_MOUNTED_FS* fs = &mountedfs[n];
		/* XXX lock etc */
		if (fs->mountpoint == NULL)
			return fs;
	}
	return NULL;
}

int
vfs_mount(const char* from, const char* to, const char* type, void* options)
{
	struct VFS_MOUNTED_FS* fs = vfs_get_availmountpoint();
	if (fs == NULL)
		return -1;

	struct DEVICE* dev = device_find(from);
	if (dev == NULL)
		return -2;

	fs->device = dev;
	fs->mountpoint = strdup(to);

	/* XXX kludge */
	fs->fsops = &fsops_ext2;

	if (!fs->fsops->mount(fs)) {
		kfree((void*)fs->mountpoint);
		memset(fs, 0, sizeof(*fs));
		return -3;
	}
	
	return 1;
}

struct VFS_INODE*
vfs_make_inode(struct VFS_MOUNTED_FS* fs)
{
	struct VFS_INODE* inode = kmalloc(sizeof(struct VFS_INODE));

	memset(inode, 0, sizeof(*inode));
	inode->fs = fs;
	return inode;
}

void
vfs_destroy_inode(struct VFS_INODE* inode)
{
	kfree(inode);
}

struct VFS_INODE*
vfs_alloc_inode(struct VFS_MOUNTED_FS* fs, ino_t inum)
{
	struct VFS_INODE* inode;

	if (fs->fsops->alloc_inode != NULL) {
		inode = fs->fsops->alloc_inode(fs);
	} else {
		inode = vfs_make_inode(fs);
	}
	inode->inum = inum;
	return inode;
}

void
vfs_free_inode(struct VFS_INODE* inode)
{
	if (inode->fs->fsops->alloc_inode != NULL) {
		inode->fs->fsops->free_inode(inode);
	} else {
		vfs_destroy_inode(inode);
	}
}

struct BIO*
vfs_bread(struct VFS_MOUNTED_FS* fs, block_t block, size_t len)
{
	return bio_read(fs->device, block, len);
}

struct VFS_INODE*
vfs_get_inode(struct VFS_MOUNTED_FS* fs, ino_t inum)
{
	struct VFS_INODE* inode = vfs_alloc_inode(fs, inum);
	if (inode == NULL)
		return NULL;
	if (!fs->fsops->read_inode(inode)) {
		vfs_free_inode(inode);
		return NULL;
	}
	return inode;
}

struct VFS_INODE*
vfs_lookup(const char* dentry)
{
	char tmp[VFS_MAX_NAME_LEN + 1];

	/* XXX we only consider the first filesystem! */
	struct VFS_MOUNTED_FS* fs = &mountedfs[0];
	struct VFS_INODE* rootinode = fs->root_inode;
	if (*dentry == '/')
		dentry++;

	const char* curdentry = dentry;
	const char* lookupptr;
	while (1) {
		char* ptr = strchr(curdentry, '/');
		if (ptr != NULL) {
			/* There's a slash in the path - must lookup next part */
			strncpy(tmp, curdentry, ptr - curdentry);
			curdentry = ++ptr;
			lookupptr = tmp;
		} else {
			lookupptr = curdentry;
			curdentry = NULL;
		}

		/* Attempt to look up whatever entry we need */
		struct VFS_INODE* inode = rootinode->iops->lookup(rootinode, lookupptr);
		if (inode == NULL) {
			if (rootinode != fs->root_inode) vfs_free_inode(rootinode);
			return NULL;
		}

		/*
		 * OK, the lookup succeeded. Continue if needed.
		 */
		if (curdentry == NULL) {
			if (rootinode != fs->root_inode) vfs_free_inode(rootinode);
			return inode;
		}

		rootinode = inode;
	}

	/* NOTREACHED */
}

int
vfs_open(const char* fname, struct VFS_FILE* file)
{
	struct VFS_INODE* inode = vfs_lookup(fname);
	if (inode == NULL)
		return 0;

	memset(file, 0, sizeof(struct VFS_FILE));
	file->inode = inode;
	file->offset = 0;
	return 1;
}

void
vfs_close(struct VFS_FILE* file)
{
	if(file->inode != NULL)
		vfs_free_inode(file->inode);
	file->inode = NULL;
}

size_t
vfs_read(struct VFS_FILE* file, void* buf, size_t len)
{
	if (file->inode == NULL)
		return 0;
	return file->inode->iops->read(file, buf, len);
}

int
vfs_seek(struct VFS_FILE* file, off_t offset)
{
	if (file->inode == NULL)
		return 0;
	if (offset > file->inode->length)
		return 0;
	file->offset = offset;
	return 1;
}

/* vim:set ts=2 sw=2: */
