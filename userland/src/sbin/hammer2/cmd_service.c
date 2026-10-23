#include "hammer2.h"

#include <machine/atomic.h>

struct hammer2_media_config {
	hammer2_volconf_t	copy_run;
	hammer2_volconf_t	copy_pend;
	pthread_t		thread;
	int			thread_started;
	pthread_cond_t		cond;
	int			ctl;
	int			fd;
	int			pipefd[2];
	dmsg_iocom_t		iocom;
	pthread_t		iocom_thread;
	enum { H2MC_STOPPED, H2MC_CONNECT, H2MC_RUNNING } state;
};

typedef struct hammer2_media_config hammer2_media_config_t;

#define H2CONFCTL_STOP          0x00000001
#define H2CONFCTL_UPDATE        0x00000002

struct diskcon {
	TAILQ_ENTRY(diskcon) entry;
	char	*disk;
};

struct service_node_opaque {
	char	cl_label[64];
	char	fs_label[64];
	dmsg_media_block_t block;
	int	attached;
	int	servicing;
	int	servicefd;
};

struct autoconn {
	TAILQ_ENTRY(autoconn) entry;
	char	*host;
	int	stage;
	int	stopme;
	int	pipefd[2];
	enum { AUTOCONN_INACTIVE, AUTOCONN_ACTIVE } state;
	pthread_t thread;
};

#define WS " \r\n"

static TAILQ_HEAD(, diskcon) diskconq = TAILQ_HEAD_INITIALIZER(diskconq);
static pthread_mutex_t diskmtx;
static pthread_mutex_t confmtx;

static void *service_thread(void *data);
static void *udev_thread(void *data);
static void *autoconn_thread(void *data);
static void master_reconnect(const char *mntpt);
static void disk_reconnect(const char *disk);
static void disk_disconnect(void *handle);
static void udev_check_disks(void);
static void hammer2_usrmsg_handler(dmsg_msg_t *msg, int unmanaged);
static void *hammer2_volconf_thread(void *info);
static void hammer2_volconf_signal(dmsg_iocom_t *iocom);
static void hammer2_volconf_start(hammer2_media_config_t *conf,
			const char *hostname);
static void hammer2_volconf_stop(hammer2_media_config_t *conf);

static void xdisk_connect(void);

static char **ServiceMounts;
static int ServiceMountCount;

int
cmd_service(int ac, char **av)
{
	struct sockaddr_in lsin;
	int on;

	ServiceMounts = av;
	ServiceMountCount = ac;
	int lfd;

	if ((lfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "master_listen: socket(): %s\n",
			strerror(errno));
		return 1;
	}
	on = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	bzero(&lsin, sizeof(lsin));
	lsin.sin_family = AF_INET;
	lsin.sin_addr.s_addr = INADDR_ANY;
	lsin.sin_port = htons(DMSG_LISTEN_PORT);
	if (bind(lfd, (struct sockaddr *)&lsin, sizeof(lsin)) < 0) {
		close(lfd);
		if (QuietOpt == 0) {
			fprintf(stderr,
				"master listen: daemon already running\n");
		}
		return 0;
	}
	if (QuietOpt == 0)
		fprintf(stderr, "master listen: startup\n");
	listen(lfd, 50);

	hammer2_demon(service_thread, (void *)(intptr_t)lfd);
	if (NormalExit)
		close(lfd);
	return 0;
}

