#define _DEFAULT_SOURCE

#include "server.h"

#include <limits.h>
#include <signal.h>
#include <stdint.h>

/*
 * Implement four TODOs in order:
 *   1. READY commands, complete response writes, update/add/close sessions
 *   2. Nonblocking fcntl record locks and local ownership
 *   3. select multiplexing, fragmented input and batched commands
 *   4. Confirmed two-client transfer and immediate lock failure
 * Socket setup, parsers, record I/O and input buffers are supplied.
 */

static const char *welcome_banner =
    "================================\n"
    " Welcome to CSIE Ledger System \n"
    "================================\n";
static const char *ready_prompt = "Please enter your command: ";
static const char *update_prompt =
    "Please enter add <delta> or close: ";

static const char *receive_prompt = "Waiting for transfer; enter cancel or exit: ";
static const char *transfer_prompt = "Waiting for receiver; enter cancel or exit: ";
static const char *accept_prompt = "Please enter accept or reject: ";
static const char *exit_prompt = ">>> Client exit.\n";

static void cleanup_transfer(request *req);

server svr;
request *requestP;
int maxfd;
int record_fd;
int record_owner[ACCOUNT_NUM];
int receiver_fd[ACCOUNT_NUM];

static int send_text(int fd, const char *text) {
    /*
     * TODO 1: Send the entire string to this blocking client socket using
     * write(). Handle short writes and EINTR. Return 0 on success or -1
     * on failure (including a write that makes no progress).
     *
     * This is also used for the welcome message, so complete it first.
     */

    (void)fd;
    (void)text;
    size_t cur = 0;
    size_t count = strlen(text);
    while(cur < count) {
        ssize_t n = write(fd, text+cur, count - cur);
        if (n < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        else if (n == 0) {
            break;
        }
        cur += n;
    }
    if(count == cur) return 0;
    else return -1;
}

static void init_request(request *req) {
    memset(req, 0, sizeof(*req));
    req->conn_fd = -1;
    req->peer_fd = -1;
    req->target_index = -1;
    req->state = READY;
    req->account_index = -1;
}

int append_input(request *req) { //讀資料
    if (req->buf_len >= sizeof(req->buf)) {
        return INPUT_ERROR;
    }
    ssize_t n = read(req->conn_fd, req->buf + req->buf_len,
                     sizeof(req->buf) - req->buf_len);
    if (n == 0) {
        return INPUT_EOF;
    }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return INPUT_RETRY;
        }
        return INPUT_ERROR;
    }
    req->buf_len += (size_t)n;
    return INPUT_DATA;
}

int pop_command(request *req, char *out, size_t out_size) { 
    char *newline = memchr(req->buf, '\n', req->buf_len);
    if (newline == NULL) {
        return 0;
    }
    size_t consumed = (size_t)(newline - req->buf) + 1;
    size_t len = consumed - 1;
    if (len > 0 && req->buf[len - 1] == '\r') {
        --len;
    }
    if (len + 1 > out_size) {
        return -1;
    }
    memcpy(out, req->buf, len);
    out[len] = '\0';
    memmove(req->buf, req->buf + consumed, req->buf_len - consumed);
    req->buf_len -= consumed;
    return 1;
}

static int set_record_lock(int index, short type) {
    /*
     * TODO 2: Apply type to exactly one record using fcntl(F_SETLK).
     * Return the syscall result and preserve errno on failure.
     * The success stub is only for developing Task 1.
     */
    (void)index;
    (void)type;
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = type;                                  // F_RDLCK / F_WRLCK / F_UNLCK
    fl.l_whence = SEEK_SET;                              // 從檔案開頭算
    fl.l_start  = (off_t)index * sizeof(account_record); // 第 index 筆的位置
    fl.l_len    = sizeof(account_record);                // 只鎖一筆（8 bytes）
    return fcntl(record_fd, F_SETLK, &fl);
}

