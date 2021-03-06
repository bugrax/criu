/*
 * IRMAP -- inode reverse mapping.
 *
 * Helps us to map inode number (and device) back to path
 * so that we can restore inotify/fanotify-s.
 *
 * Scanning _is_ slow, so we limit it with hints, which are
 * heurisitical known places where notifies are typically put.
 */

#include <stdbool.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xmalloc.h"
#include "irmap.h"
#include "mount.h"
#include "log.h"
#include "util.h"
#include "image.h"
#include "stats.h"
#include "pstree.h"

#include "protobuf.h"
#include "protobuf/fsnotify.pb-c.h"
#include "protobuf/fh.pb-c.h"

#undef	LOG_PREFIX
#define LOG_PREFIX "irmap: "

#define IRMAP_CACHE_BITS	5
#define IRMAP_CACHE_SIZE	(1 << IRMAP_CACHE_BITS)
#define IRMAP_CACHE_MASK	(IRMAP_CACHE_SIZE - 1)

static inline int irmap_hashfn(unsigned int s_dev, unsigned long i_ino)
{
	return (s_dev + i_ino) & IRMAP_CACHE_MASK;
}

struct irmap {
	unsigned int dev;
	unsigned long ino;
	char *path;
	struct irmap *next;
	bool revalidate;
	int nr_kids;
	struct irmap *kids;
};

static struct irmap *cache[IRMAP_CACHE_SIZE];

static struct irmap hints[] = {
	{ .path = "/etc", .nr_kids = -1, },
	{ .path = "/var/spool", .nr_kids = -1, },
	{ .path = "/lib/udev", .nr_kids = -1, },
	{ .path = "/no-such-path", .nr_kids = -1, },
	{ },
};

/*
 * Update inode (and device) number and cache the entry
 */
static int irmap_update_stat(struct irmap *i)
{
	struct stat st;
	int mntns_root;
	unsigned hv;

	if (i->ino)
		return 0;

	mntns_root = get_service_fd(ROOT_FD_OFF);

	pr_debug("Refresh stat for %s\n", i->path);
	if (fstatat(mntns_root, i->path + 1, &st, AT_SYMLINK_NOFOLLOW)) {
		pr_perror("Can't stat %s", i->path);
		return -1;
	}

	i->revalidate = false;
	i->dev = st.st_dev;
	i->ino = st.st_ino;
	if (!S_ISDIR(st.st_mode))
		i->nr_kids = 0; /* don't irmap_update_dir */

	hv = irmap_hashfn(i->dev, i->ino);
	i->next = cache[hv];
	cache[hv] = i;

	return 0;
}

/*
 * Update list of children, but don't cache any. Later
 * we'll scan them one-by-one and cache.
 */
static int irmap_update_dir(struct irmap *t)
{
	int fd, nr = 0, dlen, mntns_root;
	DIR *dfd;
	struct dirent *de;

	if (t->nr_kids >= 0)
		return 0;

	mntns_root = get_service_fd(ROOT_FD_OFF);

	pr_debug("Refilling %s dir\n", t->path);
	fd = openat(mntns_root, t->path + 1, O_RDONLY);
	if (fd < 0) {
		pr_perror("Can't open %s", t->path);
		return -1;
	}

	dlen = strlen(t->path);
	dfd = fdopendir(fd);
	if (!dfd) {
		pr_perror("Can't opendir %s", t->path);
		return -1;
	}

	errno = 0;
	while ((de = readdir(dfd)) != NULL) {
		struct irmap *k;

		if (dir_dots(de))
			continue;

		nr++;
		if (xrealloc_safe(&t->kids, nr * sizeof(struct irmap)))
			goto out_err;

		k = &t->kids[nr - 1];

		k->kids = NULL;	 /* for xrealloc above */
		k->ino = 0;	 /* for irmap_update_stat */
		k->nr_kids = -1; /* for irmap_update_dir */

		k->path = xmalloc(dlen + strlen(de->d_name) + 2);
		if (!k->path)
			goto out_err;

		sprintf(k->path, "%s/%s", t->path, de->d_name);
	}

	if (errno) {
		pr_perror("Readdir failed");
		goto out_err;
	}

	closedir(dfd);
	close(fd);
	t->nr_kids = nr;
	return 0;

out_err:
	xfree(t->kids);
	closedir(dfd);
	close(fd);
	return -1;
}

static struct irmap *irmap_scan(struct irmap *t, unsigned int dev, unsigned long ino)
{
	struct irmap *c;
	int i;

	if (irmap_update_stat(t))
		return NULL;

	if (t->dev == dev && t->ino == ino)
		return t;

	if (irmap_update_dir(t))
		return NULL;

	for (i = 0; i < t->nr_kids; i++) {
		c = irmap_scan(&t->kids[i], dev, ino);
		if (c)
			return c;
	}

	return NULL;
}

static int irmap_revalidate(struct irmap *c, struct irmap **p)
{
	struct stat st;
	int mntns_root;

	mntns_root = get_service_fd(ROOT_FD_OFF);

	pr_debug("Revalidate stat for %s\n", c->path);
	if (fstatat(mntns_root, c->path + 1, &st, AT_SYMLINK_NOFOLLOW)) {
		/* File can be (re)moved, so just treat it as invalid */
		pr_perror("Can't stat %s", c->path);
		goto invalid;
	}

	if (c->dev != st.st_dev)
		goto invalid;
	if (c->ino != st.st_ino)
		goto invalid;

	c->revalidate = false;
	return 0;

invalid:
	pr_debug("\t%x:%lx is invalid\n", c->dev, c->ino);
	*p = c->next;
	xfree(c->path);
	xfree(c);
	return 1;
}

