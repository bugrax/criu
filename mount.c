#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "cr_options.h"
#include "asm/types.h"
#include "util.h"
#include "util-pie.h"
#include "log.h"
#include "plugin.h"
#include "mount.h"
#include "pstree.h"
#include "proc_parse.h"
#include "image.h"
#include "namespaces.h"
#include "protobuf.h"
#include "kerndat.h"
#include "fs-magic.h"

#include "protobuf/mnt.pb-c.h"

/*
 * Single linked list of mount points get from proc/images
 */
struct mount_info *mntinfo;

static int open_mountpoint(struct mount_info *pm);

static struct mount_info *mnt_build_tree(struct mount_info *list);
static int validate_mounts(struct mount_info *info, bool call_plugins);

/* Asolute paths are used on dump and relative paths are used on restore */
static inline int is_root(char *p)
{
	return (!strcmp(p, "/"));
}

/* True for the root mount (the topmost one) */
static inline int is_root_mount(struct mount_info *mi)
{
	return is_root(mi->mountpoint + 1);
}

/*
 * True if the mountpoint target is root on its FS.
 *
 * This is used to determine whether we need to postpone
 * mounting. E.g. one can bind mount some subdir from a
 * disk, and in this case we'll have to get the root disk
 * mount first, then bind-mount it. See do_mount_one().
 */
static inline int fsroot_mounted(struct mount_info *mi)
{
	return is_root(mi->root);
}

static int __open_mountpoint(struct mount_info *pm, int mnt_fd);
int open_mount(unsigned int s_dev)
{
	struct mount_info *i;

	for (i = mntinfo; i != NULL; i = i->next)
		if (s_dev == i->s_dev)
			return __open_mountpoint(i, -1);

	return -ENOENT;
}

static struct mount_info *__lookup_mnt_id(struct mount_info *list, int id)
{
	struct mount_info *m;

	for (m = list; m != NULL; m = m->next)
		if (m->mnt_id == id)
			return m;

	return NULL;
}

struct mount_info *lookup_mnt_id(unsigned int id)
{
	return __lookup_mnt_id(mntinfo, id);
}

struct mount_info *lookup_mnt_sdev(unsigned int s_dev)
{
	struct mount_info *m;

	for (m = mntinfo; m != NULL; m = m->next)
		if (m->s_dev == s_dev)
			return m;

	return NULL;
}

static struct mount_info *mount_resolve_path(struct mount_info *mntinfo_tree, const char *path)
{
	size_t pathlen = strlen(path);
	struct mount_info *m = mntinfo_tree, *c;

	while (1) {
		list_for_each_entry(c, &m->children, siblings) {
			size_t n;

			n = strlen(c->mountpoint + 1);
			if (n > pathlen)
				continue;

			if (strncmp(c->mountpoint + 1, path, min(n, pathlen)))
				continue;
			if (n < pathlen && path[n] != '/')
				continue;

			m = c;
			break;
		}
		if (&c->siblings == &m->children)
			break;
	}

	pr_debug("Path `%s' resolved to `%s' mountpoint\n", path, m->mountpoint);
	return m;
}

dev_t phys_stat_resolve_dev(struct mount_info *tree,
				dev_t st_dev, const char *path)
{
	struct mount_info *m;

	m = mount_resolve_path(tree, path);
	/*
	 * BTRFS returns subvolume dev-id instead of
	 * superblock dev-id, in such case return device
	 * obtained from mountinfo (ie subvolume0).
	 */
	return strcmp(m->fstype->name, "btrfs") ?
		MKKDEV(MAJOR(st_dev), MINOR(st_dev)) : m->s_dev;
}

bool phys_stat_dev_match(struct mount_info *tree, dev_t st_dev,
				dev_t phys_dev, const char *path)
{
	if (st_dev == kdev_to_odev(phys_dev))
		return true;

	return phys_dev == phys_stat_resolve_dev(tree, st_dev, path);
}

/*
 * Comparer two mounts. Return true if only mount points are differ.
 * Don't care about root and mountpoints, if bind is true.
 */
static bool mounts_equal(struct mount_info* mi, struct mount_info *c, bool bind)
{
	if (mi->s_dev != c->s_dev ||
	    c->fstype != mi->fstype ||
	    strcmp(c->source, mi->source) ||
	    strcmp(c->options, mi->options))
		return false;

	if (bind)
		return true;

	if (strcmp(c->root, mi->root))
		return false;
	if (strcmp(basename(c->mountpoint), basename(mi->mountpoint)))
		return false;
	return true;
}

static struct mount_info *mnt_build_ids_tree(struct mount_info *list)
{
	struct mount_info *m, *root = NULL;

	/*
	 * Just resolve the mnt_id:parent_mnt_id relations
	 */

	pr_debug("\tBuilding plain mount tree\n");
	for (m = list; m != NULL; m = m->next) {
		struct mount_info *p;

		pr_debug("\t\tWorking on %d->%d\n", m->mnt_id, m->parent_mnt_id);
		p = __lookup_mnt_id(list, m->parent_mnt_id);
		if (!p) {
			/* This should be / */
			if (root == NULL && is_root_mount(m)) {
				root = m;
				continue;
			}

			pr_err("Mountpoint %d w/o parent %d found @%s (root %s)\n",
					m->mnt_id, m->parent_mnt_id, m->mountpoint,
					root ? "found" : "not found");
			if (root && m->is_ns_root) {
				if (!mounts_equal(root, m, true) ||
						strcmp(root->root, m->root)) {
					pr_err("Nested mount namespaces with different roots are not supported yet");
					return NULL;
				}

				/*
				 * A root of a sub mount namespace is
				 * mounted in a temporary directory in the
				 * root mount namespace, so its parent is
				 * the main root.
				 */
				p = root;
			} else
				return NULL;
		}

		m->parent = p;
		list_add_tail(&m->siblings, &p->children);
	}

	if (!root) {
		pr_err("No root found for tree\n");
		return NULL;
	}

	return root;
}

static int mnt_depth(struct mount_info *m)
{
	int depth = 0;
	char *c;

	for (c = m->mountpoint; *c != '\0'; c++)
		if (*c == '/')
			depth++;

	return depth;
}