static void release_update(request *req) {
    if (req->state != WAIT_UPDATE) return;
    /* TODO 2: Release this request's update lock and matching local owner. */
    if (record_owner[req->account_index] == req->conn_fd) {
        record_owner[req->account_index] = -1;
        set_record_lock(req->account_index, F_UNLCK);
    }
    /*
     * TODO 1: Reset the update-related fields to their READY values.
     * Preserve the connection and input buffer. Repeated cleanup must be safe.
     */

    req->state = READY;
    req->account_index = -1;
    req->current_balance = 0;
    (void)req;
    return;
}

static void close_client(int fd, fd_set *master) {
    cleanup_transfer(&requestP[fd]);
    release_update(&requestP[fd]);

    /*
     * TODO 3: Remove fd from the monitored set when master is non-NULL.
     * The sequential development loop passes NULL.
     */
 
    (void)master;       
    if(master != NULL) FD_CLR(fd, master);
    close(fd);
    init_request(&requestP[fd]);
}

/* Supplied infrastructure: close failed peer notifications outside handlers.
 * Call before select(), and skip close_pending clients in the readable batch.
 * This drains known cleanup work; it does not poll for network events.
 */
static void close_failed_clients(fd_set *master) {
    bool again;
    do {
        again = false;
        for (int fd = 0; fd < maxfd; ++fd) {
            if (requestP[fd].conn_fd >= 0 && requestP[fd].close_pending) {
                close_client(fd, master);
                again = true;
            }
        }
    } while (again);
}

