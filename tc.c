/* program must be invoked from /etc/inittab
 */
#include <errno.h>
#include <ctype.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdio.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <netinet/tcp.h>
#include <sys/klog.h>

#include "error.h"
#include "sources.h"

#define DEFAULT_LISTEN_PORT 4095
#define LEN_WRITEBUFFER 4096
//#define MAXCONN SOMAXCONN
#define MAXCONN 1

#define min(x, y) ((x) < (y) ? (x) : (y))

typedef struct
{
	int sysrq_fd;
	int vcsa0_fd;	/* virtual console 0 */

	char *dmesg_buffer;
	int dmesg_buffer_size;

} parameters_t;

int WRITE(int sock, char *s, int len)
{
	while(len > 0)
	{
		int rc = write (sock, s, len);

		if (rc == -1)
		{
			if (errno == EINTR)
				continue;

			return -1;
		}
		if (rc == 0)
			return -1;

		len -= rc;
		s += rc;
	}

	return 0;
}

int sockprint(int fd, char *format, ...)
{
	int len;
	static char buffer[LEN_WRITEBUFFER];
	va_list ap;

	va_start(ap, format);
	len = vsnprintf(buffer, sizeof(buffer), format, ap);
	va_end(ap);

	return WRITE(fd, buffer, len);
}

int readchar(int fd)
{
	for(;;)
	{
		char key;
		int rc = read(fd, &key, 1);

		if (rc == -1)
		{
			if (errno == EINTR)
				continue;

			break;
		}
		else if (rc == 0)
		{
			break;
		}

		return key;
	}

	return -1;
}

int flush_socket(int fd)
{
	for(;;)
	{
		static char buffer[16];
		int rc;
		struct timeval tv;
		fd_set rfds;

		tv.tv_sec = 0;
		tv.tv_usec = 100;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);

		rc = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (rc == -1)
		{
			if (errno == EINTR)
				continue;

			return -1;
		}
		if (rc == 0)
			break;

		if (FD_ISSET(fd, &rfds))
		{
			rc = read(fd, buffer, sizeof(buffer));
			if(rc == 0) break;
		}
	}

	return 0;
}

static char userinput_buffer[128];

char *get_string(int fd)
{
	size_t len = 0;

	if (flush_socket(fd) == -1)
		return NULL;

	do
	{
		int key = readchar(fd);
		if (key == -1)
			return NULL;

		if (key == 10 || key == 13)
			break;

		userinput_buffer[len++] = key;
	}
	while(len < (sizeof(userinput_buffer) - 1));
	userinput_buffer[len] = 0x00;

	return userinput_buffer;
}

int ec_help(int fd)
{
	int rc = 0;

	rc |= sockprint(fd, "tcpconsole v " VERSION ", (C) 2009-2012 by folkert@vanheusden.com\n");
	rc |= sockprint(fd, "1-8: set dmesg loglevel\n");
	rc |= sockprint(fd, "d: dump virtual console 0\n");
	rc |= sockprint(fd, "i: show system load\n");
	rc |= sockprint(fd, "j: 'kill -9' for a given pid\n");
	rc |= sockprint(fd, "k: 'kill -9' for a given name\n");
	rc |= sockprint(fd, "t: 'kill -SIGSTOP' everything (almost)\n");
	rc |= sockprint(fd, "c: 'kill -SIGCONT' for a given pid\n");
	rc |= sockprint(fd, "v: 'kill -SIGCONT' for a given name\n");
	rc |= sockprint(fd, "l: dump dmesg logs\n");
	rc |= sockprint(fd, "m: dump dmesg & clear dmesg buffer\n");
	rc |= sockprint(fd, "p: process list\n");
	rc |= sockprint(fd, "q: log off\n");
	rc |= sockprint(fd, "s: start sshd\n");
	rc |= sockprint(fd, "a: dump tcpconsole source code\n");
	rc |= sockprint(fd, "\nSysreq:\n");
	rc |= sockprint(fd, "B - reboot\n");
	rc |= sockprint(fd, "C - crash\n");
	rc |= sockprint(fd, "D - list all locks\n");
	rc |= sockprint(fd, "E - SIGTERM to all but init\n");
	rc |= sockprint(fd, "F - call oom_kill\n");
	rc |= sockprint(fd, "I - SIGKILL to all but init\n");
	rc |= sockprint(fd, "L - backtrace\n");
	rc |= sockprint(fd, "M - memory info dump\n");
	rc |= sockprint(fd, "P - register/flags dump\n");
	rc |= sockprint(fd, "O - switch off\n");
	rc |= sockprint(fd, "Q - list hrtimers\n");
	rc |= sockprint(fd, "S - SYNC\n");
	rc |= sockprint(fd, "T - tasklist dump\n");
	rc |= sockprint(fd, "U - umount\n");
	rc |= sockprint(fd, "W - unint. tasks dump\n");

	return rc;
}