static
void *
service_thread(void *data)
{
	struct sockaddr_in asin;
	socklen_t alen;
	pthread_t thread;
	dmsg_master_service_info_t *info;
	int lfd = (int)(intptr_t)data;
	int fd;
	int i;
	int count;
	int opt;

	setproctitle("hammer2 master listen");
	pthread_detach(pthread_self());

	pthread_create(&thread, NULL, udev_thread, NULL);

	xdisk_connect();

	pthread_create(&thread, NULL, autoconn_thread, NULL);

	{
		char **mntav;
		int mntac = 0;

		if (ServiceMountCount > 0) {

			fprintf(stderr, "hammer2 service: %d explicit mount(s)\n",
				ServiceMountCount);
			for (i = 0; i < ServiceMountCount; ++i) {
				fprintf(stderr, "hammer2 service: "
					"reconnecting %s\n", ServiceMounts[i]);
				master_reconnect(ServiceMounts[i]);
			}
		} else {
			mntav = get_hammer2_mounts(&mntac);
			fprintf(stderr, "hammer2 service: found %d hammer2 "
				"mount(s) via /proc/self/mounts\n", mntac);
			for (i = 0; i < mntac; ++i) {
				fprintf(stderr, "hammer2 service: "
					"reconnecting %s\n", mntav[i]);
				master_reconnect(mntav[i]);
			}
			if (mntav)
				put_hammer2_mounts(mntac, mntav);
		}
	}
	(void)count;

	for (;;) {
		alen = sizeof(asin);
		fd = accept(lfd, (struct sockaddr *)&asin, &alen);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		opt = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof opt);
		fprintf(stderr, "service_thread: accept fd %d\n", fd);
		info = malloc(sizeof(*info));
		bzero(info, sizeof(*info));
		info->fd = fd;
		info->detachme = 1;
		info->usrmsg_callback = hammer2_usrmsg_handler;
		info->label = strdup("client");
		pthread_create(&thread, NULL, dmsg_master_service, info);
	}
	return (NULL);
}

static
void
hammer2_usrmsg_handler(dmsg_msg_t *msg, int unmanaged)
{
	dmsg_state_t *state;
	hammer2_media_config_t *conf;
	dmsg_lnk_hammer2_volconf_t *msgconf;
	int i;

	state = msg->state;
	if (state == NULL ||
	    (state->rxcmd & DMSGF_BASECMDMASK) != DMSG_LNK_CONN) {
		hammer2_shell_parse(msg, unmanaged);
		return;
	}

	switch(msg->tcmd) {
	case DMSG_LNK_CONN | DMSGF_CREATE | DMSGF_DELETE:
	case DMSG_LNK_CONN | DMSGF_DELETE:
	case DMSG_LNK_ERROR | DMSGF_DELETE:

		if (state->media == NULL || state->media->usrhandle == NULL)
			break;
		conf = state->media->usrhandle;
		fprintf(stderr, "Shutting down media spans\n");
		for (i = 0; i < HAMMER2_COPYID_COUNT; ++i) {
			if (conf[i].thread) {
				conf[i].ctl = H2CONFCTL_STOP;
				pthread_cond_signal(&conf[i].cond);
			}
		}
		for (i = 0; i < HAMMER2_COPYID_COUNT; ++i) {
			if (conf[i].thread) {
				pthread_join(conf[i].thread, NULL);
				conf->thread_started = 0;
				pthread_cond_destroy(&conf[i].cond);
			}
		}
		state->media->usrhandle = NULL;
		free(conf);
		break;
	case DMSG_LNK_HAMMER2_VOLCONF:

		fprintf(stderr, "RECEIVED VOLCONF\n");

		if ((conf = state->media->usrhandle) == NULL) {
			conf = malloc(sizeof(*conf) * HAMMER2_COPYID_COUNT);
			bzero(conf, sizeof(*conf) * HAMMER2_COPYID_COUNT);
			state->media->usrhandle = conf;
		}
		msgconf = H2_LNK_VOLCONF(msg);

		if (msgconf->index < 0 ||
		    msgconf->index >= HAMMER2_COPYID_COUNT) {
			fprintf(stderr,
				"VOLCONF: ILLEGAL INDEX %d\n",
				msgconf->index);
			break;
		}
		if (msgconf->copy.path[sizeof(msgconf->copy.path) - 1] != 0 ||
		    msgconf->copy.path[0] == 0) {
			fprintf(stderr,
				"VOLCONF: ILLEGAL PATH %d\n",
				msgconf->index);
			break;
		}
		conf += msgconf->index;
		pthread_mutex_lock(&confmtx);
		conf->copy_pend = msgconf->copy;
		conf->ctl |= H2CONFCTL_UPDATE;
		pthread_mutex_unlock(&confmtx);
		if (conf->thread_started == 0) {
			fprintf(stderr, "VOLCONF THREAD STARTED\n");
			pthread_cond_init(&conf->cond, NULL);
			pthread_create(&conf->thread, NULL,
				       hammer2_volconf_thread, (void *)conf);
			conf->thread_started = 1;
		}
		pthread_cond_signal(&conf->cond);
		break;
	default:
		if (unmanaged)
			dmsg_msg_reply(msg, DMSG_ERR_NOSUPP);
		break;
	}
}

