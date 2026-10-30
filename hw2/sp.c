#include "sp.h"

#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <fcntl.h>
#define CH_MAX 35
Conn conns[MAX_FDS];   // 跟 fds[] 用同一個 index
struct pollfd fds[MAX_FDS];
Child child_table[CH_MAX]; //存 child 的資訊, index 會重複使用

int nfds;
int cmd_fifo; //judge -> node
int reply_fifo; //node -> judge
int read_parent = -1; //parent -> child (downpipe)
int write_parent = -1; //child -> parent (uppipe)
int is_main;
short event = POLLIN | POLLHUP;

Entry* head; //local table 存key:value
Pending* phead;

char* pro;

char* consult_path;

int main(int argc, char *argv[]) {
    if (argc > 1) {
      fprintf(stderr, "Usage: ./sp\n");
      return 1;
    }
  
    // TODO: Determine whether the current process is SP main.
    //       After that, you may want to make the following code the same for both
    //       SP main and SP child processes. Therefore, you will set some
    //       variables here.
    pro = argv[0];
    is_main = (getenv("SP_CHILD") == NULL);
    // end TODO
    // TODO: poll the STDIO/FIFO for commands.
    if(is_main) {
        add_fd(STDIN_FILENO, event, COMMAND);
        reply_fifo = STDOUT_FILENO;
    }else {
        char* fifo_read = getenv("CMD_FIFO");
        char* fifo_write = getenv("REPLY_FIFO");
        if(mkfifo(fifo_read, 0666) == -1) {
            perror("mkfifo");
            exit(1);
        } 
        if(mkfifo(fifo_write, 0666) == -1) {
            perror("mkfifo");
            exit(1);
        }
        cmd_fifo = open(fifo_read, O_RDONLY | O_CLOEXEC);
        if(cmd_fifo == -1) {
            perror("open");
            exit(1);
        }
        reply_fifo = open(fifo_write, O_WRONLY | O_CLOEXEC);
        if(reply_fifo == -1) {
            perror("open");
            exit(1);
        }
        unlink(fifo_read);
        unlink(fifo_write);
        add_fd(cmd_fifo, event, COMMAND);
        read_parent = 0;
        write_parent = 1;
        add_fd(read_parent, event, PARENT_PIPE);

    }
    while (1) {
        int ret = poll(fds, nfds, -1);
        if(ret == -1) {
            if(errno == EINTR) continue;
            else {
                perror("poll");
                break;
            }
        } 
        // end TODO
        bool break_while = 0;
        for(int i = nfds - 1; i >= 0; --i) {
            if(fds[i].revents & POLLIN || fds[i].revents & POLLHUP ) {
                int status = conn_read(&conns[i]);
                if(status == INPUT_DATA) {
                    if(conns[i].role == CONSULT) {
                        drain_consultant(&conns[i]);
                    }
                    else if(drain_lines(&conns[i])){
                        break_while = true;
                        break;
                    }
                }
                else if(status == INPUT_EOF) {
                    if(conns[i].role == CONSULT) {
                        Consultant* tmp_c = conns[i].consult;
                        waitpid(tmp_c->pid, NULL, 0);
                        remove_fd(i);
                        if(tmp_c->done == 1) { //done
                            free(tmp_c->hash);
                            free(tmp_c->key);
                            free(tmp_c);
                        }
                        else { //fail
                            start_consultant(tmp_c);
                        }
                    }
                    else if(conns[i].role == PARENT_PIPE || conns[i].role == COMMAND){
                        break_while = true;
                        break;
                    }
                    else if (conns[i].role == CHILD_PIPE){

                        waitpid(child_table[conns[i].child_slot].pid, NULL, 0);
                        close(child_table[conns[i].child_slot].write_fd);                        child_table[conns[i].child_slot].in_use = 0;                        
                        remove_fd(i);
                    }
                    break;            
                }
                else if(status == INPUT_RETRY){
                    continue;
                }
                else if(status == INPUT_ERROR) {
                    break_while = true;
                    break; 
                }
            }
            else if(fds[i].revents & POLLERR|| fds[i].revents & POLLNVAL) {
                break_while = true;
                break; 
            }
        }
        if (break_while) {
            break;
        }
        // TODO: poll parent, and child processes for custom commands.
        
        // end TODO

        // TODO: classify the command and execute it.
    
        // end TODO

        // TODO: handle custom commands.

        // end TODO

    }
    shutdown_node();
    return 0;
}