int sockerror(int fd, char *what)
{
	return sockprint(fd, "error on %s: %s (%d)\n", what, strerror(errno), errno);
}

int dump_virtual_console(int fd_out, int fd_in)
{
	struct{ char lines, cols, x, y; } scrn;
	int x, y;

	if (lseek(fd_in, 0, SEEK_SET) == -1)
		return sockerror(fd_out, "lseek");

	if (read(fd_in, &scrn, 4) == -1)
		return sockerror(fd_out, "read on vcs");

	for(y=0; y<scrn.lines; y++)
	{
		int nspaces = 0;

		for(x=0; x<scrn.cols; x++)
		{
			int loop;
			char ca[2];

			if (read(fd_in, ca, 2) == -1)
				return sockerror(fd_out, "read on vcs (data)");

			if (ca[0] != ' ')
			{
				for(loop=0; loop<nspaces; loop++)
					sockprint(fd_out, " ");
				nspaces = 0;

				sockprint(fd_out, "%c", ca[0]);
			}
			else
			{
				nspaces++;
			}
		}

		if (sockprint(fd_out, "\n") == -1)
			return -1;
	}

	return 0;
}

int dump_dmesg(int fd, char *dmesg_buffer, int dmesg_buffer_size, char clear)
{
	int loop;
	int nread = klogctl(clear?4:3, dmesg_buffer, dmesg_buffer_size);
	if (nread <= 0)
		return sockerror(fd, "klogctl(3)");

	dmesg_buffer[nread] = 0x00;

	for(loop=0; loop<nread; loop++)
	{
		if ((dmesg_buffer[loop] < 32 && dmesg_buffer[loop] != 10) || dmesg_buffer[loop] > 126)
			dmesg_buffer[loop] = ' ';
	}

	return WRITE(fd, dmesg_buffer, nread);
}

int set_dmesg_loglevel(int fd, int level)
{
	if (klogctl(8, NULL, level) == -1)
		return sockerror(fd, "klogctl(8)");

	return sockprint(fd, "dmesg loglevel set to %d\n", level);
}

int dump_loadavg(int fd)
{
	double avg[3];

	if (getloadavg(avg, 3) == -1)
		return sockerror(fd, "getloadavg(3)");

	return sockprint(fd, "load: 1min: %f, 5min: %f, 15min: %f\n", avg[0], avg[1], avg[2]);
}