static void *
hammer2_volconf_thread(void *info)
{
	hammer2_media_config_t *conf = info;

	setproctitle("hammer2 volconf");

	pthread_mutex_lock(&confmtx);
	while ((conf->ctl & H2CONFCTL_STOP) == 0) {
		if (conf->ctl & H2CONFCTL_UPDATE) {
			fprintf(stderr, "VOLCONF UPDATE\n");
			conf->ctl &= ~H2CONFCTL_UPDATE;
			if (bcmp(&conf->copy_run, &conf->copy_pend,
				 sizeof(conf->copy_run)) == 0) {
				fprintf(stderr, "VOLCONF: no changes\n");
				continue;
			}

			pthread_mutex_unlock(&confmtx);
			hammer2_volconf_stop(conf);
			conf->copy_run = conf->copy_pend;
			if (conf->copy_run.copyid != 0 &&
			    strncmp((char*)conf->copy_run.path, "span:", 5) == 0) {
				hammer2_volconf_start(conf,
						      (char*)conf->copy_run.path + 5);
			}
			pthread_mutex_lock(&confmtx);
			fprintf(stderr, "VOLCONF UPDATE DONE state %d\n", conf->state);
		}
		if (conf->state == H2MC_CONNECT) {
			hammer2_volconf_start(conf, (char*)conf->copy_run.path + 5);
			pthread_mutex_unlock(&confmtx);
			sleep(5);
			pthread_mutex_lock(&confmtx);
		} else {
			pthread_cond_wait(&conf->cond, &confmtx);
		}
	}
	pthread_mutex_unlock(&confmtx);
	hammer2_volconf_stop(conf);
	return(NULL);
}

static
void
hammer2_volconf_start(hammer2_media_config_t *conf, const char *hostname)
{
	dmsg_master_service_info_t *info;

	switch(conf->state) {
	case H2MC_STOPPED:
	case H2MC_CONNECT:
		conf->fd = dmsg_connect(hostname);
		if (conf->fd < 0) {
			fprintf(stderr, "Unable to connect to %s\n", hostname);
			conf->state = H2MC_CONNECT;
		} else if (pipe(conf->pipefd) < 0) {
			close(conf->fd);
			fprintf(stderr, "pipe() failed during volconf\n");
			conf->state = H2MC_CONNECT;
		} else {
			fprintf(stderr, "VOLCONF CONNECT\n");
			info = malloc(sizeof(*info));
			bzero(info, sizeof(*info));
			info->fd = conf->fd;
			info->altfd = conf->pipefd[0];
			info->altmsg_callback = hammer2_volconf_signal;
			info->usrmsg_callback = hammer2_usrmsg_handler;
			info->detachme = 0;
			conf->state = H2MC_RUNNING;
			pthread_create(&conf->iocom_thread, NULL,
				       dmsg_master_service, info);
		}
		break;
	case H2MC_RUNNING:
		break;
	}
}

static
void
hammer2_volconf_stop(hammer2_media_config_t *conf)
{
	switch(conf->state) {
	case H2MC_STOPPED:
		break;
	case H2MC_CONNECT:
		conf->state = H2MC_STOPPED;
		break;
	case H2MC_RUNNING:
		close(conf->pipefd[1]);
		conf->pipefd[1] = -1;
		pthread_join(conf->iocom_thread, NULL);
		conf->state = H2MC_STOPPED;
		break;
	}
}

static
void
hammer2_volconf_signal(dmsg_iocom_t *iocom)
{
	atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
}