int command_handler(Command *cmd) {
    switch (cmd->type) {
    case CREATE:
        create_handler(cmd);
        break;
    case STORE:
        store_handler(cmd);
        break;
    case DELETE:
        delete_handler(cmd);
        break;
    case FETCH:
        fetch_handler(cmd);
        break;
    case KILL:
        return KILL;
        break;
    case UPDATE:
        update_handler(cmd);
        break;
    case QUERY:
        query_handler(cmd);
        break;
    case ANSWER:
        answer_handler(cmd);
        break;
    default:
        fprintf(stderr, "Unknown command type\n");
        return -1;
    }
    return 0;
}


int create_handler(Command *cmd) {
    // TODO: Implement the CREATE command handler.
    cmd->arg1[cmd->arg1_len] = '\0';
    char* cmdfifo= cmd->arg1;
    char* repfifo = cmd->arg2;
    //pipe p[0]: read, p[1]: write
    int uppipe[2];
    int downpipe[2];
    //create pipe
    if(pipe(uppipe) == -1) {
        perror("pipe");
        return -1;
    }
    if(pipe(downpipe) == -1) {
        close(uppipe[0]);
        close(uppipe[1]);
        perror("pipe");
        return -1;
    }

    set_cloexec(uppipe[0]);
    set_cloexec(uppipe[1]);
    set_cloexec(downpipe[0]);
    set_cloexec(downpipe[1]);

    // fork: for parent return child's pid, for child return 0
    pid_t pid = fork();
    if(pid == -1) {
        perror("fork");
        close(uppipe[0]);
        close(uppipe[1]);
        close(downpipe[0]);
        close(downpipe[1]);
        return -1;
    }
    if(pid == 0) { //child //uppipe p[1]; downpipe p[0]
        setenv("SP_CHILD", "1", 1);
        setenv("CMD_FIFO", cmdfifo, 1);
        setenv("REPLY_FIFO", repfifo, 1);
        dup2(downpipe[0], 0);
        dup2(uppipe[1], 1);
        char* env[] = {pro, NULL}; //把參數做成陣列傳進去
        int er = execv(pro, env);
        if(er == -1) {
            perror("execv");
            _exit(1);
        }
    }else { //parent //uppipe[0]; downpipef[1]
        close(uppipe[1]);
        close(downpipe[0]);
        int slot = -1;
        for(int i = 0; i < CH_MAX; ++i) {
            if(child_table[i].in_use == 1) continue;
            child_table[i].in_use = 1;
            child_table[i].pid = pid;
            child_table[i].write_fd = downpipe[1];
            child_table[i].read_fd = uppipe[0];
            slot = i;
            break;
        }
        int idx = add_fd(uppipe[0], event, CHILD_PIPE);
        conns[idx].role = CHILD_PIPE;
        conns[idx].child_slot = slot;
    }
    return 0;
}

int store_handler(Command *cmd) {
    // TODO: Implement the STORE command handler.
    //arg1 = k;
    //arg2 = v;
    size_t key_len = cmd->arg1_len;
    char* key = cmd->arg1;
    Entry* cur = head;    
    while(cur != NULL) {
        if(cur->key_len == key_len && memcmp(cur->key, key, key_len) == 0) { //key exists
            free(cur->value); //原本value的空間要 free
            char* value = malloc(cmd->arg2_len);
            if(value == NULL) {
                perror("store");
                return -1;
            }
            memcpy(value, cmd->arg2, cmd->arg2_len);
            cur->value = value;
            cur->value_len = cmd->arg2_len;
            return 0;
        }
        cur = cur->next;
    }
    Entry* ent = new_entry(cmd->arg1, cmd->arg1_len, cmd->arg2, cmd->arg2_len);
    ent->next = head;
    head = ent;
    return 0;
}

