/*
 * Outbound Protel dialer daemon for use with Asterisk softmodem
 *
 * Copyright (C) 2024, Naveen Albert
 *
 * Naveen Albert <asterisk@phreaknet.org>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Outbound Protel dialer daemon for use with Asterisk softmodem
 *
 * \author Naveen Albert <asterisk@phreaknet.org>
 *
 * The Asterisk Softmodem() application can function as a virtualized
 * modem suitable for certain low-speed applications.
 * This daemon receives the data from the softmodem and logs each
 * call to a separate file for further post-processing.
 * This eliminates the need to use a physical (external, internal, or USB) modem.
 *
 * This program does some rudimentary processing to correct corruption,
 * and to try to be able to disconnect as soon as it has received enough data,
 * but more rigorous post-processing should be done with the saved files.
 *
 * The primary advantage of using this program as the "modem driver"
 * is that it will disconnect as soon as it has received the expected
 * data, rather than waiting until the answering modem disconnects.
 * As long distance is often billed in 6-second increments, this can
 * significantly cut down your long distance usage.
 *
 * Use like so:
 *
 * $> proteld -p 8300 -f printouts
 *
 * Asterisk dialplan snippet:
 * [protel-modem-dial]
 * exten => _NXXNXXXXXX,1,Dial(PJSIP/outgoing/sip:1${EXTEN}@carrier.example.com,90,G(split))
 *	 same => n(split),Hangup()
 *	 same => n(callee),Set(TIMEOUT(absolute)=90)
 *   same => n,Softmodem(127.0.0.1,8300,v(Bell103)lf)
 *	 same => n,Hangup()
 *
 * *CLI> channel originate Local/3115552368@pts application Wait 1
 *
 * This program does not do much, if any, post processing to clean up and standardize received data (nor should it).
 * If you desire, post processing can be done by uploading the files to https://phreaknet.org/cocot/
 * Choose "proteld" file format.
 */

#define _GNU_SOURCE /* for memmem in string.h */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h> /* use sockaddr_in */
#include <arpa/inet.h> /* use inet_addr */
#include <getopt.h>
#include <fcntl.h>
#include <signal.h>
#include <assert.h>

static int listen_port = -1;
static int listen_local = 0;
static int debug_level = 0;
static char outputdir[512] = "";
static int log_to_file = 0;

static int calls_success = 0;
static int calls_total = 0;

#define DATA_LENGTH 54
#define DATA_STARS 8

#define is_d(x) (x == 'D')
#define TRUE(x) (1)

#define AUTOCORRECT(pos, c, condl, condr) \
	if (data[pos] != c) { \
		if ((condl(data[pos - 1])) && (condr(data[pos + 1]))) { \
			fprintf(stderr, "Autocorrecting pos %d to %c\n", pos, c); \
			data[pos] = c; \
		} else { \
			fprintf(stderr, "Position %d should be %c but could not autocorrect\n", pos, c); \
		} \
	}

static inline void autocorrect(char *restrict data)
{
	/* The payload is not uncommonly corrupted since there is no error correction at 300 baud.
	 * Certain "cosmetic" defects can be corrected, either based on the known format of the payload,
	 * or by cross-referencing previously uncorrupted payloads.
	 *
	 * Here, we do some minor "fixups" to standardize received data. */

	if (strlen(data) <= DATA_LENGTH) {
		/* Too short */
		return;
	}

	AUTOCORRECT(11, '*', isdigit, isdigit);
	AUTOCORRECT(17, '*', isdigit, is_d);
	AUTOCORRECT(24, '*', isdigit, isdigit);
	AUTOCORRECT(29, '*', isdigit, isdigit);
	AUTOCORRECT(33, '*', isdigit, isdigit);
	AUTOCORRECT(47, '*', isdigit, isdigit);
	AUTOCORRECT(53, '*', isdigit, TRUE);
}

static inline int data_done(char *restrict buf, int len)
{
	char *start, *tmp;
	int stars = 0;

	if (len < DATA_LENGTH) {
		return 0; /* Not enough data received yet */
	}

	start = tmp = memchr(buf, '*', len);
	if (!tmp) {
		return 0;
	}

	/* Calculate actual data length */
	len -= (start - buf);

	if (len < DATA_LENGTH) {
		return 0; /* Payload still too small */
	}

	autocorrect(start);

	/* There should be 8 '*' characters, 7 if we exclude the trailing '*',
	 * which isn't strictly necessary if we get everything up to that point successfully.
	 * The last one should be 53 bytes after the first one. */
	while (*tmp) {
		if (*tmp++ == '*') {
			stars++;
		}
	}
	if (stars < DATA_STARS - 1) {
		fprintf(stderr, "\nExpecting at least %d stars, got %d\n", DATA_STARS - 1, stars);
		return 0;
	}
	if (tmp < start + 53) { /* Not long enough */
		fprintf(stderr, "\nPayload is not long enough\n");
		return 0;
	}
	return 1;
}