static
void *
udev_thread(void *data __unused)
{
	int	fd;

	pthread_detach(pthread_self());
	setproctitle("hammer2 udev_thread");

	if ((fd = open(UDEV_DEVICE_PATH, O_RDWR)) < 0) {
		fprintf(stderr, "udev_thread: unable to open \"%s\"\n",
			UDEV_DEVICE_PATH);
		pthread_exit(NULL);
	}
	udev_check_disks();
#if 0
	while (ioctl(fd, UDEVWAIT, &seq) == 0) {
		udev_check_disks();
		sleep(1);
	}
#endif
	return (NULL);
}

static void *autoconn_connect_thread(void *data);
static void autoconn_disconnect_signal(dmsg_iocom_t *iocom);

static
void *
autoconn_thread(void *data __unused)
{
	TAILQ_HEAD(, autoconn) autolist;
	struct autoconn *ac;
	struct autoconn *next;
	pthread_t thread;
	struct stat st;
	time_t	t;
	time_t	lmod;
	int	found_last;
	FILE	*fp;
	char	buf[256];

	TAILQ_INIT(&autolist);
	found_last = 0;
	lmod = 0;

	pthread_detach(pthread_self());
	setproctitle("hammer2 autoconn_thread");
	for (;;) {

		sleep(5);

		if (stat(HAMMER2_DEFAULT_DIR "/autoconn", &st) == 0) {
			if (lmod == st.st_mtime)
				continue;
			fp = fopen(HAMMER2_DEFAULT_DIR "/autoconn", "r");
			if (fp == NULL)
				continue;
		} else {
			if (lmod == 0)
				continue;
			fp = NULL;
		}

		if (fp == NULL && found_last) {
			found_last = 0;
			continue;
		} else if (fp && found_last == 0) {
			fclose(fp);
			found_last = 1;
			continue;
		}

		time(&t);
		if (fp) {
			if (t == st.st_mtime) {
				fclose(fp);
				continue;
			}
			t = st.st_mtime;
		} else {
			t = 0;
		}

		TAILQ_FOREACH(ac, &autolist, entry)
			ac->stage = 0;
		while (fp && fgets(buf, sizeof(buf), fp) != NULL) {
			char *host;

			if ((host = strtok(buf, " \t\r\n")) == NULL ||
			    host[0] == '#') {
				continue;
			}
			TAILQ_FOREACH(ac, &autolist, entry) {
				if (strcmp(host, ac->host) == 0)
					break;
			}
			if (ac == NULL) {
				ac = malloc(sizeof(*ac));
				bzero(ac, sizeof(*ac));
				ac->host = strdup(host);
				ac->state = AUTOCONN_INACTIVE;
				TAILQ_INSERT_TAIL(&autolist, ac, entry);
			}
			ac->stage = 1;
		}

		if (fp) {
			if (fstat(fileno(fp), &st) < 0) {
				fclose(fp);
				continue;
			}
			fclose(fp);
			if (t != st.st_mtime)
				continue;
		}

		lmod = t;
		next = TAILQ_FIRST(&autolist);
		while ((ac = next) != NULL) {
			next = TAILQ_NEXT(ac, entry);

			if (ac->stage && ac->state == AUTOCONN_INACTIVE) {
				if (pipe(ac->pipefd) == 0) {
					ac->stopme = 0;
					ac->state = AUTOCONN_ACTIVE;
					pthread_create(&thread, NULL,
						       autoconn_connect_thread,
						       ac);
				}
			}

			if (ac->stage == 0 &&
			    ac->state == AUTOCONN_ACTIVE) {
				if (ac->stopme == 0) {
					char dummy = 0;
					ac->stopme = 1;
					write(ac->pipefd[1], &dummy, 1);
				}
			}

			if (ac->stage == 0 &&
			    ac->state == AUTOCONN_INACTIVE) {
				TAILQ_REMOVE(&autolist, ac, entry);
				free(ac->host);
				free(ac);
				continue;
			}
		}
		sleep(5);
	}
	return(NULL);
}