int delete_handler(Command *cmd) {
    // TODO: Implement the DELETE command handler.
    char* key = cmd->arg1;
    size_t key_len = cmd->arg1_len;
    Entry* cur = head;
    Entry* prev = NULL;
    while(cur != NULL) {
        if(cur->key_len == key_len && memcmp(cur->key, key, key_len) == 0) { //key exists
            if(cur == head) {
                Entry* new_head = head->next;
                free_entry(head);
                head = new_head;
            }
            else {
                prev->next = cur->next;
                free_entry(cur);
            }
            return 0;
        }
        else {
            prev = cur;
            cur = cur->next;
        }
    }
    return 0;
}

int fetch_handler(Command *cmd) { // fetch <hash> <key> 
    // TODO: Implement the FETCH command handler.
    Origin ori;
    ori.slot = -1;
    ori.type = FROM_JUDGE;
    lookup(cmd, &ori);
    return 0;
}


int update_handler(Command *cmd) {
    // TODO: Implement the UPDATE command handler.
        struct stat st;
        char* path = cmd->arg1;
        cmd->arg1[cmd->arg1_len] = '\0';
        if (stat(path, &st) == -1) {
            // 不存在或沒有權限查看 
            return -1;
        }
        if (!S_ISREG(st.st_mode)) {
            // 存在但不是 regular file
            return -1;
        }
        if(!(access(path, X_OK) == 0)) {
            //不能access
            return -1;           
        }
        setenv("JUDGE_CONSULTANT", path, 1);   
    return 0;
}

int query_handler(Command *cmd) { // Q <hash> <key>
    Origin ori;
    ori.slot = cmd->src_slot;
    ori.type = FROM_CHILD;
    lookup(cmd, &ori);
    return 0;
}

int answer_handler(Command *cmd) { // A <hash> <value> 
    Origin out;
    if(pending_take(cmd->arg1, cmd->arg1_len, &out) == -1) {
        fprintf(stderr, "answer: unknown hash %.*s\n", (int)cmd->arg1_len, cmd->arg1);
        return -1;
    }
    deliver(&out, cmd->arg1, cmd->arg1_len, cmd->arg2, cmd->arg2_len);
    return 0;
}

int lookup(Command* cmd, Origin* origin) { //
    char* key = cmd->arg2;
    size_t key_len = cmd->arg2_len;
    Entry* cur = head;
    while(cur != NULL) {
        if(cur->key_len == key_len && memcmp(cur->key, key, key_len) == 0) { //key exists
            deliver(origin, cmd->arg1, cmd->arg1_len, cur->value, cur->value_len);
            return 0;
        }
        cur = cur->next;
    }
    //找不到 
    
    if(is_main){ //如果是 sp main:問 consultant
        Consultant* consult = malloc(sizeof(Consultant));
        consult->hash = malloc(cmd->arg1_len);
        memcpy(consult->hash, cmd->arg1, cmd->arg1_len);
        consult->hash_len = cmd->arg1_len;   
        consult->key = malloc(cmd->arg2_len);
        memcpy(consult->key, cmd->arg2, cmd->arg2_len);
        consult->key_len = cmd->arg2_len;
        consult->pid = 0;
        consult->done = 0;
        consult->origin = *origin;
        start_consultant(consult);
        return 0;
    } else {//如果不是 sp main:問 parent
        pending_add(cmd->arg1, cmd->arg1_len, origin);
        write_all(write_parent, "Q ", 2);
        write_all(write_parent, cmd->arg1, cmd->arg1_len);
        write_all(write_parent, " ", 1);
        write_all(write_parent, cmd->arg2, cmd->arg2_len);
        write_all(write_parent, "\n", 1);
    }
    return 0;
}

int deliver(Origin* origin, char* hash, size_t hash_len, char* value, size_t value_len){
    if(origin->type == FROM_JUDGE) {
        write_all(reply_fifo, hash, hash_len);
        write_all(reply_fifo, " ", 1);
        write_all(reply_fifo, value, value_len);
        write_all(reply_fifo, "\n", 1);
    }
    else {
        write_all(child_table[origin->slot].write_fd, "A", 1);
        write_all(child_table[origin->slot].write_fd, " ", 1);
        write_all(child_table[origin->slot].write_fd, hash,hash_len);
        write_all(child_table[origin->slot].write_fd, " ", 1);
        write_all(child_table[origin->slot].write_fd, value, value_len);
        write_all(child_table[origin->slot].write_fd, "\n", 1);     
    }
    return 0;
}