static void mnt_resort_siblings(struct mount_info *tree)
{
	struct mount_info *m, *p;
	LIST_HEAD(list);

	/*
	 * Put siblings of each node in an order they can be (u)mounted
	 * I.e. if we have mounts on foo/bar/, foo/bar/foobar/ and foo/
	 * we should put them in the foo/bar/foobar/, foo/bar/, foo/ order.
	 * Otherwise we will not be able to (u)mount them in a sequence.
	 *
	 * Funny, but all we need for this is to sort them in the descending
	 * order of the amount of /-s in a path =)
	 *
	 * Use stupid insertion sort here, we're not expecting mount trees
	 * to contain hundreds (or more) elements.
	 */

	pr_info("\tResorting siblings on %d\n", tree->mnt_id);
	while (!list_empty(&tree->children)) {
		int depth;

		m = list_first_entry(&tree->children, struct mount_info, siblings);
		list_del(&m->siblings);

		depth = mnt_depth(m);
		list_for_each_entry(p, &list, siblings)
			if (mnt_depth(p) <= depth)
				break;

		list_add(&m->siblings, &p->siblings);
		mnt_resort_siblings(m);
	}

	list_splice(&list, &tree->children);
}

static void mnt_tree_show(struct mount_info *tree, int off)
{
	struct mount_info *m;

	pr_info("%*s[%s](%d->%d)\n", off, "",
			tree->mountpoint, tree->mnt_id, tree->parent_mnt_id);

	list_for_each_entry(m, &tree->children, siblings)
		mnt_tree_show(m, off + 1);

	pr_info("%*s<--\n", off, "");
}

static int validate_mounts(struct mount_info *info, bool call_plugins)
{
	struct mount_info *m, *t;

	for (m = info; m; m = m->next) {
		if (m->parent && m->parent->shared_id) {
			struct mount_info *ct;
			if (list_empty(&m->parent->mnt_share))
				continue;
			t = list_first_entry(&m->parent->mnt_share, struct mount_info, mnt_share);

			list_for_each_entry(ct, &t->children, siblings) {
				if (mounts_equal(m, ct, false))
					break;
			}
			if (&ct->siblings == &t->children) {
				pr_err("Two shared mounts %d, %d have different sets of children\n",
					m->parent->mnt_id, t->mnt_id);
				pr_err("%d:%s doesn't have a proper point for %d:%s\n",
					t->mnt_id, t->mountpoint,
					m->mnt_id, m->mountpoint);
				return -1;
			}
		}

		if (m->parent && !fsroot_mounted(m)) {
			list_for_each_entry(t, &m->mnt_bind, mnt_bind) {
				if (fsroot_mounted(t) || t->parent == NULL)
					break;
			}
			if (&t->mnt_bind == &m->mnt_bind) {
				int ret;

				if (call_plugins) {
					ret = cr_plugin_dump_ext_mount(m->mountpoint + 1, m->mnt_id);
					if (ret == 0)
						m->need_plugin = true;
				} else if (m->need_plugin)
					/* plugin should take care of this one */
					ret = 0;
				else
					ret = -ENOTSUP;
				if (ret < 0) {
					if (ret == -ENOTSUP)
						pr_err("%d:%s doesn't have a proper root mount\n",
								m->mnt_id, m->mountpoint);
					return -1;
				}
			}
		}

		if (m->parent == NULL)
			continue;

		list_for_each_entry(t, &m->parent->children, siblings) {
			int tlen, mlen;

			if (m == t)
				continue;

			tlen = strlen(t->mountpoint);
			mlen = strlen(m->mountpoint);
			if (mlen < tlen)
				continue;
			if (strncmp(t->mountpoint, m->mountpoint, tlen))
				continue;
			if (mlen > tlen && m->mountpoint[tlen] != '/')
				continue;
			pr_err("%d:%s is overmounted\n", m->mnt_id, m->mountpoint);
			return -1;
		}
	}

	return 0;
}

static int collect_shared(struct mount_info *info)
{
	struct mount_info *m, *t;

	/*
	 * If we have a shared mounts, both master
	 * slave targets are to be present in mount
	 * list, otherwise we can't be sure if we can
	 * recreate the scheme later on restore.
	 */
	for (m = info; m; m = m->next) {
		bool need_share, need_master;

		need_share = m->shared_id && list_empty(&m->mnt_share);
		need_master = m->master_id;

		for (t = info; t && (need_share || need_master); t = t->next) {
			if (t == m)
				continue;
			if (need_master && t->shared_id == m->master_id) {
				pr_debug("The mount %d is slave for %d\n", m->mnt_id, t->mnt_id);
				list_add(&m->mnt_slave, &t->mnt_slave_list);
				m->mnt_master = t;
				need_master = false;
			}

			/* Collect all mounts from this group */
			if (need_share && t->shared_id == m->shared_id) {
				pr_debug("Mount %d is shared with %d group %d\n",
						m->mnt_id, t->mnt_id, m->shared_id);
				list_add(&t->mnt_share, &m->mnt_share);
			}
		}

		if (need_master && m->parent) {
			pr_err("Mount %d (master_id: %d shared_id: %d) "
			       "has unreachable sharing\n", m->mnt_id,
				m->master_id, m->shared_id);
			return -1;
		}

		/* Search bind-mounts */
		if (list_empty(&m->mnt_bind)) {
			/*
			 * A first mounted point will be set up as a source point
			 * for others. Look at propagate_mount()
			 */
			for (t = m->next; t; t = t->next) {
				if (mounts_equal(m, t, true))
					list_add(&t->mnt_bind, &m->mnt_bind);
			}
		}
	}

	return 0;
}

static struct mount_info *mnt_build_tree(struct mount_info *list)
{
	struct mount_info *tree;

	/*
	 * Organize them in a sequence in which they can be mounted/umounted.
	 */

	pr_info("Building mountpoints tree\n");
	tree = mnt_build_ids_tree(list);
	if (!tree)
		return NULL;

	mnt_resort_siblings(tree);
	if (collect_shared(list))
		return NULL;
	pr_info("Done:\n");
	mnt_tree_show(tree, 0);
	return tree;
}

/*
 * mnt_fd is a file descriptor on the mountpoint, which is closed in an error case.
 * If mnt_fd is -1, the mountpoint will be opened by this function.
 */
static int __open_mountpoint(struct mount_info *pm, int mnt_fd)
{
	dev_t dev;
	struct stat st;
	int ret;

	if (mnt_fd == -1) {
		int mntns_root;

		mntns_root = mntns_collect_root(pm->nsid->pid);
		if (mntns_root < 0)
			return -1;

		mnt_fd = openat(mntns_root, pm->mountpoint, O_RDONLY);
		if (mnt_fd < 0) {
			pr_perror("Can't open %s", pm->mountpoint);
			return -1;
		}
	}

	ret = fstat(mnt_fd, &st);
	if (ret < 0) {
		pr_perror("fstat(%s) failed", pm->mountpoint);
		goto err;
	}

	dev = phys_stat_resolve_dev(pm->nsid->mnt.mntinfo_tree, st.st_dev, pm->mountpoint + 1);
	if (dev != pm->s_dev) {
		pr_err("The file system %#x (%#x) %s %s is inaccessible\n",
				pm->s_dev, (int)dev, pm->fstype->name, pm->mountpoint);
		goto err;
	}

	return mnt_fd;
err:
	close(mnt_fd);
	return -1;
}

