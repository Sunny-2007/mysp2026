#ifndef SERVER_H
#define SERVER_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define ERR_EXIT(msg)          \
    do {                       \
        perror(msg);           \
        exit(EXIT_FAILURE);    \
    } while (0)

#define ACCOUNT_NUM 20
#define ACCOUNT_ID_START 902001
#define ACCOUNT_ID_END 902020
#define MAX_BALANCE 1000000
#define MAX_MSG_LEN 512
#define LOCK_RETRY_MS 50
#define RECORD_PATH "./accountRecord"

typedef struct {
    int id;
    int balance;
} account_record;

enum client_state {
    READY,
    WAIT_UPDATE,
    WAIT_RECEIVE,
    WAIT_TRANSFER_OUT,
    WAIT_TRANSFER_IN,
    WAIT_TRANSFER_LOCK,
    WAIT_TRANSFER_PEER
};

enum input_status {
    INPUT_ERROR = -1,
    INPUT_EOF = 0,
    INPUT_DATA = 1,
    INPUT_RETRY = 2
};

typedef struct {
    char hostname[512];
    unsigned short port;
    int listen_fd;
} server;

typedef struct {
    char host[512];
    int conn_fd;

    /* Fragmented input belonging only to this client. */
    char buf[MAX_MSG_LEN];
    size_t buf_len;

    enum client_state state;
    int account_index;
    int current_balance;

    /* TODO 4 storage. Pending transfer values live on the sender request. */
    int peer_fd;              /* -1 when unpaired */
    int target_index;         /* source uses account_index */
    int target_balance;
    int amount;
    bool close_pending;      /* failed notification; handled by supplied sweep */
} request;

extern server svr;
extern request *requestP;
extern int maxfd;
extern int record_fd;
extern int record_owner[ACCOUNT_NUM];
extern int receiver_fd[ACCOUNT_NUM]; /* local one-shot receiver registration */

/* Complete input-buffer helpers supplied by the starter. */
int append_input(request *reqP);
int pop_command(request *reqP, char *command, size_t command_size);

#endif
