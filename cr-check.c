#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include "proc_parse.h"
#include "sockets.h"
#include "crtools.h"
#include "log.h"
#include "util-net.h"
#include "syscall.h"
#include "files.h"
#include "sk-inet.h"
#include "proc_parse.h"

static int check_map_files(void)
{
	int ret;

	ret = access("/proc/self/map_files", R_OK);
	if (!ret)
		return 0;

	pr_msg("/proc/<pid>/map_files directory is missing.\n");
	return -1;
}

static int check_sock_diag(void)
{
	int ret;

	ret = collect_sockets();
	if (!ret)
		return 0;

	pr_msg("sock diag infrastructure is incomplete.\n");
	return -1;
}

static int check_ns_last_pid(void)
{
	int ret;

	ret = access(LAST_PID_PATH, W_OK);
	if (!ret)
		return 0;

	pr_msg("%s sysctl is missing.\n", LAST_PID_PATH);
	return -1;
}

static int check_sock_peek_off(void)
{
	int sk;
	int ret, off, sz;

	sk = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (sk < 0) {
		pr_perror("Can't create unix socket for check");
		return -1;
	}

	sz = sizeof(off);
	ret = getsockopt(sk, SOL_SOCKET, SO_PEEK_OFF, &off, (socklen_t *)&sz);
	close(sk);

	if ((ret == 0) && (off == -1) && (sz == sizeof(int)))
		return 0;

	pr_msg("SO_PEEK_OFF sockoption doesn't work.\n");
	return -1;
}

static int check_kcmp(void)
{
	int ret = sys_kcmp(getpid(), -1, -1, -1, -1);

	if (ret != -ENOSYS)
		return 0;

	pr_msg("System call kcmp is not supported\n");
	return -1;
}

static int check_prctl(void)
{
	unsigned long user_auxv = 0;
	unsigned int *tid_addr;
	int ret;

	ret = sys_prctl(PR_GET_TID_ADDRESS, (unsigned long)&tid_addr, 0, 0, 0);
	if (ret) {
		pr_msg("prctl: PR_GET_TID_ADDRESS is not supported\n");
		return -1;
	}

	ret = sys_prctl(PR_SET_MM, PR_SET_MM_BRK, sys_brk(0), 0, 0);
	if (ret) {
		if (ret == -EPERM)
			pr_msg("prctl: One needs CAP_SYS_RESOURCE capability to perform testing\n");
		else
			pr_msg("prctl: PR_SET_MM is not supported\n");
		return -1;
	}

	ret = sys_prctl(PR_SET_MM, PR_SET_MM_EXE_FILE, -1, 0, 0);
	if (ret != -EBADF) {
		pr_msg("prctl: PR_SET_MM_EXE_FILE is not supported (%d)\n", ret);
		return -1;
	}

	ret = sys_prctl(PR_SET_MM, PR_SET_MM_AUXV, (long)&user_auxv, sizeof(user_auxv), 0);
	if (ret) {
		pr_msg("prctl: PR_SET_MM_AUXV is not supported\n");
		return -1;
	}

	return 0;
}

static int check_fcntl(void)
{
	/*
	 * FIXME Add test for F_GETOWNER_UIDS once
	 * it's merged into mainline and kernel part
	 * settle down.
	 */
	return 0;
}

static int check_proc_stat(void)
{
	struct proc_pid_stat stat;
	int ret;

	ret = parse_pid_stat(getpid(), &stat);
	if (ret) {
		pr_msg("procfs: stat extension is not supported\n");
		return -1;
	}

	return 0;
}

static int check_one_fdinfo(union fdinfo_entries *e, void *arg)
{
	*(int *)arg = (int)e->efd.counter;
	return 0;
}

static int check_fdinfo_eventfd(void)
{
	int fd, ret;
	int cnt = 13, proc_cnt = 0;

	fd = eventfd(cnt, 0);
	if (fd < 0) {
		pr_perror("Can't make eventfd");
		return -1;
	}

	ret = parse_fdinfo(fd, FDINFO_EVENTFD, check_one_fdinfo, &proc_cnt);
	close(fd);

	if (ret) {
		pr_err("Error parsing proc fdinfo\n");
		return -1;
	}

	if (proc_cnt != cnt) {
		pr_err("Counter mismatch (or not met) %d want %d\n",
				proc_cnt, cnt);
		return -1;
	}

	pr_info("Eventfd fdinfo works OK (%d vs %d)\n", cnt, proc_cnt);
	return 0;
}

static int check_one_epoll(union fdinfo_entries *e, void *arg)
{
	*(int *)arg = e->epl.tfd;
	return 0;
}

static int check_fdinfo_eventpoll(void)
{
	int efd, pfd[2], proc_fd = 0, ret;
	struct epoll_event ev;

	if (pipe(pfd)) {
		pr_perror("Can't make pipe to watch");
		return -1;
	}

	efd = epoll_create(1);
	if (efd < 0) {
		pr_perror("Can't make epoll fd");
		return -1;
	}

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLOUT;

	if (epoll_ctl(efd, EPOLL_CTL_ADD, pfd[0], &ev)) {
		pr_perror("Can't add epoll tfd");
		return -1;
	}

	ret = parse_fdinfo(efd, FDINFO_EVENTPOLL, check_one_epoll, &proc_fd);
	close(efd);
	close(pfd[0]);
	close(pfd[1]);

	if (ret) {
		pr_err("Error parsing proc fdinfo");
		return -1;
	}

	if (pfd[0] != proc_fd) {
		pr_err("TFD mismatch (or not met) %d want %d\n",
				proc_fd, pfd[0]);
		return -1;
	}

	pr_info("Epoll fdinfo works OK (%d vs %d)\n", pfd[0], proc_fd);
	return 0;
}

static int check_fdinfo_ext(void)
{
	int ret = 0;

	ret |= check_fdinfo_eventfd();
	ret |= check_fdinfo_eventpoll();

	return ret;
}

int cr_check(void)
{
	int ret = 0;

	ret |= check_map_files();
	ret |= check_sock_diag();
	ret |= check_ns_last_pid();
	ret |= check_sock_peek_off();
	ret |= check_kcmp();
	ret |= check_prctl();
	ret |= check_fcntl();
	ret |= check_proc_stat();
	ret |= check_tcp_repair();
	ret |= check_fdinfo_ext();

	if (!ret)
		pr_msg("Looks good.\n");

	return ret;
}