static int open_mountpoint(struct mount_info *pm)
{
	int fd = -1, ns_old = -1;
	char mnt_path[] = "/tmp/cr-tmpfs.XXXXXX";

	/*
	 * If a mount doesn't have children, we can open a mount point,
	 * otherwise we need to create a "private" copy.
	 */
	if (list_empty(&pm->children))
		return __open_mountpoint(pm, -1);

	pr_info("Something is mounted on top of %s\n", pm->mountpoint);

	/*
	 * To create a "private" copy, the target mount is bind-mounted
	 * in a temporary place w/o MS_REC (non-recursively).
	 * A mount point can't be bind-mounted in criu's namespace, it will be
	 * mounted in a target namespace. The sequence of actions is
	 * mkdtemp, setns(tgt), mount, open, detach, setns(old).
	 */

	if (switch_ns(root_item->pid.real, &mnt_ns_desc, &ns_old) < 0)
		return -1;

	if (mkdtemp(mnt_path) == NULL) {
		pr_perror("Can't create a temporary directory");
		goto out;
	}

	if (mount(pm->mountpoint, mnt_path, NULL, MS_BIND, NULL)) {
		pr_perror("Can't bind-mount %d:%s to %s",
				pm->mnt_id, pm->mountpoint, mnt_path);
		rmdir(mnt_path);
		goto out;
	}

	fd = open_detach_mount(mnt_path);
	if (fd < 0)
		goto out;

	if (restore_ns(ns_old, &mnt_ns_desc)) {
		ns_old = -1;
		goto out;
	}

	return __open_mountpoint(pm, fd);;
out:
	if (ns_old >= 0)
		 restore_ns(ns_old, &mnt_ns_desc);
	close_safe(&fd);
	return -1;
}

/* Is it mounted w or w/o the newinstance option */
static int devpts_dump(struct mount_info *pm)
{
	static const char newinstance[] = ",newinstance";
	struct stat *host_st;
	struct stat st;
	int fdir;
	char *buf;
	int len;

	host_st = kerndat_get_devpts_stat();
	if (host_st == NULL)
		return -1;

	fdir = open_mountpoint(pm);
	if (fdir < 0)
		return -1;

	if (fstat(fdir, &st)) {
		pr_perror("Unable to statfs %d:%s",
				pm->mnt_id, pm->mountpoint);
		close(fdir);
		return -1;
	}
	close(fdir);

	if (host_st->st_dev == st.st_dev)
		return 0;

	len = strlen(pm->options);
	buf = xrealloc(pm->options, len + sizeof(newinstance));
	if (buf == NULL)
		return -1;
	memcpy(buf, newinstance, sizeof(newinstance));

	pm->options = buf;

	return 0;
}

static int tmpfs_dump(struct mount_info *pm)
{
	int ret = -1;
	char tmpfs_path[PSFDS];
	int fd = -1, fd_img = -1;

	fd = open_mountpoint(pm);
	if (fd < 0)
		return -1;

	if (fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) & ~FD_CLOEXEC) == -1) {
		pr_perror("Can not drop FD_CLOEXEC");
		goto out;
	}

	fd_img = open_image(CR_FD_TMPFS_IMG, O_DUMP, pm->mnt_id);
	if (fd_img < 0)
		goto out;

	sprintf(tmpfs_path, "/proc/self/fd/%d", fd);

	ret = cr_system(-1, fd_img, -1, "tar", (char *[])
			{ "tar", "--create",
			"--gzip",
			"--one-file-system",
			"--check-links",
			"--preserve-permissions",
			"--sparse",
			"--numeric-owner",
			"--directory", tmpfs_path, ".", NULL });

	if (ret)
		pr_err("Can't dump tmpfs content\n");

out:
	close_safe(&fd_img);
	close_safe(&fd);
	return ret;
}

static int tmpfs_restore(struct mount_info *pm)
{
	int ret;
	int fd_img;

	fd_img = open_image(CR_FD_TMPFS_IMG, O_RSTR, pm->mnt_id);
	if (fd_img < 0)
		return -1;

	ret = cr_system(fd_img, -1, -1, "tar",
			(char *[]) {"tar", "--extract", "--gzip",
				"--directory", pm->mountpoint, NULL});
	close(fd_img);

	if (ret) {
		pr_err("Can't restore tmpfs content\n");
		return -1;
	}

	return 0;
}

static int binfmt_misc_dump(struct mount_info *pm)
{
	int fd, ret = -1;
	struct dirent *de;
	DIR *fdir = NULL;

	fd = open_mountpoint(pm);
	if (fd < 0)
		return -1;

	fdir = fdopendir(fd);
	if (fdir == NULL) {
		close(fd);
		return -1;
	}

	while ((de = readdir(fdir))) {
		if (dir_dots(de))
			continue;
		if (!strcmp(de->d_name, "register"))
			continue;
		if (!strcmp(de->d_name, "status"))
			continue;

		pr_err("binfmt_misc isn't empty: %s\n", de->d_name);
		goto out;
	}

	ret = 0;
out:
	closedir(fdir);
	return ret;
}

static struct fstype fstypes[] = {
	{
		.name = "unsupported",
		.code = FSTYPE__UNSUPPORTED,
	}, {
		.name = "proc",
		.code = FSTYPE__PROC,
	}, {
		.name = "sysfs",
		.code = FSTYPE__SYSFS,
	}, {
		.name = "devtmpfs",
		.code = FSTYPE__DEVTMPFS,
	}, {
		.name = "binfmt_misc",
		.code = FSTYPE__BINFMT_MISC,
		.dump = binfmt_misc_dump,
	}, {
		.name = "tmpfs",
		.code = FSTYPE__TMPFS,
		.dump = tmpfs_dump,
		.restore = tmpfs_restore,
	}, {
		.name = "devpts",
		.dump = devpts_dump,
		.code = FSTYPE__DEVPTS,
	}, {
		.name = "simfs",
		.code = FSTYPE__SIMFS,
	}, {
		.name = "btrfs",
		.code = FSTYPE__UNSUPPORTED,
	}
};