int dump_ps(int fd)
{
	int rc = 0, tnprocs = 0, tnthreads = 0;
	struct dirent *de;
	DIR *dirp = opendir("/proc");

	while((de = readdir(dirp)) != NULL)
	{
		if (isdigit(de -> d_name[0]))
		{
			FILE *fh;
			static char path[128];
			static char statusattrname[128];
			static int ruid=-1, euid=-1, suid=-1, fuid=-1;
			
			snprintf(path, sizeof(path), "/proc/%s/status", de -> d_name);
			fh = fopen(path, "r");
			
			if (fh)
			{
				while(!feof(fh))
				{
					fscanf(fh, "%s", statusattrname);
					if(strcmp(statusattrname, "Uid:")==0)
					{	/* Uid: Real, effective, saved set, and filesystem UIDs */
						fscanf(fh, "%d %d %d %d", &ruid, &euid, &suid, &fuid);
					}
					else
					{	/* Skip this line */
						static char c;
						while(!feof(fh) && (c = fgetc(fh)) != '\n');
					}
				}
			}
			
			if (fh)
			{
				fclose(fh);
				
				snprintf(path, sizeof(path), "/proc/%s/stat", de -> d_name);
				fh = fopen(path, "r");
			}
			
			if (fh)
			{
				static char state;
				static int dummy, nthreads, ppid, rss;
				static long pos = 0;
				{
					// Find the last ")" char in stat file and parse fields thereafter.
					#define RIGHTBRACKET ')'
					while(1)
					{
						state = fgetc(fh);
						if (state == EOF) break;
						if (state == RIGHTBRACKET) pos = ftell(fh);
					}
					fseek(fh, pos, 0);
				}
				fscanf(fh, " %c %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &state, &ppid, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &nthreads, &dummy, &dummy, &dummy, &rss);

				rc |= sockprint(fd, "%5s ppid %5d ruid %4d euid %4d thrds %2d rss %5d %c ", de -> d_name, ppid, ruid, euid, nthreads, rss, state);

				tnprocs++;
				tnthreads += nthreads;
			}

			if (fh)
			{
				fclose(fh);

				snprintf(path, sizeof(path), "/proc/%s/cmdline", de -> d_name);
				fh = fopen(path, "r");
			}

			if (fh)
			{
				int len, loop;
				static char cmdline[4096];

				len = fread(cmdline, 1, sizeof(cmdline) - 1, fh);
				if (len < 0)
					rc |= sockerror(fd, "fread");
				else
				{
					cmdline[len] = 0x00;
					for(loop=0; loop<len; loop++)
					{
						if (cmdline[loop] == 0x00)
							cmdline[loop] = ' ';
					}
					rc |= sockprint(fd, "[%d] %s\n", len, cmdline);
				}

				fclose(fh);
			}
			else
			{
				rc |= sockprint(fd, "Error opening %s\n", path);
			}

			if (rc) break;
		}
	}

	closedir(dirp);

	rc |= sockprint(fd, "# procs: %d, # threads: %d\n", tnprocs, tnthreads);

	return rc;
}

int do_sysreq(int fd, char key, int sysreq_fd)
{
	int yn;

	if (key < 'a' || key > 'z')
		return sockprint(fd, "key out of range\n");

	if (sockprint(fd, "Send %c to sysreq? (y/n)\n", key) == -1)
		return -1;

	do
	{
		yn = readchar(fd);
	}
	while(yn != 'y' && yn != 'n' && yn != -1);

	if (yn == 'y')
	{
		if (WRITE(sysreq_fd, &key, 1) == -1)
			return sockerror(fd, "WRITE(sysreq_fd)");
	}

	return yn == -1 ? -1 : 0;
}

int kill_one_proc(int client_fd)
{
	int rc = 0;
	pid_t pid;
	char *entered;

	if (sockprint(client_fd, "Process id (PID, q to abort): ") == -1)
		return -1;

	entered = get_string(client_fd);
	if (!entered)
		return -1;

	if (strcmp(entered, "q") == 0)
	{
		return 0;
	}

	pid = atoi(entered);
	rc = sockprint(client_fd, "Killing pid %d\n", pid);

	if (kill(pid, SIGKILL) == -1)
		rc |= sockerror(client_fd, "kill(-9)");

	return rc;
}

int cont_one_proc(int client_fd)
{
	int rc = 0;
	pid_t pid;
	char *entered;

	if (sockprint(client_fd, "Process id (PID, q to abort): ") == -1)
		return -1;

	entered = get_string(client_fd);
	if (!entered)
		return -1;

	if (strcmp(entered, "q") == 0)
	{
		return 0;
	}

	pid = atoi(entered);
	rc = sockprint(client_fd, "Continuing pid %d\n", pid);

	if (kill(pid, SIGCONT) == -1)
		rc |= sockerror(client_fd, "kill(-SIGCONT)");

	return rc;
}

int cont_procs(int client_fd)
{
	char *entered;
	int nprocs = 0;

	if (sockprint(client_fd, "Process name (q to abort): ") == -1)
		return -1;

	entered = get_string(client_fd);
	if (!entered)
		return -1;

	if (strcmp(entered, "q") == 0)
	{
		return 0;
	}

	struct dirent *de;
	DIR *dirp = opendir("/proc");
	while((de = readdir(dirp)) != NULL)
	{
		if (isdigit(de -> d_name[0]))
		{
			FILE *fh;
			static char path[24];

			snprintf(path, sizeof(path), "/proc/%s/comm", de -> d_name);
			fh = fopen(path, "r");
			if (fh)
			{
				static char comm[64];
				
				if (fgets(comm, sizeof(comm), fh) > 0)
				{
					if (strstr(comm, entered) != NULL)
					{
						pid_t pid = atoi(de -> d_name);
						if (sockprint(client_fd, "Continuing pid %d %s\n", pid, comm) == -1)
							break;

						if (kill(pid, SIGCONT) == -1)
						{
							if (sockerror(client_fd, "kill(-CONT)") == -1)
								break;
						}
						else
						{
							nprocs++;
						}
					}
				}
				fclose(fh);
			}
		}
	}
	closedir(dirp);

	return sockprint(client_fd, "Continued %d processes\n", nprocs);
}

int kill_procs(int client_fd)
{
	int nprocs = 0;
	struct dirent *de;
	char *entered;

	if (sockprint(client_fd, "Process name (q to abort): ") == -1)
		return -1;

	entered = get_string(client_fd);
	if (!entered)
		return -1;

	if (strcmp(entered, "q") == 0)
	{
		return 0;
	}

	if (sockprint(client_fd, "\nKilling process %s\n", entered) == -1)
	{
		return -1;
	}

	DIR *dirp = opendir("/proc");
	while((de = readdir(dirp)) != NULL)
	{
		if (isdigit(de -> d_name[0]))
		{
			FILE *fh;
			static char path[128];

			snprintf(path, sizeof(path), "/proc/%s/stat", de -> d_name);
			fh = fopen(path, "r");
			if (fh)
			{
				static char fname[4096], dummystr[2], *pdummy;
				int dummy;
				fscanf(fh, "%d %s %c %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &dummy, fname, &dummystr[0], &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy);

				pdummy = strrchr(fname, ')');
				if (pdummy) *pdummy = 0x00;
				pdummy = fname + 1;
				if (strcmp(pdummy, entered) == 0)
				{
					pid_t pid = atoi(de -> d_name);
					if (sockprint(client_fd, "Killing pid %d\n", pid) == -1)
						break;

					if (kill(pid, SIGKILL) == -1)
					{
						if (sockerror(client_fd, "kill(-9)") == -1)
							break;
					}

					nprocs++;
				}

				fclose(fh);
			}
		}
	}

	closedir(dirp);

	return sockprint(client_fd, "Killed %d processes\n", nprocs);
}

int stop_all_procs(int client_fd)
{
	int nprocs = 0;
	int nprocs_ok = 0;
	struct dirent *de;
	DIR *dirp = opendir("/proc");

	if (sockprint(client_fd, "\nStopping processes\n") == -1)
	{
		return -1;
	}

	while((de = readdir(dirp)) != NULL)
	{
		if (isdigit(de -> d_name[0]))
		{
			pid_t pid = atoi(de -> d_name);
			if (pid > 2 /* skip init:1 and kthreadd:2 */ && pid != getpid() /* and ourself */)
			{
				static char path[128];
				static char symlinktarget_buf[12];
				snprintf(path, sizeof(path), "/proc/%d/exe", pid);
				if (readlink(path, symlinktarget_buf, 12) == -1)
				{
					if (errno == ENOENT) /* skip kernel thread */ continue;
				}
				else
				{
					if (strcmp(symlinktarget_buf, "/sbin/getty") == 0) /* skip getty */ continue;
				}
				
				if (sockprint(client_fd, "Stopping pid %d\n", pid) == -1)
					break;

				if(kill(pid, SIGSTOP) == -1)
				{
					// ignore error (eg. process exited)
				}
				else
				{
					nprocs_ok++;
				}
				nprocs++;
			}
		}
	}

	closedir(dirp);

	return sockprint(client_fd, "Stopped %d processes (out of %d total)\n", nprocs_ok, nprocs);
}

int start_sshd(int fd)
{
	pid_t pid = fork();
	if(pid == -1)
	{
		return sockerror(fd, "fork(2)");
	}
	else if(pid == 0)
	{
		daemon(0, 0);
		execl("/usr/sbin/sshd", "/usr/sbin/sshd", NULL);
		_exit(-1); 
	}
	
	return sockprint(fd, "sshd started on pid %d\n", pid);
}

void serve_client(int fd, parameters_t *pars)
{
	sockprint(fd, "Enter 'h' for help\n");

	for(;;)
	{
		int key;
		size_t index;

		if (sockprint(fd, "emergency console > ") == -1)
			break;

		if ((key = readchar(fd)) == -1)
			break;

		if (key < 32 || key > 126)
		{
			if (sockprint(fd, "\r") == -1)
				break;
			continue;
		}

		if (sockprint(fd, "%c\n", key) == -1)
			break;

		switch(key)
		{
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
				if (set_dmesg_loglevel(fd, key - '0') == -1)
					return;
				break;

			case 'd':
				if (dump_virtual_console(fd, pars -> vcsa0_fd) == -1)
					return;
				break;

			case '?':
			case 'h':
				if (ec_help(fd) == -1)
					return;
				break;

			case 'i':
				if (dump_loadavg(fd) == -1)
					return;
				break;

			case 'j':
				if (kill_one_proc(fd) == -1)
					return;
				break;

			case 'k':
				if (kill_procs(fd) == -1)
					return;
				break;

			case 'l':
				if (dump_dmesg(fd, pars -> dmesg_buffer, pars -> dmesg_buffer_size, 0) == -1)
					return;
				break;

			case 'm':
				if (dump_dmesg(fd, pars -> dmesg_buffer, pars -> dmesg_buffer_size, 1) == -1)
					return;
				break;

			case 'p':
				if (dump_ps(fd) == -1)
					return;
				break;

			case 'q':
				return;
			
			case 's':
				if (start_sshd(fd) == -1)
					return;
				break;

			case 't':
				if (stop_all_procs(fd) == -1)
					return;
				break;

			case 'c':
				if (cont_one_proc(fd) == -1)
					return;
				break;

			case 'v':
				if (cont_procs(fd) == -1)
					return;
				break;

			case 'a':
				index = 0;
				while (index < sizeof(SELF_SOURCE_CODE))
				{
					if (sockprint(fd, "%.*s", index+LEN_WRITEBUFFER>sizeof(SELF_SOURCE_CODE) ? sizeof(SELF_SOURCE_CODE)-index : LEN_WRITEBUFFER, &SELF_SOURCE_CODE[index]) == -1)
						return;
					index += LEN_WRITEBUFFER;
				}
				break;

			case 10:
			case 13:
				break;
			default:
				if (isupper(key))
					do_sysreq(fd, tolower(key), pars -> sysrq_fd);
				else
					sockprint(fd, "'%c' is not understood\n", key);
				break;
		}
	}
}

int verify_password(int client_fd, char *password)
{
	char *entered;
        char dont_auth[] = { 0xff, 0xf4, 0x25 };
        char suppress_goahead[] = { 0xff, 0xfb, 0x03 };
        char dont_linemode[] = { 0xff, 0xfe, 0x22 };
        char dont_new_env[] = { 0xff, 0xfe, 0x27 };
        char will_echo[] = { 0xff, 0xfb, 0x01 };
        char dont_echo[] = { 0xff, 0xfe, 0x01 };
        char noecho[] = { 0xff, 0xfd, 0x2d };

        WRITE(client_fd, suppress_goahead, sizeof suppress_goahead );
        WRITE(client_fd, dont_linemode, sizeof dont_linemode );
        WRITE(client_fd, dont_new_env, sizeof dont_new_env);
        WRITE(client_fd, will_echo, sizeof will_echo);
        WRITE(client_fd, dont_echo, sizeof dont_echo);

	if (sockprint(client_fd, "Password: ") == -1)
		return -1;

	entered = get_string(client_fd);
	if (!entered)
		return -1;

	if (strcmp(password, entered) == 0)
	{
		return 0;
	}

	return -1;
}

void kill_orphans()
{
	FILE *fd;
	unsigned int tcp_max_orphans;
	const char *ctrl_file = "/proc/sys/net/ipv4/tcp_max_orphans";
	
	fd = fopen(ctrl_file, "r");
	fscanf(fd, "%u", &tcp_max_orphans);
	fclose(fd);
	
	fd = fopen(ctrl_file, "w");
	fprintf(fd, "%u", 0);
	fclose(fd);
	
	usleep(500000);

	fd = fopen(ctrl_file, "w");
	fprintf(fd, "%u", tcp_max_orphans);
	fclose(fd);
}

void listen_on_socket(int port, parameters_t *pars, char *password)
{
	int server_fd;
	struct sockaddr_in server_addr;
	int on = 1, optlen, sec60 = 60;
	char trys = 0;

	memset(&server_addr, 0x00, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	server_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (server_fd == -1)
		error_exit(127, "error creating socket");

	try_bind:
	if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1)
	{
		/*
		port++;
		server_addr.sin_port = htons(port);

		if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1)
		{
			error_exit(-1, "bind() failed");
			port -= 2;
			goto try_bind;
		}
		*/

		trys++;
		if(trys > 2)
		{
			error_exit(-1, "bind() failed");
		}
		kill_orphans();
		goto try_bind;
	}

	if (listen(server_fd, MAXCONN))
		error_exit(127, "listen(%d) failed", MAXCONN);

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1)
		error_exit(127, "setsockopt(SO_REUSEADDR) failed");

	if (setsockopt(server_fd, IPPROTO_TCP, TCP_KEEPIDLE, &sec60, sizeof(sec60)) == -1)
		error_exit(127, "setsockopt(TCP_KEEPIDLE) failed");

	if (setsockopt(server_fd, IPPROTO_TCP, TCP_KEEPINTVL, &sec60, sizeof(sec60)) == -1)
		error_exit(127, "setsockopt(TCP_KEEPINTVL) failed");

	syslog(LOG_INFO, "Listening on %d", port);

	for(;;)
	{
		struct sockaddr_in client_addr;
		socklen_t client_addr_size = sizeof(client_addr);
		int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_size);
		if (client_fd == -1)
		{
			if (errno == EINTR)
				continue;

			sleep(1);
			continue;
		}

		optlen = sizeof(on);
		if(setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &on, optlen) == -1)
		{
			if (sockerror(client_fd, "setsockopt(SO_KEEPALIVE)") == -1)
			{
				close(client_fd);
				continue;
			}
		}

		if (verify_password(client_fd, password) == 0)
			serve_client(client_fd, pars);

		close(client_fd);
	}
}