static int save_data(const unsigned char *restrict buf, int len, int success)
{
	char filename[684];
	char *tmp;
	int fd;
	ssize_t wres;

	if (success) {
		/* Determine the phone number */
		tmp = memchr(buf, '*', len);
		assert(tmp != NULL);
		tmp++;
		snprintf(filename, sizeof(filename), "%s/%lu_%.*s.txt", outputdir, time(NULL), 10, tmp);
	} else {
		/* If we couldn't successfully infer the phone number,
		 * use the current timestamp to make a unique name.
		 * There's a small chance this filename might already exist,
		 * if this daemon is being used by multiple modems concurrently,
		 * so also add a random number for good measure.
		 */
		do {
			snprintf(filename, sizeof(filename), "%s/%lu_%d_R.txt", outputdir, time(NULL), rand() % 100000);
		} while (!access(filename, R_OK));
	}

	/* We're writing everything at once,
	 * so there's not much point in using a buffered write. */
	fd = open(filename, O_WRONLY | O_CREAT);
	if (fd < 0) {
		fprintf(stderr, "open(%s) failed: %s\n", filename, strerror(errno));
		return -1;
	}

	wres = write(fd, buf, len);
	if (wres != len) {
		fprintf(stderr, "Wanted to write %d bytes to %s, only wrote %lu: %s\n", len, filename, wres, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static void *handler(void *varg)
{
	int *fdptr = varg;
	int fd = *fdptr;
	unsigned char buf[512];
	char *pos = (char*) buf;
	int bytes_read = 0;
	size_t left = sizeof(buf);
	int i;
	int success = 0;
	int reset = 0;

	fprintf(stderr, "Call # %d: New connection on fd %d\n", ++calls_total, fd);

	for (;;) {
		/* Given it's a 300 baud modem,
		 * we're probably going to be reading
		 * from the socket byte by byte */
		int res = read(fd, pos, left - 1);
		if (res <= 0) {
			fprintf(stderr, "\nread(%d) returned %d: %s\n", fd, res, strerror(errno));
			break;
		}

		/* Print printable data as it's received over the socket from the modem
		 * XXX If there are concurrent connections, this could cause formatting issues due to interleaving... */
		for (i = 0; i < res; i++) {
			if (isprint(pos[i])) {
				fprintf(stdout, "%c", pos[i]);
				fflush(stdout);
			} else {
				fprintf(stderr, " [%d] ", pos[i]);
			}
		}

		pos += res;
		left -= res;
		bytes_read += res;

		/* A printout looks something like this (with byte values enclosed in [])
		 * The exact number of null bytes and non-printable characters is not exact.
		 *
		 * TC! [0] [0] [0] [144] [0] [0] [0] [0] *3115552368*43125*DD8822*1234*032*2312237122028*37090*
		 *
		 * After that, [1] [0] [0] [239/240] is typical.
		 *
		 * TC! (3 bytes)
		 * (3 0 bytes)
		 * (1 144 byte)
		 * (4 0 bytes)
		 * (54 data bytes)
		 * = 65 total bytes
		 *
		 * This usually repeats after 10-20 seconds.
		 * We can abort as soon as we have a full, uncorrupted printout.
		 */

		buf[bytes_read] = '\0'; /* Null terminate so we can use string comparison functions */
		if (data_done((char*) buf, bytes_read)) {
			success = 1;
			break;
		/* If we encounter [1] [0] [0] at this point, reset and wait again
		 * Only look AFTER the payload, which is why we skip the first 30. */
		} else if (bytes_read > DATA_LENGTH && (memmem(buf + 30, bytes_read - 30, "\x01\x00\x00", 3) || memmem(buf + 30, bytes_read - 30, "\x00\x00\x00", 3))) {
			/* Payload was probably corrupted.
			 * Reset and see if it comes through the second time. */
			if (++reset == 2) {
				fprintf(stderr, "\nDuplicate corruption, aborting\n");
				/* We already got 2 printouts, there won't be any more,
				 * so disconnect immediately. */
				break;
			}
			fprintf(stderr, "\nResetting buffer (data corrupted)\n");
			bytes_read = 0;
			left = sizeof(buf);
			pos = (char*) buf;
		} else if (left <= 1) {
			fprintf(stderr, "Buffer truncation occurred\n");
		}
	}

	/* Close the socket as soon as we can
	 * to force the modem to disconnect,
	 * and end the phone call. */
	close(fd);

	if (log_to_file) {
		/* Create the log file now,
		 * since we can infer the phone number
		 * from the data itself (if success). */
		save_data(buf, bytes_read, success);
	}

	calls_success += success;

	fprintf(stderr, "\n");
	return NULL;
}

static void sigint_handler(int sig)
{
	(void) sig;
	fprintf(stderr, "\n");
	fprintf(stderr, "%-16s: %5d\n", "Calls Processed", calls_total);
	fprintf(stderr, "%-16s: %5d\n", "Calls Succeeded", calls_success);
	exit(EXIT_SUCCESS);
}

static int parse_options(int argc, char *argv[])
{
	static const char *getopt_settings = "f:lhpv";
	int c;

	while ((c = getopt(argc, argv, getopt_settings)) != -1) {
		switch (c) {
		case 'f':
			strncpy(outputdir, optarg, sizeof(outputdir) - 1);
			outputdir[sizeof(outputdir) - 1] = '\0';
			log_to_file = 1;
			break;
		case 'l':
			listen_local = 1;
			break;
		case 'h':
			fprintf(stderr, "proteld [-options]\n");
			fprintf(stderr, "   -f directory   Log printouts to this directory\n");
			fprintf(stderr, "   -l             Listen only on localhost\n");
			fprintf(stderr, "   -p port        Port on which to listen\n");
			fprintf(stderr, "   -v             Increase verbosity\n");
			return -1;
		case 'p':
			listen_port = atoi(argv[optind++]);
			break;
		case 'v':
			debug_level++;
			break;
		default:
			fprintf(stderr, "Unknown option: %c\n", c);
			return -1;
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct sockaddr_in sinaddr;
	socklen_t len;
	int sfd, res;
	int sock;
	const int enable = 1;

	if (parse_options(argc, argv)) {
		return -1;
	} else if (listen_port == -1) {
		fprintf(stderr, "Must specify a port: tcplog -p <port>\n");
		return -1;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		fprintf(stderr, "Unable to create TCP socket: %s\n", strerror(errno));
		return -1;
	}

	/* Allow reuse so we can rerun quickly */
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
		fprintf(stderr, "Unable to create setsockopt: %s\n", strerror(errno));
		return -1;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0) {
		fprintf(stderr, "Unable to create setsockopt: %s\n", strerror(errno));
		return -1;
	}

	memset(&sinaddr, 0, sizeof(sinaddr));
	sinaddr.sin_family = AF_INET;
	/* Using INADDR_LOOPBACK doesn't always work but inet_addr("127.0.0.1") does... */
	sinaddr.sin_addr.s_addr = listen_local ? inet_addr("127.0.0.1") : INADDR_ANY;
	sinaddr.sin_port = htons(listen_port);

	if (bind(sock, (struct sockaddr *) &sinaddr, sizeof(struct sockaddr_in))) {
		fprintf(stderr, "Unable to bind TCP socket to port %d: %s\n", listen_port, strerror(errno));
		close(sock);
		return -1;
	}

	if (listen(sock, 2) < 0) {
		fprintf(stderr, "Unable to listen on TCP socket on port %d: %s\n", listen_port, strerror(errno));
		close(sock);
		return -1;
	}

	signal(SIGINT, sigint_handler);

	fprintf(stderr, "Listening on port %d\n", listen_port);

	for (;;) {
		pthread_attr_t attr;
		pthread_t thread;
		sfd = accept(sock, (struct sockaddr *) &sinaddr, &len);
		if (sfd < 0) {
			if (errno != EINTR) {
				fprintf(stderr, "accept returned %d: %s\n", sfd, strerror(errno));
				break;
			}
			continue;
		}

		/* Make the thread detached, since we're not going to join it, ever */
		pthread_attr_init(&attr);
		res = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (res) {
			fprintf(stderr, "pthread_attr_setdetachstate: %s\n", strerror(res));
			close(sfd);
			continue;
		}
		if (pthread_create(&thread, &attr, handler, &sfd)) {
			fprintf(stderr, "pthread_create failed: %s\n", strerror(errno));
			close(sfd);
		}
		usleep(100000); /* Wait for thread to start and dereference sfd before we accept() and overwrite it */
	}

	close(sock);
	fprintf(stderr, "Listener thread has exited\n");
}