struct fstype *find_fstype_by_name(char *fst)
{
	int i;

	/*
	 * This fn is required for two things.
	 * 1st -- to check supported filesystems (as just mounting
	 * anything is wrong, almost every fs has its own features)
	 * 2nd -- save some space in the image (since we scan all
	 * names anyway)
	 */

	for (i = 0; i < ARRAY_SIZE(fstypes); i++)
		if (!strcmp(fstypes[i].name, fst))
			return fstypes + i;

	return &fstypes[0];
}

static struct fstype *decode_fstype(u32 fst)
{
	int i;

	if (fst == FSTYPE__UNSUPPORTED)
		goto uns;

	for (i = 0; i < ARRAY_SIZE(fstypes); i++)
		if (fstypes[i].code == fst)
			return fstypes + i;
uns:
	return &fstypes[0];
}

static int dump_one_mountpoint(struct mount_info *pm, int fd)
{
	MntEntry me = MNT_ENTRY__INIT;

	pr_info("\t%d: %x:%s @ %s\n", pm->mnt_id, pm->s_dev,
			pm->root, pm->mountpoint);

	me.fstype		= pm->fstype->code;
	if ((me.fstype == FSTYPE__UNSUPPORTED) && !is_root_mount(pm)) {
		struct mount_info *t;

		/* Is it a bind-mount of the root mount */
		list_for_each_entry(t, &pm->mnt_bind, mnt_bind)
			if (t->parent == NULL)
				break;

		if (&t->mnt_bind == &pm->mnt_bind ||
		    strlen(t->source) > strlen(pm->source)) {
			pr_err("FS mnt %s dev %#x root %s unsupported\n",
					pm->mountpoint, pm->s_dev, pm->root);
			return -1;
		}
	}

	if (!pm->need_plugin && pm->fstype->dump && pm->fstype->dump(pm))
		return -1;

	me.mnt_id		= pm->mnt_id;
	me.root_dev		= pm->s_dev;
	me.parent_mnt_id	= pm->parent_mnt_id;
	me.flags		= pm->flags;
	me.root			= pm->root;
	me.mountpoint		= pm->mountpoint + 1;
	me.source		= pm->source;
	me.options		= pm->options;
	me.shared_id		= pm->shared_id;
	me.has_shared_id	= true;
	me.master_id		= pm->master_id;
	me.has_master_id	= true;
	if (pm->need_plugin) {
		me.has_with_plugin = true;
		me.with_plugin = true;
	}

	if (pb_write_one(fd, &me, PB_MNT))
		return -1;

	return 0;
}

struct mount_info *collect_mntinfo(struct ns_id *ns)
{
	struct mount_info *pm, *p;

	if (mntns_collect_root(ns->pid) < 0)
		return NULL;

	pm = parse_mountinfo(ns->pid, ns);
	if (!pm) {
		pr_err("Can't parse %d's mountinfo\n", ns->pid);
		return NULL;
	}

	ns->mnt.mntinfo_tree = mnt_build_tree(pm);
	if (ns->mnt.mntinfo_tree == NULL)
		goto err;

	return pm;
err:
	while (pm) {
		p = pm;
		pm = pm->next;
		xfree(p);
	}

	ns->mnt.mntinfo_tree = NULL;

	return NULL;
}

int dump_mnt_ns(struct ns_id *ns)
{
	struct mount_info *pm, *pms;
	int img_fd = -1, ret = -1;
	int ns_id = ns->id;

	pms = collect_mntinfo(ns);
	if (pms == NULL)
		goto err;

	if (validate_mounts(pms, true))
		goto err;

	pr_info("Dumping mountpoints\n");

	img_fd = open_image(CR_FD_MNTS, O_DUMP, ns_id);
	if (img_fd < 0)
		goto err;

	for (pm = pms; pm; pm = pm->next)
		if (dump_one_mountpoint(pm, img_fd))
			goto err;

	if (mntinfo == NULL)
		mntinfo = pms;
	else {
		for (pm = mntinfo; pm->next != NULL; pm = pm->next);

		pm->next = pms;
	}
	ret = 0;
err:
	close(img_fd);
	return ret;
}

/*
 * _fn_f  - pre-order traversal function
 * _fn_f  - post-order traversal function
 * _plist - a postpone list. _el is added to this list, if _fn_f returns
 *	    a positive value, and all lower elements are not enumirated.
 */
#define MNT_TREE_WALK(_r, _el, _fn_f, _fn_r, _plist, _prgs) do {		\
		struct mount_info *_mi = _r;					\
										\
		while (1) {							\
			int ret;						\
										\
			list_del_init(&_mi->postpone);				\
										\
			ret = _fn_f(_mi);					\
			if (ret < 0)						\
				return -1;					\
			else if (ret > 0) {					\
				list_add_tail(&_mi->postpone, _plist);		\
				goto up;					\
			}							\
										\
			_prgs++;					\
										\
			if (!list_empty(&_mi->children)) {			\
				_mi = list_entry(_mi->children._el,		\
						struct mount_info, siblings);	\
				continue;					\
			}							\
	up:									\
			if (_fn_r(_mi))						\
				return -1;					\
			if (_mi == _r)						\
				break;						\
			if (_mi->siblings._el == &_mi->parent->children) {	\
				_mi = _mi->parent;				\
				goto up;					\
			}							\
			_mi = list_entry(_mi->siblings._el,			\
					struct mount_info, siblings);		\
		}								\
	} while (0)

#define MNT_WALK_NONE	0 &&


static int mnt_tree_for_each(struct mount_info *start,
		int (*fn)(struct mount_info *))
{
	struct mount_info *tmp;
	LIST_HEAD(postpone);
	LIST_HEAD(postpone2);
	int progress;

	pr_debug("Start with %d:%s\n", start->mnt_id, start->mountpoint);
	list_add(&start->postpone, &postpone);

again:
	progress = 0;

	list_for_each_entry_safe(start, tmp, &postpone, postpone)
		MNT_TREE_WALK(start, next, fn, MNT_WALK_NONE, &postpone2, progress);

	if (!progress) {
		struct mount_info *m;

		pr_err("A few mount points can't be mounted");
		list_for_each_entry(m, &postpone2, postpone) {
			pr_err("%d:%d %s %s %s\n", m->mnt_id,
				m->parent_mnt_id, m->root,
				m->mountpoint, m->source);
		}
		return -1;
	}

	list_splice_init(&postpone2, &postpone);

	if (!list_empty(&postpone))
		goto again;

	return 0;

}

