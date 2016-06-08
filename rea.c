#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <signal.h>
#include <netdb.h>
#include <sys/epoll.h>
#include "rea.h"

fd_set rfds;
fd_set wfds;
Client *clients[MAX_CLIENTS];

#define MAX_EVENTS 100

int main(int argc, char *argv[])
{
	int status, maxfd, i, fd, added, epfd;
	Client *c;


	if (argc != 2) {
		fprintf(stderr, "Usage: %s <port>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	setupAndListen(argv[1]);

	// epoll
	//
	struct epoll_event ev;
	struct epoll_event evs[MAX_EVENTS];
	int nfds;
	epfd = epoll_create1(0);
	if (epfd == -1) {
		err(EXIT_FAILURE, "epoll_create error");
	}

	ev.events = EPOLLIN;
	ev.data.fd = server->fd;

	status = epoll_ctl(epfd, EPOLL_CTL_ADD, server->fd, &ev);
	if (status == -1) {
		err(EXIT_FAILURE, "epoll_ctl error");
	}

	//epoll wait
	for (;;) {
		nfds = epoll_wait(epfd, evs, MAX_EVENTS, -1);
		fd = accept4(server->fd, server->addr->ai_addr,
				&(server->addr->ai_addrlen), SOCK_NONBLOCK);
		// handle accept4 error?
		//
		for (i = 0; i < nfds; i++) {
			if (evs[i].data.) {
			}
		}



		printf("foo! %d\n", nfds);
	}
	

	while(1) {
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(server->fd, &rfds);
		maxfd = server->fd;

		for (i = 0; i < MAX_CLIENTS; i++) {
			c = clients[i];

			if (c) {
				if (c->to_reply == 1) {
					FD_SET(c->fd, &wfds);
					c->to_reply = 0;
				} else {
					FD_SET(c->fd, &rfds);
				}

				if (fd > maxfd) {
					maxfd = c->fd;
				}
			}
		}

		/* TODO: also add exception set */
		status = select(maxfd+1, &rfds, &wfds, NULL, NULL);
		if (status < 0) {
			err(EXIT_FAILURE, "Socket select error");
		}

		if (FD_ISSET(server->fd, &rfds)) {
			fd = accept4(server->fd, server->addr->ai_addr, &(server->addr->ai_addrlen), SOCK_NONBLOCK);
			if (fd < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					continue;
				}
				err(EXIT_FAILURE, "Socket accept error");
			}

			added = 0;
			for (i = 0; i < MAX_CLIENTS; i++) {
				if (clients[i] == 0) {
					clients[i] = makeClient(fd);
					added = 1;
					if (fd > maxfd) {
						maxfd = fd;
					}
					break;
				}
			}

			if (!added) {
				fprintf(stderr, "Could not find room for client fd: %d\n", fd);
			}

			printf("Accepted connection! (fd: %d)\n", fd);
		}

		for (i = 0; i < MAX_CLIENTS; i++) {
			c = clients[i];

			if (c == 0) {
				continue;
			}

			if (FD_ISSET(c->fd, &wfds)) {
				respond(c);
			} else if (FD_ISSET(c->fd, &rfds) && c->cstate == CONNECTED) {
				readResponse(c);
			}
		}
	}
}


void setupAndListen(char *port)
{
	int status, fd;
	struct addrinfo hints;
	struct addrinfo *ai;

	server = (Server *)malloc(sizeof(Server));
	if (!server) {
		fprintf(stderr, "Couldn't allocate memory for starting the server\n");
		exit(EXIT_FAILURE);
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET; /* Only IPv4 for now */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; /* Listen on all network addresses */

	if ((status = getaddrinfo(NULL, port, &hints, &ai)) != 0) {
		fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
		exit(EXIT_FAILURE);
	}

	/*
	 * Normally only a single protocol exists to support a particular
	 * socket [type, protocol family] combination, so we can skip specifying it
	 */
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		err(EXIT_FAILURE, "Socket creation error");
	}

	status = bind(fd, ai->ai_addr, ai->ai_addrlen);
	if (status != 0) {
		err(EXIT_FAILURE, "Socket bind error");
	}

	status = listen(fd, RECV_BACKLOG);
	if (status != 0) {
		err(EXIT_FAILURE, "Socket listen error");
	}

	server->fd = fd;
	server->addr = ai;

	setupSighandlers();

	printf("Listening on 0.0.0.0:%s ...\n", port);
}


int on_message_complete_cb(http_parser *p)
{
	Client *c = p->data;

	c->pstate = SUCCESS;
	c->to_reply = 1;

	return 0;
}


void readResponse(Client *c)
{
	int nparsed;
	int status = recv(c->fd, c->buf, RECV_BUFFER, 0);

	if (status < 0 && errno != EAGAIN && errno != EWOULDBLOCK ) {
		// TODO: Leave this on until we work on the possible errors
		// from recv. In the future we should handle them.
		err(EXIT_FAILURE, "Message recv error (client: %d)\n", c->fd);
	} else {
		if (status == 0) {
			printf("Client %d closed the connection.\n", c->fd);
			c->cstate = DISCONNECTED;
			c->to_reply = 1;
		}

		nparsed = http_parser_execute(c->parser, c->parser_settings, c->buf, status);

		if (nparsed != status) {
			c->pstate = ERROR;
			c->to_reply = 1;
			printf("Parse error: %s\n", http_errno_description(HTTP_PARSER_ERRNO(c->parser)));
		}
	}
}


void respond(Client *c)
{
	char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nCool\r\n\r\n";
	char *resp400 = "HTTP/1.1 400 Bad Request\r\n\r\n";

	if (c->pstate == ERROR) {
		send(c->fd, resp400, strlen(resp400), 0);
		closeClient(c->fd);
	} else if (c->pstate == SUCCESS) {
		send(c->fd, resp, strlen(resp), 0);
		closeClient(c->fd);
	}
}


Client *makeClient(int fd)
{
	http_parser_settings *settings = malloc(sizeof(http_parser_settings));
	http_parser *parser = malloc(sizeof(http_parser));

	http_parser_settings_init(settings);
	http_parser_init(parser, HTTP_REQUEST);

	settings->on_message_complete = on_message_complete_cb;

	Client *c = malloc(sizeof(Client));
	if (!c) {
		fprintf(stderr, "Couldn't allocate memory for connection %d\n", fd);
		exit(EXIT_FAILURE);
	}

	c->fd = fd;
	c->cstate = CONNECTED;
	c->pstate = IN_PROGRESS;
	c->to_reply = 0;
	memset(c->buf, 0, RECV_BUFFER);
	c->parser_settings = settings;
	c->parser = parser;
	c->parser->data = c;

	return c;
}


void closeClient(int fd)
{
	int i;

	if (close(fd) < 0) {
		fprintf(stderr, "Client %d close error\n", fd);
		exit(EXIT_FAILURE);
	}

	FD_CLR(fd, &rfds);
	FD_CLR(fd, &wfds);

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i] && clients[i]->fd == fd) {
			free(clients[i]);
			clients[i] = 0;
			return;
		}
	}
}


void setupSighandlers(void)
{
	struct sigaction act;
	act.sa_handler = shutdownServer;

	int status = sigaction(SIGINT, &act, NULL);
	if (status != 0) {
		err(EXIT_FAILURE, "Error setting up signal handler\n");
	}
}


void shutdownServer(int sig)
{
	printf("\nShutting down...\n");

	int status = close(server->fd);
	if (status != 0) {
		err(EXIT_FAILURE, "Socket cleanup error");
	}

	freeaddrinfo(server->addr);
	printf("Goodbye!\n");
	exit(EXIT_SUCCESS);
}