int pending_add(const char *hash, size_t hash_len, Origin *ori) {
    Pending* p = malloc(sizeof(Pending));
    if (p == NULL) { perror("pending_add"); return -1; }
    p->hash = malloc(hash_len);
    if (p ->hash == NULL) { perror("pending_add"); free(p); return -1; }
    memcpy(p->hash, hash, hash_len);
    p->hash_len = hash_len;
    p->origin = *ori;
    p->next = phead;
    // p->key = NULL;
    phead = p;
    return 0;
}
int pending_take(const char *hash, size_t hash_len, Origin *out) {
    Pending* cur = phead;
    Pending* prev = NULL;
    while(cur != NULL) {
        if(cur->hash_len == hash_len && memcmp(cur->hash, hash, hash_len) == 0) { //找到了
            *out = cur->origin;
            if (prev == NULL) phead = cur->next; //cur == phead;
            else   prev->next = cur->next; //cur != phead;
            free(cur->hash);
            free(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    return -1;
}


int write_all(int fd, const char* ptr, size_t len) {
    size_t done = 0;
    while(done  < len) {
        ssize_t n = write(fd, ptr+done, len - done);
        if(n == -1) {
            if(errno == EINTR) {
                continue;
            }
            else {
                perror("write_all");
                return -1;
            }
        }
        done += n;
    }
    return 0;
}


void conn_init(Conn* c, int fd) {
    c->fd = fd;
    c->buf = NULL;
    c->cap = 0;
    c->len = 0;
    c->scanned = 0;
    c->start = 0; 
    c->consult = NULL;
    return;
}

void conn_free(Conn* c) {
    free(c->buf);
    c->fd = -1;
    c->buf = NULL;
    c->cap = 0;
    c->len = 0;
    c->scanned = 0;
    c->start = 0; 
    return;   
}

bool conn_reserve(Conn* c, size_t need) {
    if(c->cap - c->len >= need) return true;
    size_t now_c = c->cap? c->cap : BUF_SIZE;
    while(now_c - c->len < need) {
        now_c *= 2;
    }
    void* tmp = realloc(c->buf, now_c);
    if(tmp == NULL) {
        return false;
    }
    c->buf = tmp;
    c->cap = now_c;

    return true;
}

int conn_read(Conn* c) {
    if(!conn_reserve(c, BUF_SIZE)) return INPUT_ERROR;
    ssize_t ret = read(c->fd, c->buf + c->len, c->cap - c->len);
    if(ret > 0) {
        c->len += ret;
        return INPUT_DATA;
    }
    if(ret == 0) {
        return INPUT_EOF;
    }
    if(ret == -1 && errno == EINTR) {
        return INPUT_RETRY;
    }
    else {
        perror("read");
        return INPUT_ERROR;
    }
}

void conn_compact(Conn *c) {
    if(c->start == 0) return;
    memmove(c->buf, c->buf + c->start, c->len - c->start);
    c->len -= c->start;
    c->scanned -= c->start;
    c->start = 0;
    if (c->cap > (1 << 20) && c->cap / 4 > c->len)  {
        void* tmp = realloc(c->buf, c->cap / 4);
        if(tmp != NULL) {
            c->buf = tmp;
            c->cap /= 4;
        }
    }
    return;
}


int conn_next_line(Conn* c, char** line, size_t *line_len) {
    void* pt = memchr(c->buf + c->scanned, '\n', c->len - c->scanned);
    if(pt == NULL) {
        c->scanned = c->len;
        return 0;
    }

    int id = (char*)pt - c->buf;
    size_t len = (size_t)id - c->start;
    c->buf[id] = '\0';
    *line = c->buf + c->start;
    *line_len = len;
    c->start = id + 1;
    c->scanned = id + 1;
    return 1;
}

int drain_lines(Conn* c){
    char* line;
    size_t line_len;
    while (conn_next_line(c, &line, &line_len)) {
        Command cmd;
        cmd.src_slot = -1;
        if(c->role == COMMAND) {
            parse_command(line, line_len, &cmd);
        }
        else if (c->role == PARENT_PIPE || c->role == CHILD_PIPE) {
            parse_protocal(line, line_len, &cmd);
            if(c->role == CHILD_PIPE) cmd.src_slot = c->child_slot;
        }
        int status = command_handler(&cmd);
        if(status == KILL) {conn_compact(c); return 1;}
    }
    conn_compact(c);
    return 0;
}

int drain_consultant(Conn* c) {
    char* line;
    size_t line_len;
    if(conn_next_line(c, &line, &line_len)) {
        char* sp1 = memchr(line, ' ', line_len);
        if(sp1 == NULL) {
            return -1;
        }
        int n = (char*)sp1 - line;
        if(n == 3 && strncmp(line, "Ok:", 3) == 0) {
            char* start = sp1+1;
            char* end = line + line_len;
            deliver(&c->consult->origin, c->consult->hash, c->consult->hash_len, start, end-start);
            c->consult->done = 1;
            return 0;
        }
        return 0;
    } 
    else {
        return -1;
    }  
}

int start_consultant(Consultant* consult) {
    // fork: for parent return child's pid, for child return 0
    int to_c[2];
    int from_c[2];
    //create pipe
    if(pipe(to_c) == -1) {
        perror("pipe");
        return -1;
    }
    if(pipe(from_c) == -1) {
        close(to_c[0]);
        close(to_c[1]);
        perror("pipe");
        return -1;
    }
    set_cloexec(to_c[0]);
    set_cloexec(to_c[1]);
    set_cloexec(from_c[0]);
    set_cloexec(from_c[1]);

    pid_t pid = fork();
    if(pid == -1) {
        perror("fork");
        close(to_c[0]);
        close(to_c[1]);
        close(from_c[0]);
        close(from_c[1]);
        return -1;
    }
    if(pid == 0) { //child //uppipe p[1]; downpipe p[0]
        dup2(to_c[0], 0);
        dup2(from_c[1], 1);
        char* path = getenv("JUDGE_CONSULTANT");
        int er = execl(path, path, (char *)NULL);
        if(er == -1) {
            perror("exec");
            _exit(1);
        }     
    }
    else {
        close(to_c[0]);
        close(from_c[1]);
        write_all(to_c[1], consult->key, consult->key_len);
        write_all(to_c[1], "\n", 1); 
        close(to_c[1]);
        int idx = add_fd(from_c[0], event, CONSULT);
        conns[idx].consult = consult;
        consult->pid = pid;
        consult->done = 0;
    }
    return 0;
}

int parse_command(char *line, size_t len, Command *cmd) {
    char* sp1 = memchr(line, ' ', len);
    if(sp1 == NULL) {
        if(len == 4 && strncmp(line, "kill", 4) == 0) {
            cmd->type = KILL;
            return 0;
        }
        cmd->type = UNKNOWN;
        return -1;
    }
    int n = (char*)sp1 - line;
    if(n == 6 && strncmp(line, "create", 6) == 0) {
        cmd->type = CREATE;
        cmd->arg1 = sp1 + 1;
        char* end = line + len;
        char* sp2 = memchr(cmd->arg1, ' ', len-(n+1));
        if (sp2 == NULL) { cmd->type = UNKNOWN; return -1; }
        cmd->arg1_len = sp2 - cmd->arg1;
        cmd->arg2 = sp2 + 1;
        cmd->arg2_len = end - cmd->arg2;
        return 0;
    }
    else if(n == 5 && strncmp(line, "store", 5) == 0) {
        cmd->type = STORE;
        cmd->arg1 = sp1 + 1;
        char* end = line + len;
        char* sp2 = memchr(cmd->arg1, ' ', len-(n+1));
        if (sp2 == NULL) { cmd->type = UNKNOWN; return -1; }
        cmd->arg1_len = sp2 - cmd->arg1;
        cmd->arg2 = sp2 + 1;
        cmd->arg2_len = end - cmd->arg2;
        return 0;    
    }
    else if(n == 6 && strncmp(line, "delete", 6) == 0 ) {
        cmd->type = DELETE;
        cmd->arg1 = sp1 + 1;
        char* end = line + len;
        cmd->arg1_len = end - cmd->arg1;
        return 0;      
    }
    else if(n == 5 && strncmp(line, "fetch", 5) == 0 ) {
        cmd->type = FETCH;
        cmd->arg1 = sp1 + 1;
        char* end = line + len;
        char* sp2 = memchr(cmd->arg1, ' ', len-(n+1));
        if (sp2 == NULL) { cmd->type = UNKNOWN; return -1; }
        cmd->arg1_len = sp2 - cmd->arg1;
        cmd->arg2 = sp2 + 1;
        cmd->arg2_len = end - cmd->arg2;  
        return 0;
    }
    else if(n == 6 && strncmp(line, "update", 6) == 0) {
        cmd->type = UPDATE;
        cmd->arg1 = sp1 + 1;
        char* end = line + len;
        cmd->arg1_len = end - cmd->arg1;     
        return 0;
    }
    else {
        cmd->type = UNKNOWN;
        return -1;
    }
}

int parse_protocal(char *line, size_t len, Command *cmd) {
    char* sp1 = memchr(line, ' ', len);
    if(sp1 == NULL) {
        cmd->type = UNKNOWN;
        return -1;
    }
    int n = (char*)sp1 - line;
    if(n == 1 && line[0] == 'Q' ) { //Query
        cmd->type = QUERY;
        cmd->arg1 = sp1 + 1;
        char* end = line + len;
        char* sp2 = memchr(cmd->arg1, ' ', len-(n+1));
        if (sp2 == NULL) { cmd->type = UNKNOWN; return -1; }
        cmd->arg1_len = sp2 - cmd->arg1;
        cmd->arg2 = sp2 + 1;
        cmd->arg2_len = end - cmd->arg2;  
        return 0;
    }
    else if(n == 1 && line[0] == 'A') { //Answer
        cmd->type = ANSWER;
        cmd->arg1 = sp1 + 1;
        char* end = line + len;
        char* sp2 = memchr(cmd->arg1, ' ', len-(n+1));
        if (sp2 == NULL) { cmd->type = UNKNOWN; return -1; }
        cmd->arg1_len = sp2 - cmd->arg1;
        cmd->arg2 = sp2 + 1;
        cmd->arg2_len = end - cmd->arg2;    
        return 0;
    }
    else {
        cmd->type = UNKNOWN;
        return -1;
    }
}
int add_fd(int fd, short event, int role) {  
    if(nfds >= MAX_FDS) return -1;  
    fds[nfds].fd = fd;
    fds[nfds].events = event;
    fds[nfds].revents = 0;
    conn_init(&conns[nfds], fd);
    conns[nfds].role = role;
    nfds++;
    return nfds - 1;
}

int remove_fd(int i) {
    close(fds[i].fd);
    conn_free(&conns[i]);
    fds[i] = fds[nfds - 1];
    conns[i] = conns[nfds - 1];
    conns[nfds - 1].buf = NULL;
    nfds--;
    return 0;
}

void shutdown_node(void) {
    for(int i = 0; i < CH_MAX; ++i) {
        if(!child_table[i].in_use) continue;
        close(child_table[i].write_fd);
    }
    for(int i = 0; i < CH_MAX; ++i) {
        if(!child_table[i].in_use) continue;
        waitpid(child_table[i].pid, NULL, 0);
    }
    if(is_main) {
        for(int i = 0; i < nfds; ++i) {
            if(conns[i].role == CONSULT) {
                waitpid(conns[i].consult->pid, NULL, 0);
            }
        }
    }
    close(reply_fifo);
    close(cmd_fifo);
    close(write_parent);
    return;
}

void free_entry(Entry* ent) {
    if(ent == NULL) return;
    free(ent->key);
    free(ent->value);
    free(ent);
    return;
}

Entry* new_entry(char* key, size_t key_len, char* value, size_t value_len) {
    Entry* ent = malloc(sizeof(Entry));
    ent->key = malloc(key_len);
    memcpy(ent->key, key, key_len);
    ent->key_len = key_len;
    ent->value = malloc(value_len);
    memcpy(ent->value, value, value_len);
    ent->value_len = value_len;
    ent->next = NULL;
    return ent;
}

int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD); // 先讀出現有的 fd flags
    if (flags == -1) { perror("fcntl F_GETFD"); return -1; }

    flags |= FD_CLOEXEC;             // 加上 close-on-exec
    if (fcntl(fd, F_SETFD, flags) == -1) { perror("fcntl F_SETFD"); return -1; } 
    return 0;
}