static int mnt_tree_for_each_reverse(struct mount_info *m,
		int (*fn)(struct mount_info *))
{
	int progress = 0;

	MNT_TREE_WALK(m, prev, MNT_WALK_NONE, fn, (struct list_head *) NULL, progress);

	return 0;
}

static char *resolve_source(struct mount_info *mi)
{
	if (kdev_major(mi->s_dev) == 0)
		/*
		 * Anonymous block device. Kernel creates them for
		 * diskless mounts.
		 */
		return mi->source;

	pr_err("No device for %s mount\n", mi->mountpoint);
	return NULL;
}

static int restore_shared_options(struct mount_info *mi, bool private, bool shared, bool slave)
{
	pr_debug("%d:%s private %d shared %d slave %d\n",
			mi->mnt_id, mi->mountpoint, private, shared, slave);

	if (private && mount(NULL, mi->mountpoint, NULL, MS_PRIVATE, NULL)) {
		pr_perror("Unable to make %s private", mi->mountpoint);
		return -1;
	}
	if (slave && mount(NULL, mi->mountpoint, NULL, MS_SLAVE, NULL)) {
		pr_perror("Unable to make %s slave", mi->mountpoint);
		return -1;
	}
	if (shared && mount(NULL, mi->mountpoint, NULL, MS_SHARED, NULL)) {
		pr_perror("Unable to make %s shared", mi->mountpoint);
		return -1;
	}

	return 0;
}

/*
 * Umount points, which are propagated in slave parents, because
 * we can't be sure, that they were inherited in a real life.
 */
static int umount_from_slaves(struct mount_info *mi)
{
	struct mount_info *t;
	char mpath[PATH_MAX];

	list_for_each_entry(t, &mi->parent->mnt_slave_list, mnt_slave) {
		if (!t->mounted)
			continue;

		snprintf(mpath, sizeof(mpath), "%s/%s",
				t->mountpoint, basename(mi->mountpoint));
		pr_debug("\t\tUmount %s\n", mpath);
		if (umount(mpath) == -1) {
			pr_perror("Can't umount %s", mpath);
			return -1;
		}
	}

	return 0;
}

/*
 * If something is mounted in one shared point, it will be spread in
 * all other points from this shared group.
 *
 * Look at Documentation/filesystems/sharedsubtree.txt for more details
 */
static int propagate_siblings(struct mount_info *mi)
{
	struct mount_info *t;

	/*
	 * Find all mounts, which must be bind-mounted from this one
	 * to inherite shared group or master id
	 */
	list_for_each_entry(t, &mi->mnt_share, mnt_share) {
		pr_debug("\t\tBind %s\n", t->mountpoint);
		t->bind = mi;
	}

	list_for_each_entry(t, &mi->mnt_slave_list, mnt_slave) {
		pr_debug("\t\tBind %s\n", t->mountpoint);
		t->bind = mi;
	}

	return 0;
}

static int propagate_mount(struct mount_info *mi)
{
	struct mount_info *t;

	propagate_siblings(mi);

	if (!mi->parent)
		goto skip_parent;

	umount_from_slaves(mi);

	/* Propagate this mount to everyone from a parent group */

	list_for_each_entry(t, &mi->parent->mnt_share, mnt_share) {
		struct mount_info *c;

		list_for_each_entry(c, &t->children, siblings) {
			if (mounts_equal(mi, c, false)) {
				pr_debug("\t\tPropogate %s\n", c->mountpoint);
				c->mounted = true;
				propagate_siblings(c);
				umount_from_slaves(c);
			}
		}
	}

skip_parent:
	/*
	 * FIXME Currently non-root mounts can be restored
	 * only if a proper root mount exists
	 */
	if (fsroot_mounted(mi) || mi->parent == NULL)
		list_for_each_entry(t, &mi->mnt_bind, mnt_bind) {
			if (t->bind)
				continue;
			if (t->master_id)
				continue;
			t->bind = mi;
		}

	return 0;
}

static int do_new_mount(struct mount_info *mi)
{
	char *src;
	struct fstype *tp = mi->fstype;
	struct mount_info *t;

	src = resolve_source(mi);
	if (!src)
		return -1;

	/*
	 * Wait while all parent are not mounted
	 *
	 * FIXME a child is shared only between parents,
	 * who was present in a moment of birth
	 */
	if (mi->parent->flags & MS_SHARED) {
		list_for_each_entry(t, &mi->parent->mnt_share, mnt_share) {
			if (!t->mounted) {
				pr_debug("\t\tPostpone %s due to %s\n",
						mi->mountpoint, t->mountpoint);
				return 1;
			}
		}
	}

	if (mount(src, mi->mountpoint, tp->name,
			mi->flags & (~MS_SHARED), mi->options) < 0) {
		pr_perror("Can't mount at %s", mi->mountpoint);
		return -1;
	}

	if (restore_shared_options(mi, 0, mi->shared_id, 0))
		return -1;

	mi->mounted = true;

	if (tp->restore && tp->restore(mi))
		return -1;

	return 0;
}

static int restore_ext_mount(struct mount_info *mi)
{
	int ret;

	pr_debug("Restoring external bind mount %s\n", mi->mountpoint);
	ret = cr_plugin_restore_ext_mount(mi->mnt_id, mi->mountpoint, "/", NULL);
	if (ret)
		pr_perror("Can't restore ext mount (%d)\n", ret);
	return ret;
}

static int do_bind_mount(struct mount_info *mi)
{
	bool shared = mi->shared_id && mi->shared_id == mi->bind->shared_id;

	if (!mi->need_plugin) {
		char rpath[PATH_MAX];
		int tok = 0;

		/*
		 * Cut common part of root.
		 * For non-root binds the source is always "/" (checked)
		 * so this will result in this slash removal only.
		 */
		while (mi->root[tok] == mi->bind->root[tok]) {
			tok++;
			if (mi->bind->root[tok] == '\0')
				break;
			BUG_ON(mi->root[tok] == '\0');
		}

		snprintf(rpath, sizeof(rpath), "%s/%s",
				mi->bind->mountpoint, mi->root + tok);
		pr_info("\tBind %s to %s\n", rpath, mi->mountpoint);

		if (mount(rpath, mi->mountpoint, NULL,
					MS_BIND, NULL) < 0) {
			pr_perror("Can't mount at %s", mi->mountpoint);
			return -1;
		}
	} else {
		if (restore_ext_mount(mi))
			return -1;
	}

	/*
	 * shared - the mount is in the same shared group with mi->bind
	 * mi->shared_id && !shared - create a new shared group
	 */
	if (restore_shared_options(mi, !shared && !mi->master_id,
					mi->shared_id && !shared,
					mi->master_id))
		return -1;

	mi->mounted = true;

	return 0;
}