static
void *
autoconn_connect_thread(void *data)
{
	dmsg_master_service_info_t *info;
	struct autoconn *ac;
	void *res;
	int fd;

	ac = data;
	pthread_detach(pthread_self());
	setproctitle("hammer2 dmsg");

	while (ac->stopme == 0) {
		fd = dmsg_connect(ac->host);
		if (fd < 0) {
			if (DMsgDebugOpt > 2) {
				fprintf(stderr,
					"autoconn: Connect failure: %s\n",
					ac->host);
			}
			sleep(5);
			continue;
		}
		fprintf(stderr, "autoconn: Connect %s\n", ac->host);

		info = malloc(sizeof(*info));
		bzero(info, sizeof(*info));
		info->fd = fd;
		info->altfd = ac->pipefd[0];
		info->altmsg_callback = autoconn_disconnect_signal;
		info->usrmsg_callback = hammer2_usrmsg_handler;
		info->detachme = 0;
		info->noclosealt = 1;
		pthread_create(&ac->thread, NULL, dmsg_master_service, info);
		pthread_join(ac->thread, &res);
	}
	close(ac->pipefd[0]);
	ac->state = AUTOCONN_INACTIVE;

	return(NULL);
}

static
void
autoconn_disconnect_signal(dmsg_iocom_t *iocom)
{
	fprintf(stderr, "autoconn: Shutting down socket\n");
	atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
}

static
void
udev_check_disks(void)
{
	char tmpbuf[1024];
	char *buf = NULL;
	char *disk;
	int error;
	size_t n;

	for (;;) {
		n = 0;
		error = sysctlbyname("kern.disks", NULL, &n, NULL, 0);
		if (error < 0 || n == 0)
			break;
		if (n >= sizeof(tmpbuf))
			buf = malloc(n + 1);
		else
			buf = tmpbuf;
		error = sysctlbyname("kern.disks", buf, &n, NULL, 0);
		if (error == 0) {
			buf[n] = 0;
			break;
		}
		if (buf != tmpbuf) {
			free(buf);
			buf = NULL;
		}
		if (errno != ENOMEM)
			break;
	}
	if (buf) {
		fprintf(stderr, "DISKS: %s\n", buf);
		for (disk = strtok(buf, WS); disk; disk = strtok(NULL, WS)) {
			disk_reconnect(disk);
		}
		if (buf != tmpbuf)
			free(buf);
	}
}

static
void
master_reconnect(const char *mntpt)
{
	struct hammer2_ioc_recluster recls;
	dmsg_master_service_info_t *info;
	pthread_t thread;
	int fd;
	int pipefds[2];

	fd = open(mntpt, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "reconnect %s: no access to mount\n", mntpt);
		return;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipefds) < 0) {
		fprintf(stderr, "reconnect %s: socketpair() failed\n", mntpt);
		close(fd);
		return;
	}
	bzero(&recls, sizeof(recls));
	recls.fd = pipefds[0];
	if (ioctl(fd, HAMMER2IOC_RECLUSTER, &recls) < 0) {
		fprintf(stderr, "reconnect %s: ioctl failed: %s\n",
			mntpt, strerror(errno));
		close(pipefds[0]);
		close(pipefds[1]);
		close(fd);
		return;
	}
	close(pipefds[0]);
	close(fd);

	if (QuietOpt == 0)
		fprintf(stderr, "reconnect %s: connected to kernel\n", mntpt);

	info = malloc(sizeof(*info));
	bzero(info, sizeof(*info));
	info->fd = pipefds[1];
	info->detachme = 1;
	info->usrmsg_callback = hammer2_usrmsg_handler;
	info->label = strdup("hammer2");
	pthread_create(&thread, NULL, dmsg_master_service, info);
}

static
void
disk_reconnect(const char *disk)
{

	struct diskcon *dc;
	dmsg_master_service_info_t *info;
	pthread_t thread;
	int fd;
	int pipefds[2];
	char *path;

	if (strncmp(disk, "md", 2) == 0)
		return;
	if (strncmp(disk, "xa", 2) == 0)
		return;

	pthread_mutex_lock(&diskmtx);
	TAILQ_FOREACH(dc, &diskconq, entry) {
		if (strcmp(dc->disk, disk) == 0)
			break;
	}
	pthread_mutex_unlock(&diskmtx);
	if (dc)
		return;

	asprintf(&path, "/dev/%s", disk);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "reconnect %s: no access to disk\n", disk);
		free(path);
		return;
	}
	free(path);
	if (pipe(pipefds) < 0) {
		fprintf(stderr, "reconnect %s: pipe() failed\n", disk);
		close(fd);
		return;
	}