static bool parse_int32(const char *text, int *value) {
    const char *digits = text;
    if (*digits == '+' || *digits == '-') ++digits;
    if (*digits == '\0') return false;
    for (const char *p = digits; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    if (*text == '\0' || *text == ' ' || *text == '\t') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno == ERANGE || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool parse_account_command(const char *line, const char *verb,
                                  int *index) {
    size_t verb_len = strlen(verb);
    if (strncmp(line, verb, verb_len) != 0 || line[verb_len] != ' ') {
        return false;
    }
    const char *arg = line + verb_len + 1;
    if (*arg == '\0' || strchr(arg, ' ') != NULL || strchr(arg, '\t') != NULL) {
        return false;
    }
    int id;
    if (!parse_int32(arg, &id) ||
        id < ACCOUNT_ID_START || id > ACCOUNT_ID_END) {
        return false;
    }
    *index = id - ACCOUNT_ID_START;
    return true;
}



static bool parse_delta_command(const char *line, int *delta) {
    const char prefix[] = "add ";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *arg = line + sizeof(prefix) - 1;
    if (*arg == '\0' || strchr(arg, ' ') != NULL || strchr(arg, '\t') != NULL) {
        return false;
    }
    return parse_int32(arg, delta);
}

static bool parse_transfer_command(const char *line, int *source,
                                   int *target, int *amount) {
    const char prefix[] = "transfer ";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;
    char args[MAX_MSG_LEN];
    if (strlen(line) >= sizeof(args)) return false;
    strcpy(args, line + sizeof(prefix) - 1);
    char *second = strchr(args, ' ');
    if (second == NULL) return false;
    *second++ = '\0';
    char *third = strchr(second, ' ');
    if (third == NULL) return false;
    *third++ = '\0';
    int src, dst;
    if (!parse_int32(args, &src) || !parse_int32(second, &dst) ||
        !parse_int32(third, amount) || *amount <= 0 || src == dst ||
        src < ACCOUNT_ID_START || src > ACCOUNT_ID_END ||
        dst < ACCOUNT_ID_START || dst > ACCOUNT_ID_END) return false;
    *source = src - ACCOUNT_ID_START;
    *target = dst - ACCOUNT_ID_START;
    return true;
}

static int read_record_at(int index, account_record *record) {
    off_t offset = (off_t)index * (off_t)sizeof(*record);
    return pread(record_fd, record, sizeof(*record), offset) ==
                   (ssize_t)sizeof(*record)
               ? 0
               : -1;
}

static int write_record_at(int index, const account_record *record) {
    off_t offset = (off_t)index * (off_t)sizeof(*record);
    return pwrite(record_fd, record, sizeof(*record), offset) ==
                   (ssize_t)sizeof(*record)
               ? 0
               : -1;
}

/* A handler returns true to keep the connection, false to close it. */
static bool handle_ready(request *req, const char *line) {
    /* TODO 1: read <id>, update <id>, exit. Invalid input closes the client.
     * TODO 2: read takes a short shared lock; update retains a write lock.
     * Check local ownership too. A lock conflict immediately replies Locked.
     * After acquiring an update lock, read the current balance and enter
     * WAIT_UPDATE. There is no blocking update mode or retry state.
     */
    (void)req; (void)line;
    (void)ready_prompt; (void)update_prompt;
    (void)parse_account_command; (void)read_record_at; (void)set_record_lock;
    int idx = 0;
    if (parse_account_command(line, "read", &idx)) {
        account_record rec;
        if (record_owner[idx] != -1) {
           send_text(req->conn_fd, ">>> Locked.\nPlease enter your command: ");
           return true;
        }

        if(set_record_lock(idx, F_RDLCK)< 0) { 
            if (errno == EAGAIN || errno == EACCES){
                send_text(req->conn_fd, ">>> Locked.\nPlease enter your command: ");
                return true;
            }
            return false;
        }

        if(read_record_at(idx, &rec)== -1) {
            set_record_lock(idx, F_UNLCK);
            return false;
        }
        set_record_lock(idx, F_UNLCK);
        char msg[MAX_MSG_LEN];
        snprintf(msg, sizeof(msg), ">>> Account %d balance: %d\nPlease enter your command: ", idx + ACCOUNT_ID_START, rec.balance);
        send_text(req->conn_fd, msg);
        return true;

    } else if (parse_account_command(line, "update", &idx)){
        account_record rec;
        if (record_owner[idx] != -1) {
           send_text(req->conn_fd, ">>> Locked.\nPlease enter your command: ");
           return true;
        }
        if(set_record_lock(idx, F_WRLCK)< 0) { 
            if (errno == EAGAIN || errno == EACCES){
                send_text(req->conn_fd, ">>> Locked.\nPlease enter your command: ");
                return true;
            }
            return false;
        }
        int ret = read_record_at(idx, &rec);
        if(ret == -1) {
            set_record_lock(idx, F_UNLCK);
            return false;
        } 
        req->current_balance = rec.balance;    
        record_owner[idx] = req->conn_fd;
        req->account_index = idx;
        char msg[MAX_MSG_LEN];
        snprintf(msg, sizeof(msg), ">>> Account %d balance: %d\n>>> Update lock acquired.\nPlease enter add <delta> or close: ", idx + ACCOUNT_ID_START, req->current_balance);
        send_text(req->conn_fd, msg);
        req->state = WAIT_UPDATE;
        return true;      
    } else if (strcmp(line, "exit") == 0) {
        send_text(req->conn_fd, exit_prompt);
        return false;
    }
    else {
        send_text(req->conn_fd, ">>> [Error] Invalid command.\n");
        return false;
    }
    return false;
}

static bool handle_wait_update(request *req, const char *line) {
    /* TODO 1: add <delta>, close, exit.
     * Each valid add writes immediately and updates current_balance; remain
     * in WAIT_UPDATE, retaining the lock. A range error also stays here.
     * close releases this session and returns READY without closing the socket.
     * exit/invalid input/EOF clean up and close the socket. Never roll back
     * previously successful adds. cancel is invalid in WAIT_UPDATE.
     * TODO 2: only close/connection cleanup releases the update lock/owner.
     */
    (void)req; (void)line;
    (void)parse_delta_command; (void)write_record_at;
    int idx = req->account_index;
 
    int delta;
    char msg[MAX_MSG_LEN];
    if(parse_delta_command(line, &delta)) {  //add <amount>
        long long nb = (long long)req->current_balance + delta;
        if(nb >= 0 && nb <= MAX_BALANCE) {   
            account_record newrec;
            newrec.balance = nb;
            newrec.id = idx + ACCOUNT_ID_START;
            if (write_record_at(idx, &newrec) < 0) return false;   // 先寫檔  
            req->current_balance = nb;                
            snprintf(msg, sizeof(msg), ">>> Update successful.\n>>> Account %d balance: %d\nPlease enter add <delta> or close: ", idx + ACCOUNT_ID_START, req->current_balance);    
            send_text(req->conn_fd, msg);     
            return true;   
        }
        else {
            send_text(req->conn_fd, ">>> [Error] Balance out of range.\nPlease enter add <delta> or close: "); 
            return true;
        }
    } 
    else if(strcmp(line, "close") == 0) { //close
        send_text(req->conn_fd, ">>> Update closed.\nPlease enter your command: ");     
        release_update(req);
        return true;
    }
    else if(strcmp(line, "exit") == 0) { //exit
        send_text(req->conn_fd, exit_prompt);
        return false;       
    }
    send_text(req->conn_fd, ">>> [Error] Invalid command.\n");
    return false;
}

/* TODO 4: Complete the two-client transfer lifecycle described in SPEC.md.
 * receiver_fd[] is registration only; it must not replace record_owner[].
 * Both record_owner entries of a pending offer belong to the sender fd.
 * Use the sender request to store the two indices, balances, amount and peer.
 * Set peer.close_pending if a peer notification fails; do not close a peer
 * inside a command handler. The supplied sweep handles those failures.
 */

// 解lock、清 owner、register 雙方都設回 READY。
static void finish_transfer(request *sender) {
    request *receiver = &requestP[sender->peer_fd];
    set_record_lock(sender->account_index, F_UNLCK);
    set_record_lock(receiver->target_index, F_UNLCK);
    receiver_fd[receiver->target_index] = -1;
    record_owner[sender->account_index] = -1;
    record_owner[sender->target_index] = -1;
    sender->state = READY;
    receiver->state = READY;
    sender->peer_fd = -1;
    sender->target_balance = 0;
    sender->target_index = -1;
    sender->account_index = -1;
    sender->amount = 0;
    sender->current_balance = 0;
    receiver->amount = 0;
    receiver->current_balance = 0;
    receiver->peer_fd = -1;
    receiver->target_index = -1;
    return;
}

static void cleanup_transfer(request *req) {
    /* TODO 4: Remove registration, release only this transfer's locks,
     * reset both participants and notify the surviving peer on disconnect.
     * No-op for READY/WAIT_UPDATE. Never commit during cleanup.
     */
    (void)req;
    if(req->state == READY || req->state == WAIT_UPDATE) return;
    if(req->state == WAIT_RECEIVE) {
        receiver_fd[req->target_index] = -1;
        req->target_index = -1;
        req->state = READY;
        return;
    }
    int survivor = req->peer_fd;
    if(req->state == WAIT_TRANSFER_IN) {
        request *snd = &requestP[req->peer_fd];
        finish_transfer(snd);       
    }
    else { //WAIT_TRANSFER_OUT
        finish_transfer(req);
    }   
    if(send_text(survivor, ">>> Transfer canceled: peer disconnected.\n")) {
        requestP[survivor].close_pending = true;
        return;
    }
    send_text(survivor, ready_prompt);
    return;
}

static bool handle_transfer_ready(request *req, const char *line) {
    /* TODO 4: receive <id>, transfer <source> <target> <amount>.
     * Match a local receiver, check local owners, and try both F_WRLCK locks.
     * Any conflict: release acquired locks, reply Locked to the sender,
     * leave it READY, and preserve the receiver's WAIT_RECEIVE registration.
     * Never queue or automatically retry a failed transfer.
     * Only after BOTH locks succeed, read/check balances and create an offer.
     * Balance failure also releases both locks and preserves registration.
     */
    (void)req; (void)line;
    (void)parse_transfer_command;
    (void)receive_prompt; (void)transfer_prompt; (void)accept_prompt;
    int source, target, amount;
    char msg[MAX_MSG_LEN];
    if(parse_transfer_command(line, &source, &target, &amount)) {
        int rfd = receiver_fd[target];
        if(rfd == -1 || requestP[rfd].state != WAIT_RECEIVE) {
            send_text(req->conn_fd, ">>> [Error] Receiver unavailable.\n");
            send_text(req->conn_fd, ready_prompt);
            return true;
        }
        if(record_owner[source] != -1 || record_owner[target] != -1) { //有其中一個fd locked by peers, return 
            send_text(req->conn_fd, ">>> Locked.\n");
            send_text(req->conn_fd, ready_prompt);   
            return true;
        }
        if(set_record_lock(source, F_WRLCK)< 0) { 
            if (errno == EAGAIN || errno == EACCES){
                send_text(req->conn_fd, ">>> Locked.\nPlease enter your command: ");
                return true;
            }
            return false;
        }    
        if(set_record_lock(target, F_WRLCK)< 0) {
            set_record_lock(source, F_UNLCK);
            if (errno == EAGAIN || errno == EACCES){
                send_text(req->conn_fd, ">>> Locked.\nPlease enter your command: ");
                return true;
            }
            return false;
        }

        account_record source_rec, target_rec;
        if(read_record_at(target, &target_rec) == -1){
            set_record_lock(target, F_UNLCK);
            set_record_lock(source, F_UNLCK);
            return false;
        }
        if(read_record_at(source, &source_rec) == -1){
            set_record_lock(target, F_UNLCK);
            set_record_lock(source, F_UNLCK);
            return false;
        }
        long long target_nb = amount + (long long) target_rec.balance;
        long long source_nb = (long long) source_rec.balance - (long long) amount;
        if(target_nb > MAX_BALANCE || source_nb < 0) {
            send_text(req->conn_fd, ">>> [Error] Balance out of range.\n");
            send_text(req->conn_fd, ready_prompt);
            set_record_lock(source, F_UNLCK);
            set_record_lock(target, F_UNLCK);

            return true;
        }
        //record_owner 都記在 sender 這邊
        record_owner[source] = req->conn_fd;
        record_owner[target] = req->conn_fd;
        //index, balance, amount 也是
        req->peer_fd = receiver_fd[target];
        req->account_index = source;
        req->target_index = target; 
        req->target_balance = target_rec.balance;
        req->current_balance = source_rec.balance;
        req->amount = amount;
        req->state = WAIT_TRANSFER_OUT;

        int target_fd = receiver_fd[target]; 
        request *rcv = &requestP[target_fd]; // rcv is target request

        rcv->peer_fd = req->conn_fd;
        req->peer_fd = rcv->conn_fd;
        rcv->state = WAIT_TRANSFER_IN;
        send_text(req->conn_fd, ">>> Transfer requested.\n");
        send_text(req->conn_fd, transfer_prompt);
        snprintf(msg, sizeof(msg), ">>> Transfer offer: %d -> %d, amount: %d\n", source + ACCOUNT_ID_START, target + ACCOUNT_ID_START, amount);
        if(send_text(req->peer_fd, msg) < 0) {
            requestP[req->peer_fd].close_pending = true; 
            return true;
        }
        send_text(req->peer_fd, accept_prompt);
        return true;

    }else if(parse_account_command(line, "receive", &target)) {
        if(receiver_fd[target] != -1) {
            send_text(req->conn_fd,">>> [Error] Receiver already registered.\n");
            send_text(req->conn_fd,ready_prompt);
            return true;
        }
        receiver_fd[target] = req->conn_fd;
        req->target_index = target;
        req->state = WAIT_RECEIVE;
        snprintf(msg, sizeof(msg), ">>> Ready to receive on account %d.\nWaiting for transfer; enter cancel or exit: ", target + ACCOUNT_ID_START);
        send_text(req->conn_fd, msg);
        return true;
    }
    else if(strcmp(line, "cancel") == 0) {
        send_text(req->conn_fd, ">>> Receive canceled.\nPlease enter your command: ");
        req->state = READY;
        return true;
    }
    else if(strcmp(line, "exit") == 0) { //exit
        send_text(req->conn_fd, ">>> Client exit.\n");
        return false;       
    }
    send_text(req->conn_fd, ">>> [Error] Invalid command.\n");
    return false; 

}

static bool handle_wait_transfer(request *req, const char *line) {
    /* TODO 4: Handle receive and transfer-confirmation states.
     * Only the matched receiver may accept/reject; only the sender cancels
     * a pending offer. Exit/invalid input use close_client() cleanup.
     * Complete/cancel/reject must release both locks and registration.
     */
    (void)line;
    char msg[MAX_MSG_LEN];
    if(req->state == WAIT_TRANSFER_IN) {
        if(strcmp(line, "accept") == 0) {
            account_record source_rec, target_rec;
            request *snd = &requestP[req->peer_fd];
            read_record_at(snd->target_index, &target_rec);
            read_record_at(snd->account_index, &source_rec);
            target_rec.balance += snd->amount;
            source_rec.balance -= snd->amount;
            write_record_at(snd->target_index, &target_rec);
            write_record_at(snd->account_index, &source_rec);
            snprintf(msg, sizeof(msg), ">>> Transfer completed.\n>>> Account %d balance: %d\n>>> Account %d balance: %d\n%s", snd->target_index + ACCOUNT_ID_START, target_rec.balance, snd->account_index + ACCOUNT_ID_START, source_rec.balance, ready_prompt);

            if(send_text(snd->conn_fd, msg) < 0) {
                snd->close_pending = true;
            }
            send_text(req->conn_fd, msg);
            snd->state = READY;
            req->state = READY;
            finish_transfer(snd);
            return true;
        }
        if(strcmp(line, "reject") == 0) {
            send_text(req->conn_fd, ">>> Transfer rejected.\n"); 
            send_text(req->peer_fd, ">>> Transfer rejected.\n");     
            send_text(req->conn_fd, ready_prompt);     
            send_text(req->peer_fd, ready_prompt);
            finish_transfer(&requestP[req->peer_fd]);
            return true;
        }
        if(strcmp(line, "exit") == 0) {
            send_text(req->conn_fd, exit_prompt);
            cleanup_transfer(req);
            return false;
        }
        send_text(req->conn_fd, ">>> [Error] Invalid command.\n");
        cleanup_transfer(req);
        return false;
    }
    if(req->state == WAIT_TRANSFER_OUT) {
        if(strcmp(line, "cancel") == 0) {
            send_text(req->conn_fd, ">>> Transfer canceled.\n");  
            send_text(req->peer_fd, ">>> Transfer canceled.\n");   
                   
            send_text(req->conn_fd, ready_prompt);
            send_text(req->peer_fd, ready_prompt);
            finish_transfer(req);
            return true;
        }
        if(strcmp(line, "exit") == 0) {
            send_text(req->conn_fd, ">>> Client exit.\n");
            cleanup_transfer(req);
            return false;
        }
        send_text(req->conn_fd, ">>> [Error] Invalid command.\n");
        cleanup_transfer(req);
        return false;
    }
    if(req->state == WAIT_RECEIVE) {
        if(strcmp(line, "cancel") == 0) {
            send_text(req->conn_fd, ">>> Receive canceled.\nPlease enter your command: ");
            req->state = READY;
            receiver_fd[req->target_index] = -1;
            req->target_index = -1;
            return true;
        }
        else if(strcmp(line, "exit") == 0) { //exit
            cleanup_transfer(req);
            send_text(req->conn_fd, ">>> Client exit.\n");
            return false;       
        }
        cleanup_transfer(req);
        send_text(req->conn_fd, ">>> [Error] Invalid command.\n");
        return false;
    }
    return false;
}

/* Supplied per-client state dispatcher. */
static bool handle_command(request *req, const char *line) {
    if (req->state == READY) {
        if (strncmp(line, "receive ", 8) == 0 ||
            strncmp(line, "transfer ", 9) == 0) {
            return handle_transfer_ready(req, line);
        }
        return handle_ready(req, line);
    }
    if (req->state == WAIT_UPDATE)
        return handle_wait_update(req, line);
    return handle_wait_transfer(req, line);
}





static bool drain_commands(request *req) {
    /* TODO 3: dispatch ALL complete lines in order and preserve a partial tail.
     * Stop and discard remaining input when a handler requests disconnection.
     * add remains WAIT_UPDATE; close returns READY, so later lines in the
     * same batch must be interpreted using the resulting state.
     */
    (void)req; (void)handle_command;
    int keep = 1;
    while (keep) {
        char line[MAX_MSG_LEN];
        int popped = pop_command(req, line, sizeof(line));
        if (popped == 0) {
            return true;
        }
        if (popped < 0) {
            (void)send_text(req->conn_fd, ">>> [Error] Invalid command.\n");
            return false;
        }
        keep = handle_command(req, line);
    }
    return false;
}

static void init_server(unsigned short port) {
    struct sockaddr_in addr;
    int reuse = 1;
    gethostname(svr.hostname, sizeof(svr.hostname));
    svr.port = port;
    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0) {
        ERR_EXIT("socket");
    }
    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        ERR_EXIT("setsockopt");
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(svr.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, 1024) < 0) {
        ERR_EXIT("listen");
    }
    maxfd = FD_SETSIZE;
    requestP = calloc((size_t)maxfd, sizeof(*requestP));
    if (requestP == NULL) {
        ERR_EXIT("calloc");
    }
    for (int i = 0; i < maxfd; ++i) {
        init_request(&requestP[i]);
    }
    for (int i = 0; i < ACCOUNT_NUM; ++i) {
        record_owner[i] = -1;
        receiver_fd[i] = -1;
    }
}