char *irmap_lookup(unsigned int s_dev, unsigned long i_ino)
{
	struct irmap *c, *h, **p;
	char *path = NULL;
	int hv;

	s_dev = kdev_to_odev(s_dev);

	pr_debug("Resolving %x:%lx path\n", s_dev, i_ino);

	if (mntns_collect_root(root_item->pid.real) < 0)
		goto out;

	timing_start(TIME_IRMAP_RESOLVE);

	hv = irmap_hashfn(s_dev, i_ino);
	for (p = &cache[hv]; *p; p = &(*p)->next) {
		c = *p;
		if (!(c->dev == s_dev && c->ino == i_ino))
			continue;

		if (c->revalidate && irmap_revalidate(c, p))
			continue;

		pr_debug("\tFound %s in cache\n", c->path);
		path = c->path;
		goto out;
	}

	for (h = hints; h->path; h++) {
		pr_debug("Scanning %s hint\n", h->path);
		c = irmap_scan(h, s_dev, i_ino);
		if (c) {
			pr_debug("\tScanned %s\n", c->path);
			path = c->path;
			goto out;
		}
	}

out:
	timing_stop(TIME_IRMAP_RESOLVE);
	return path;
}

/*
 * IRMAP pre-cache -- do early irmap scan on pre-dump to reduce
 * the freeze time on dump
 */

struct irmap_predump {
	unsigned int dev;
	unsigned long ino;
	FhEntry fh;
	struct irmap_predump *next;
};

static struct irmap_predump *predump_queue;

int irmap_queue_cache(unsigned int dev, unsigned long ino,
		FhEntry *fh)
{
	struct irmap_predump *ip;

	ip = xmalloc(sizeof(*ip));
	if (!ip)
		return -1;

	ip->dev = dev;
	ip->ino = ino;
	ip->fh = *fh;
	fh->handle = NULL; /* don't free in free_fhandle */

	pr_debug("Queue %x:%lx for pre-dump\n", dev, ino);

	ip->next = predump_queue;
	predump_queue = ip;
	return 0;
}

int irmap_predump_run(void)
{
	int ret = 0, fd;
	struct irmap_predump *ip;

	fd = open_image_at(AT_FDCWD, CR_FD_IRMAP_CACHE, O_DUMP);
	if (fd < 0)
		return -1;

	pr_info("Running irmap pre-dump\n");

	for (ip = predump_queue; ip; ip = ip->next) {
		pr_debug("\tchecking %x:%lx\n", ip->dev, ip->ino);
		ret = check_open_handle(ip->dev, ip->ino, &ip->fh);
		if (ret) {
			pr_err("Failed to resolve %x:%lx\n", ip->dev, ip->ino);
			break;
		}

		if (ip->fh.path) {
			IrmapCacheEntry ic = IRMAP_CACHE_ENTRY__INIT;

			pr_info("Irmap cache %x:%lx -> %s\n", ip->dev, ip->ino, ip->fh.path);
			ic.dev = ip->dev;
			ic.inode = ip->ino;
			ic.path = ip->fh.path;

			ret = pb_write_one(fd, &ic, PB_IRMAP_CACHE);
			if (ret)
				break;
		}
	}

	close(fd);
	return ret;
}

static int irmap_cache_one(IrmapCacheEntry *ie)
{
	struct irmap *ic;
	unsigned hv;

	ic = xmalloc(sizeof(*ic));
	if (!ic)
		return -1;

	ic->dev = ie->dev;
	ic->ino = ie->inode;
	ic->path = xstrdup(ie->path);
	if (!ie->path) {
		xfree(ic);
		return -1;
	}

	ic->nr_kids = 0;
	/*
	 * We've loaded entry from cache, thus we'll need to check
	 * whether it's still valid when find it in cache.
	 */
	ic->revalidate = true;

	pr_debug("Pre-cache %x:%lx -> %s\n", ic->dev, ic->ino, ic->path);

	hv = irmap_hashfn(ic->dev, ic->ino);
	ic->next = cache[hv];
	cache[hv] = ic;

	return 0;
}

static int open_irmap_cache(int *fd)
{
	int dir = AT_FDCWD;

	pr_info("Searching irmap cache in work dir\n");
in:
	*fd = open_image_at(dir, CR_FD_IRMAP_CACHE, O_RSTR | O_OPT);
	if (dir != AT_FDCWD)
		close(dir);

	if (*fd >= 0) {
		pr_info("... done\n");
		return 1;
	}

	if (*fd == -ENOENT && dir == AT_FDCWD) {
		pr_info("Searching irmap cache in parent\n");
		dir = openat(get_service_fd(IMG_FD_OFF), CR_PARENT_LINK, O_RDONLY);
		if (dir >= 0)
			goto in;
	}

	if (*fd != -ENOENT)
		return -1;

	pr_info("No irmap cache\n");
	return 0;
}

int irmap_load_cache(void)
{
	int fd, ret;

	ret = open_irmap_cache(&fd);
	if (ret <= 0)
		return ret;

	pr_info("Loading irmap cache\n");
	while (1) {
		IrmapCacheEntry *ic;

		ret = pb_read_one_eof(fd, &ic, PB_IRMAP_CACHE);
		if (ret <= 0)
			break;

		ret = irmap_cache_one(ic);
		if (ret < 0)
			break;

		irmap_cache_entry__free_unpacked(ic, NULL);
	}

	close(fd);
	return ret;
}