void write_pidfile(char *file)
{
	FILE *pidf = fopen(file, "w");
	if (pidf == NULL)
		error_exit(127, "Error creating pid-file %s\n", file);

	fprintf(pidf, "%d\n", getpid());

	fclose(pidf);
}

int open_file(char *path, int mode)
{
	int fd = open(path, mode);
	if (fd == -1)
		error_exit(127, "Open_file(%s) failed", path);

	return fd;
}

char * read_password(char *file)
{
	char buffer[128], *pw, *lf;
	struct stat buf;

	if(stat(file, &buf) == -1)
	{
		syslog(LOG_WARNING, "No password set!");
		pw = strdup("");
		goto ret;
	}
	
	int fd = open_file(file, O_RDONLY), rc;

	if (fstat(fd, &buf) == -1)
		error_exit(127, "fstat(%s) failed", file);

	rc = read(fd, buffer, sizeof(buffer) - 1);
	if (rc == -1)
		error_exit(127, "error reading password");
	buffer[rc] = 0x00;

	lf = strchr(buffer, '\n');
	if (lf) *lf = 0x00;

	close(fd);

	pw = strdup(buffer);
	ret:
	if (!pw)
		error_exit(127, "strdup() failed");

	return pw;
}

int main(int argc, char **argv)
{
	char *password;
	int port;
	parameters_t pars;
	struct sched_param sched_par;

	if(argc <= 1 || sscanf(argv[1], "%d", &port) == 0)
		port = DEFAULT_LISTEN_PORT;

	openlog("tcpconsole", LOG_PERROR|LOG_NDELAY|LOG_NOWAIT|LOG_PID, LOG_DAEMON);

	if (getuid())
		error_exit(127, "This program must be invoked with root-rights.");

	password = read_password("/etc/tcpconsole.pw");

	if (signal(SIGTERM, SIG_IGN) == SIG_ERR)
		error_exit(127, "signal(SIGTERM) failed");

	if (signal(SIGHUP,  SIG_IGN) == SIG_ERR)
		error_exit(127, "signal(SIGHUP) failed");

	pars.sysrq_fd = open_file("/proc/sysrq-trigger", O_WRONLY);
	pars.vcsa0_fd = open_file("/dev/vcsa", O_RDONLY);

	if (setpriority(PRIO_PROCESS, 0, -10) == -1)
		error_exit(127, "Setpriority failed");

	if (nice(-20) == -1)
		error_exit(127, "Failed to set nice-value to -20");

	if (mlockall(MCL_CURRENT) == -1 || mlockall(MCL_FUTURE) == -1)
		error_exit(127, "Failed to lock program in core");

	memset(&sched_par, 0x00, sizeof(sched_par));
	sched_par.sched_priority = sched_get_priority_max(SCHED_RR);
	if (sched_setscheduler(0, SCHED_RR, &sched_par) == -1)
		error_exit(127, "Failed to set scheduler properties for this process");

	syslog(LOG_INFO, "tcpconsole started");

	write_pidfile("/var/run/tcpconsole.pid");

	if ((pars.dmesg_buffer_size = klogctl(10, NULL, 0)) == -1)
		error_exit(127, "klogctl(10) failed");
	pars.dmesg_buffer = (char *)malloc(pars.dmesg_buffer_size + 1);
	if (!pars.dmesg_buffer)
		error_exit(127, "malloc failure");

	listen_on_socket(port, &pars, password);

	return 1;
}