static void serve_clients(void) {
    (void)drain_commands; /* Used by the TODO 3 event loop. */
    /*
     * TODO 3: Replace this one-client-at-a-time loop with select().

     * Keep independent client states and partial input, and handle cleanup.

     * No lock-retry timer is needed; select may use a NULL timeout.
     * Call close_failed_clients(&master) before select(), skip close_pending
     * clients in a readable batch, and use drain_commands() after input.
     * The supplied sequential loop supports Tasks 1 and 2 development.
     * Implement this loop for Tasks 3 and 4. Transfer lock conflicts return
     * Locked immediately; no waiting states or automatic retries exist.
     */
    fd_set master;
    FD_ZERO(&master);
    FD_SET(svr.listen_fd, &master);
    while (1) {
        close_failed_clients(&master);
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        fd_set read_fds = master;
        if(select(maxfd, &read_fds, NULL, NULL, NULL) < 0)  {
            if (errno == EINTR) continue;
            ERR_EXIT("select");
        }
        for(int fd = 0; fd < maxfd; fd++) {
            if(!FD_ISSET(fd, &read_fds)) { //一定要已經讀到資料（在fdset裡面才能繼續做事)
                continue;
            }
            if(fd == svr.listen_fd) {
                int cfd = accept(svr.listen_fd, (struct sockaddr *)&peer, &peer_len); //新listen 到的client_fd
                if (cfd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    ERR_EXIT("accept");
                }
                if (cfd >= maxfd) {
                    close(cfd);
                    continue;
                }
                
                init_request(&requestP[cfd]);
                requestP[cfd].conn_fd = cfd;
                if (send_text(cfd, welcome_banner) < 0 ||
                    send_text(cfd, ready_prompt) < 0) {
                    close_client(cfd, NULL);
                    continue;
                }
                FD_SET(cfd, &master);
                continue;
            }
        
            //client fd
            if (requestP[fd].close_pending) continue;
            int status = append_input(&requestP[fd]);
            if (status == INPUT_EOF || status == INPUT_ERROR) {
                close_client(fd, &master);
                continue;
            }
            if (status == INPUT_RETRY) {
                continue;
            }
            if(!drain_commands(&requestP[fd])) {
                close_client(fd, &master);
            } 
        }   
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    record_fd = open(RECORD_PATH, O_RDWR);
    if (record_fd < 0) {
        ERR_EXIT("open accountRecord");
    }
    init_server((unsigned short)atoi(argv[1]));
    serve_clients();
    return EXIT_SUCCESS;
}