static bool can_mount_now(struct mount_info *mi)
{
	/* The root mount */
	if (!mi->parent)
		return true;

	/*
	 * Private root mounts can be mounted at any time
	 */
	if (!mi->master_id && fsroot_mounted(mi))
		return true;

	/*
	 * Other mounts can be mounted only if they have
	 * the master mount (see propagate_mount) or if we
	 * expect a plugin to help us.
	 */
	if (mi->bind || mi->need_plugin)
		return true;

	return false;
}

static int do_mount_root(struct mount_info *mi)
{
	if (restore_shared_options(mi, !mi->shared_id && !mi->master_id,
						mi->shared_id, mi->master_id))
		return -1;

	mi->mounted = true;

	return 0;
}

static int do_mount_one(struct mount_info *mi)
{
	int ret;

	if (mi->mounted)
		return 0;

	if (!can_mount_now(mi)) {
		pr_debug("Postpone slave %s\n", mi->mountpoint);
		return 1;
	}

	pr_debug("\tMounting %s @%s (%d)\n", mi->fstype->name, mi->mountpoint, mi->need_plugin);

	if (!mi->parent)
		ret = do_mount_root(mi);
	else if (!mi->bind && !mi->need_plugin)
		ret = do_new_mount(mi);
	else
		ret = do_bind_mount(mi);

	if (ret == 0 && propagate_mount(mi))
		return -1;

	if (mi->fstype->code == FSTYPE__UNSUPPORTED) {
		struct statfs st;

		if (statfs(mi->mountpoint, &st)) {
			pr_perror("Unable to statfs %s", mi->mountpoint);
			return -1;
		}
		if (st.f_type == BTRFS_SUPER_MAGIC)
			mi->fstype = find_fstype_by_name("btrfs");
	}

	return ret;
}

static int do_umount_one(struct mount_info *mi)
{
	if (!mi->parent)
		return 0;

	if (mount("none", mi->parent->mountpoint, "none", MS_REC|MS_PRIVATE, NULL)) {
		pr_perror("Can't mark %s as private", mi->parent->mountpoint);
		return -1;
	}

	if (umount(mi->mountpoint)) {
		pr_perror("Can't umount at %s", mi->mountpoint);
		return -1;
	}

	pr_info("Umounted at %s\n", mi->mountpoint);
	return 0;
}

static int clean_mnt_ns(struct mount_info *mntinfo_tree)
{
	pr_info("Cleaning mount namespace\n");

	/*
	 * Mountinfos were collected at prepare stage
	 */

	return mnt_tree_for_each_reverse(mntinfo_tree, do_umount_one);
}

static int cr_pivot_root(char *root)
{
	char put_root[] = "crtools-put-root.XXXXXX";

	pr_info("Move the root to %s\n", root ? : ".");

	if (root) {
		if (chdir(root)) {
			pr_perror("chdir(%s) failed", root);
			return -1;
		}
	}

	if (mkdtemp(put_root) == NULL) {
		pr_perror("Can't create a temporary directory");
		return -1;
	}

	if (pivot_root(".", put_root)) {
		pr_perror("pivot_root(., %s) failed", put_root);
		if (rmdir(put_root))
			pr_perror("Can't remove the directory %s", put_root);
		return -1;
	}

	if (mount("none", put_root, "none", MS_REC|MS_PRIVATE, NULL)) {
		pr_perror("Can't remount root with MS_PRIVATE");
		return -1;
	}

	if (mount("none", put_root, "none", MS_REC|MS_PRIVATE, NULL)) {
		pr_perror("Can't remount root with MS_PRIVATE");
		return -1;
	}

	if (umount2(put_root, MNT_DETACH)) {
		pr_perror("Can't umount %s", put_root);
		return -1;
	}
	if (rmdir(put_root)) {
		pr_perror("Can't remove the directory %s", put_root);
		return -1;
	}

	return 0;
}

struct mount_info *mnt_entry_alloc()
{
	struct mount_info *new;

	new = xzalloc(sizeof(struct mount_info));
	if (new) {
		INIT_LIST_HEAD(&new->children);
		INIT_LIST_HEAD(&new->siblings);
		INIT_LIST_HEAD(&new->mnt_slave_list);
		INIT_LIST_HEAD(&new->mnt_share);
		INIT_LIST_HEAD(&new->mnt_bind);
		INIT_LIST_HEAD(&new->postpone);
	}
	return new;
}

void mnt_entry_free(struct mount_info *mi)
{
	if (mi == NULL)
		return;

	xfree(mi->root);
	xfree(mi->mountpoint);
	xfree(mi->source);
	xfree(mi->options);
	xfree(mi);
}

static void free_mntinfo(struct mount_info *pms)
{
	while (pms) {
		struct mount_info *pm;

		pm = pms->next;
		mnt_entry_free(pms);
		pms = pm;
	}
}

/*
 * mnt_roots is a temporary directory for restoring sub-trees of
 * non-root namespaces.
 */
static char *mnt_roots;

static int create_mnt_roots()
{
	if (mnt_roots)
		return 0;

	if (chdir(opts.root ? : "/")) {
		pr_perror("Unable to change working directory on %s", opts.root);
		return -1;
	}

	mnt_roots = strdup(".criu.mntns.XXXXXX");
	if (mnt_roots == NULL) {
		pr_perror("Can't allocate memory");
		return -1;
	}

	if (mkdtemp(mnt_roots) == NULL) {
		pr_perror("Unable to create a temporary directory");
		mnt_roots = NULL;
		return -1;
	}

	return 0;
}

int rst_collect_local_mntns()
{
	struct ns_id *nsid;

	nsid = shmalloc(sizeof(struct ns_id));
	if (nsid == NULL)
		return -1;

	nsid->nd = &mnt_ns_desc;
	nsid->id = 0;
	nsid->pid = getpid();
	futex_set(&nsid->created, 1);

	mntinfo = collect_mntinfo(nsid);
	if (mntinfo == NULL)
		return -1;

	nsid->next = ns_ids;
	ns_ids = nsid;

	pr_info("Add namespace %d pid %d\n", nsid->id, nsid->pid);

	return 0;
}