#if 0
	bzero(&recls, sizeof(recls));
	recls.fd = pipefds[0];
	if (ioctl(fd, DIOCRECLUSTER, &recls) < 0) {
		fprintf(stderr, "reconnect %s: ioctl failed\n", disk);
		close(pipefds[0]);
		close(pipefds[1]);
		close(fd);
		return;
	}
#endif
	close(pipefds[0]);
	close(fd);

	dc = malloc(sizeof(*dc));
	dc->disk = strdup(disk);
	pthread_mutex_lock(&diskmtx);
	TAILQ_INSERT_TAIL(&diskconq, dc, entry);
	pthread_mutex_unlock(&diskmtx);

	info = malloc(sizeof(*info));
	bzero(info, sizeof(*info));
	info->fd = pipefds[1];
	info->detachme = 1;
	info->usrmsg_callback = hammer2_usrmsg_handler;
	info->exit_callback = disk_disconnect;
	info->handle = dc;
	info->label = strdup(dc->disk);
	pthread_create(&thread, NULL, dmsg_master_service, info);
}

static
void
disk_disconnect(void *handle)
{
	struct diskcon *dc = handle;

	fprintf(stderr, "DISK_DISCONNECT %s\n", dc->disk);

	pthread_mutex_lock(&diskmtx);
	TAILQ_REMOVE(&diskconq, dc, entry);
	pthread_mutex_unlock(&diskmtx);
	free(dc->disk);
	free(dc);
}

static
void
xdisk_connect(void)
{
	dmsg_master_service_info_t *info;

	pthread_t thread;
	int pipefds[2];
	int xfd;
	int error;

	xfd = open("/dev/xdisk", O_RDWR, 0600);
	if (xfd < 0) {
		fprintf(stderr, "xdisk_connect: Unable to open /dev/xdisk\n");
		return;
	}

	if (pipe(pipefds) < 0) {
		fprintf(stderr, "xdisk_connect: pipe() failed\n");
		return;
	}

	info = malloc(sizeof(*info));
	bzero(info, sizeof(*info));
	info->fd = pipefds[1];
	info->detachme = 1;
	info->usrmsg_callback = hammer2_usrmsg_handler;
	info->exit_callback = NULL;
	pthread_create(&thread, NULL, dmsg_master_service, info);

#if 0
	bzero(&xaioc, sizeof(xaioc));
	xaioc.fd = pipefds[0];
	error = ioctl(xfd, XDISKIOCATTACH, &xaioc);
#endif
	error = -1;
	close(pipefds[0]);
	close(xfd);

	if (error < 0) {
		fprintf(stderr,
			"xdisk_connect: cannot attach %s\n",
			strerror(errno));
		return;
	}
}

void
hammer2_demon(void *(*func)(void *), void *arg)
{
	pthread_t thread;
	pid_t pid;
	int ttyfd;

	if (DebugOpt) {
                pthread_create(&thread, NULL, func, arg);
		NormalExit = 0;
		return;
	}

	if ((pid = fork()) < 0) {
		fprintf(stderr, "hammer2: fork(): %s\n", strerror(errno));
		exit(1);
	}
	if (pid > 0) {
		while (waitpid(pid, NULL, 0) != pid)
			;
		return;
	}

	ttyfd = open("/dev/null", O_RDWR);
	if (ttyfd >= 0) {
		if (ttyfd != 0)
			dup2(ttyfd, 0);
		if (ttyfd != 1)
			dup2(ttyfd, 1);
		if (ttyfd != 2)
			dup2(ttyfd, 2);
		if (ttyfd > 2)
			close(ttyfd);
	}

	ttyfd = open("/dev/tty", O_RDWR);
	if (ttyfd >= 0) {
		ioctl(ttyfd, TIOCNOTTY, 0);
		close(ttyfd);
	}
	setsid();

	if ((pid = fork()) < 0) {
		_exit(2);
	}
	if (pid > 0)
		_exit(0);

	setsid();
	pthread_create(&thread, NULL, func, arg);
	pthread_exit(NULL);
	_exit(2);
}