static int collect_mnt_from_image(struct mount_info **pms, struct ns_id *nsid)
{
	MntEntry *me = NULL;
	int img, ret;

	img = open_image(CR_FD_MNTS, O_RSTR, nsid->id);
	if (img < 0)
		return -1;

	pr_debug("Reading mountpoint images\n");

	while (1) {
		struct mount_info *pm;
		char root[PATH_MAX] = ".";
		int len, root_len = 1;

		ret = pb_read_one_eof(img, &me, PB_MNT);
		if (ret <= 0)
			break;

		pm = mnt_entry_alloc();
		if (!pm)
			goto err;

		pm->nsid = nsid;
		pm->next = *pms;
		*pms = pm;

		pm->mnt_id		= me->mnt_id;
		pm->parent_mnt_id	= me->parent_mnt_id;
		pm->s_dev		= me->root_dev;
		pm->flags		= me->flags;
		pm->shared_id		= me->shared_id;
		pm->master_id		= me->master_id;
		pm->need_plugin		= me->with_plugin;
		pm->is_ns_root		= is_root(me->mountpoint);

		/* FIXME: abort unsupported early */
		pm->fstype		= decode_fstype(me->fstype);

		pr_debug("\t\tGetting root for %d\n", pm->mnt_id);
		pm->root = xstrdup(me->root);
		if (!pm->root)
			goto err;

		if (nsid->id != root_item->ids->mnt_ns_id)
			root_len = snprintf(root, sizeof(root), "%s/%d/",
						mnt_roots, nsid->id);
		len  = strlen(me->mountpoint) + root_len + 1;
		pm->mountpoint = xmalloc(len);
		if (!pm->mountpoint)
			goto err;
		/*
		 * For bind-mounts we would also fix the root here
		 * too, but bind-mounts restore merges mountpoint
		 * and root paths together, so there's no need in
		 * that.
		 */

		strcpy(pm->mountpoint, root);
		strcpy(pm->mountpoint + root_len, me->mountpoint);

		pr_debug("\t\tGetting mpt for %d %s\n", pm->mnt_id, pm->mountpoint);

		pr_debug("\t\tGetting source for %d\n", pm->mnt_id);
		pm->source = xstrdup(me->source);
		if (!pm->source)
			goto err;

		pr_debug("\t\tGetting opts for %d\n", pm->mnt_id);
		pm->options = xstrdup(me->options);
		if (!pm->options)
			goto err;

		pr_debug("\tRead %d mp @ %s\n", pm->mnt_id, pm->mountpoint);
	}

	if (me)
		mnt_entry__free_unpacked(me, NULL);

	close(img);

	return 0;
err:
	close_safe(&img);
	return -1;
}

static struct mount_info *read_mnt_ns_img()
{
	struct mount_info *pms = NULL;
	struct ns_id *nsid;

	nsid = ns_ids;
	while (nsid) {
		if (nsid->nd != &mnt_ns_desc) {
			nsid = nsid->next;
			continue;
		}

		if (nsid->id != root_item->ids->mnt_ns_id)
			if (create_mnt_roots(true))
				return NULL;

		if (collect_mnt_from_image(&pms, nsid))
			goto err;

		nsid = nsid->next;
	}

	/* Here is not matter where the mount list is saved */
	mntinfo = pms;

	return pms;
err:
	return NULL;
}

char *rst_get_mnt_root(int mnt_id)
{
	struct mount_info *m;
	static char path[PATH_MAX] = "/";

	if (!(root_ns_mask & CLONE_NEWNS))
		return path;

	if (mnt_id == -1)
		return path;

	m = lookup_mnt_id(mnt_id);
	if (m == NULL)
		return NULL;

	if (m->nsid->pid == getpid())
		return path;

	snprintf(path, sizeof(path), "%s/%d/",
			mnt_roots, m->nsid->id);

	return path;
}

int restore_task_mnt_ns(struct ns_id *nsid, pid_t pid)
{
	char path[PATH_MAX];

	if (root_item->ids->mnt_ns_id == nsid->id)
		return 0;

	if (nsid->pid != getpid()) {
		int fd;

		futex_wait_while_eq(&nsid->created, 0);
		fd = open_proc(nsid->pid, "ns/mnt");
		if (fd < 0)
			return -1;

		if (setns(fd, CLONE_NEWNS)) {
			pr_perror("Unable to change mount namespace");
			return -1;
		}
		return 0;
	}

	if (unshare(CLONE_NEWNS)) {
		pr_perror("Unable to unshare mount namespace");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%d/", mnt_roots, nsid->id);

	if (cr_pivot_root(path))
		return -1;

	futex_set_and_wake(&nsid->created, 1);

	return 0;
}

/*
 * All nested mount namespaces are restore as sub-trees of the root namespace.
 */
static int prepare_roots_yard(void)
{
	char path[PATH_MAX];
	struct ns_id *nsid;

	if (mnt_roots == NULL)
		return 0;

	if (mount("none", mnt_roots, "tmpfs", 0, NULL)) {
		pr_perror("Unable to mount tmpfs in %s", mnt_roots);
		return -1;
	}
	if (mount("none", mnt_roots, NULL, MS_PRIVATE, NULL))
		return -1;

	nsid = ns_ids;
	while (nsid) {
		if (nsid->nd != &mnt_ns_desc) {
			nsid = nsid->next;
			continue;
		}

		snprintf(path, sizeof(path), "%s/%d",
				mnt_roots, nsid->id);

		if (mkdir(path, 0600)) {
			pr_perror("Unable to create %s", path);
			return -1;
		}
		nsid = nsid->next;
	}

	return 0;
}

static int populate_mnt_ns(int ns_pid, struct mount_info *mis)
{
	struct mount_info *pms;
	struct ns_id *nsid;

	if (prepare_roots_yard())
		return -1;

	pms = mnt_build_tree(mis);
	if (!pms)
		return -1;

	for (nsid = ns_ids; nsid; nsid = nsid->next) {
		if (nsid->nd != &mnt_ns_desc)
			continue;

		nsid->mnt.mntinfo_tree = pms;
	}

	if (validate_mounts(mis, false))
		return -1;

	return mnt_tree_for_each(pms, do_mount_one);
}

int fini_mnt_ns()
{
	int ret = 0;

	if (mnt_roots == NULL)
		return 0;

	if (mount("none", mnt_roots, "none", MS_REC|MS_PRIVATE, NULL)) {
		pr_perror("Can't remount root with MS_PRIVATE");
		ret = 1;
	}
	/*
	 * Don't exit after a first error, becuase this function
	 * can be used to rollback in a error case.
	 * Don't worry about MNT_DETACH, because files are restored after this
	 * and nobody will not be restored from a wrong mount namespace.
	 */
	if (umount2(mnt_roots, MNT_DETACH)) {
		pr_perror("Can't unmount %s", mnt_roots);
		ret = 1;
	}
	if (rmdir(mnt_roots)) {
		pr_perror("Can't remove the directory %s", mnt_roots);
		ret = 1;
	}

	return ret;
}

int prepare_mnt_ns(int ns_pid)
{
	int ret = -1;
	struct mount_info *mis, *old;
	struct ns_id ns = { .pid = getpid(), .nd = &mnt_ns_desc };

	pr_info("Restoring mount namespace\n");

	old = collect_mntinfo(&ns);
	if (old == NULL)
		return -1;

	close_proc();

	mis = read_mnt_ns_img(ns_pid);
	if (!mis)
		goto out;

	if (chdir(opts.root ? : "/")) {
		pr_perror("chdir(%s) failed", opts.root ? : "/");
		return -1;
	}

	/*
	 * The new mount namespace is filled with the mountpoint
	 * clones from the original one. We have to umount them
	 * prior to recreating new ones.
	 */
	if (!opts.root) {
		if (clean_mnt_ns(ns.mnt.mntinfo_tree))
			return -1;
	} else {
		struct mount_info *mi;

		/* moving a mount residing under a shared mount is invalid. */
		mi = mount_resolve_path(ns.mnt.mntinfo_tree, opts.root);
		if (mi == NULL) {
			pr_err("Unable to find mount point for %s\n", opts.root);
			return -1;
		}
		if (mi->parent == NULL) {
			pr_err("New root and old root are the same\n");
			return -1;
		}

		/* Our root is mounted over the parent (in the same directory) */
		if (!strcmp(mi->parent->mountpoint, mi->mountpoint)) {
			pr_err("The parent of the new root is unreachable\n");
			return -1;
		}

		if (mount("none", mi->parent->mountpoint + 1, "none", MS_SLAVE, NULL)) {
			pr_perror("Can't remount the parent of the new root with MS_SLAVE");
			return -1;
		}
	}

	free_mntinfo(old);

	ret = populate_mnt_ns(ns_pid, mis);
	if (ret)
		goto out;

	if (opts.root)
		ret = cr_pivot_root(NULL);
out:
	return ret;
}

int mntns_collect_root(pid_t pid)
{
	static int mntns_root_pid = -1;

	int fd, pfd;
	int ret;
	char path[PATH_MAX + 1];

	if (mntns_root_pid == pid) /* The required root is already opened */
		return get_service_fd(ROOT_FD_OFF);

	close_service_fd(ROOT_FD_OFF);

	if (!(root_ns_mask & CLONE_NEWNS)) {
		/*
		 * If criu and tasks we dump live in the same mount
		 * namespace, we can just open the root directory.
		 * All paths resolution would occur relative to criu's
		 * root. Even if it is not namespace's root, provided
		 * file paths are resolved, we'd get consistent dump.
		 */
		fd = open("/", O_RDONLY | O_DIRECTORY);
		if (fd < 0) {
			pr_perror("Can't open root\n");
			return -1;
		}

		goto set_root;
	}

	/*
	 * If /proc/pid/root links on '/', it signs that a root of the task
	 * and a root of mntns is the same.
	 */

	pfd = open_pid_proc(pid);
	ret = readlinkat(pfd, "root", path, sizeof(path) - 1);
	if (ret < 0) {
		close_pid_proc();
		return ret;
	}

	path[ret] = '\0';

	if (ret != 1 || path[0] != '/') {
		pr_err("The root task has another root than mntns: %s\n", path);
		close_pid_proc();
		return -1;
	}

	fd = openat(pfd, "root", O_RDONLY | O_DIRECTORY, 0);
	close_pid_proc();
	if (fd < 0) {
		pr_perror("Can't open the task root");
		return -1;
	}

set_root:
	ret = install_service_fd(ROOT_FD_OFF, fd);
	if (ret >= 0)
		mntns_root_pid = pid;
	close(fd);
	return ret;
}

struct ns_id *lookup_nsid_by_mnt_id(int mnt_id)
{
	struct mount_info *mi;

	/*
	 * Kernel before 3.15 doesn't show mnt_id for file descriptors.
	 * mnt_id isn't saved for files, if mntns isn't dumped.
	 * In both these cases we have only one root, so here
	 * is not matter which mount will be restured.
	 */
	if (mnt_id == -1)
		mi = mntinfo;
	else
		mi = lookup_mnt_id(mnt_id);

	if (mi == NULL)
		return NULL;

	return mi->nsid;
}

int collect_mnt_namespaces(void)
{
	struct mount_info *pm, *pms;
	struct ns_id *ns;
	int ret = -1;

	for (ns = ns_ids; ns; ns = ns->next) {
		if (ns->pid == getpid()) {
			if (!(root_ns_mask & CLONE_NEWNS)) {
				mntinfo = collect_mntinfo(ns);
				if (mntinfo == NULL)
					return -1;
			}
			/* Skip current namespaces, which are in the list too  */
			continue;
		}

		if (!(ns->nd->cflag & CLONE_NEWNS))
			continue;

		pr_info("Dump MNT namespace (mountpoints) %d via %d\n",
				ns->id, ns->pid);
		pms = collect_mntinfo(ns);
		if (pms == NULL)
			goto err;

		if (mntinfo == NULL)
			mntinfo = pms;
		else {
			for (pm = mntinfo; pm->next != NULL; pm = pm->next);

			pm->next = pms;
		}
	}
	ret = 0;
err:
	return ret;
}

int dump_mnt_namespaces(void)
{
	struct ns_id *ns;
	int ret = 0, n = 0;

	for (ns = ns_ids; ns; ns = ns->next) {
		/* Skip current namespaces, which are in the list too  */
		if (ns->pid == getpid()) {
			if (!(root_ns_mask & CLONE_NEWNS))
				mntinfo = collect_mntinfo(ns);
				if (mntinfo == NULL)
					return -1;
			continue;
		}

		if (!(ns->nd->cflag & CLONE_NEWNS))
			continue;

		n++;

		if (n == 2 && check_mnt_id()) {
			pr_err("Nested mount namespaces are not supported "
				"without mnt_id in fdinfo\n");
			return -1;
		}
		pr_info("Dump MNT namespace (mountpoints) %d via %d\n",
				ns->id, ns->pid);
		ret = dump_mnt_ns(ns);
		if (ret)
			break;
	}

	return ret;
}

struct ns_desc mnt_ns_desc = NS_DESC_ENTRY(CLONE_NEWNS, "mnt");
