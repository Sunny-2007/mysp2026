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
Child child_table[CH_MAX];

int nfds = 0;
int cmd_fifo = 0;
int reply_fifo = 0;
int read_parent;
int write_partent;
short event = POLLIN | POLLHUP;

Entry* head; //local table 存key:value
Pending* phead;

char* pro;
void conn_init(Conn* c, int fd) {
    c->fd = fd;
    c->buf = NULL;
    c->cap = 0;
    c->len = 0;
    c->pid = 0;
    c->scanned = 0;
    c->start = 0; 
    return;
}

void conn_free(Conn* c) {
    free(c->buf);
    c->fd = -1;
    c->buf = NULL;
    c->cap = 0;
    c->len = 0;
    c->pid = 0;
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
        parse_command(line, line_len, &cmd);
        int status = command_handler(&cmd);
        if(status == KILL) {conn_compact(c); return 1;}
    }
    conn_compact(c);
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

int add_fd(int fd, short event) {  
    if(nfds >= MAX_FDS) return -1;  
    fds[nfds].fd = fd;
    fds[nfds].events = event;
    fds[nfds].revents = 0;
    conn_init(&conns[nfds], fd);
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
};

int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD); // 先讀出現有的 fd flags
    if (flags == -1) { perror("fcntl F_GETFD"); return -1; }

    flags |= FD_CLOEXEC;             // 加上 close-on-exec
    if (fcntl(fd, F_SETFD, flags) == -1) { perror("fcntl F_SETFD"); return -1; } 

    return 0;
}

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
    int is_main = (getenv("SP_CHILD") == NULL);
    // end TODO
    // TODO: poll the STDIO/FIFO for commands.
    if(is_main) {
        add_fd(STDIN_FILENO, event);
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
        cmd_fifo = open(fifo_read, O_RDONLY);
        if(cmd_fifo == -1) {
            perror("open");
            exit(1);
        }
        reply_fifo = open(fifo_write, O_WRONLY);
        if(reply_fifo == -1) {
            perror("open");
            exit(1);
        }
        add_fd(cmd_fifo, event);
        add_fd(reply_fifo, event);
    }
    while (1) {
        Command command;
        char *command_buffer = NULL;

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
                    if(drain_lines(&conns[i])){
                        break_while = true;
                        break;
                    }
                }
                else if(status == INPUT_EOF) {
                    break_while = true;
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
        kill_handler(cmd);
        return KILL;
        break;
    case UPDATE:
        update_handler(cmd);
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
        exit(1);
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
        int slot = 0;
        for(int i = 0; i < CH_MAX; ++i) {
            if(child_table[i].in_use == 1) continue;
            child_table[i].in_use = 1;
            child_table[i].pid = pid;
            child_table[i].write_fd = downpipe[1];
            child_table[i].read_fd = uppipe[0];
            slot = i;
            break;
        }
        int idx = add_fd(uppipe[0], event);
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

int fetch_handler(Command *cmd) {
    // TODO: Implement the FETCH command handler.
    char* key = cmd->arg2;
    size_t key_len = cmd->arg2_len;
    Entry* cur = head;
    while(cur != NULL) {
        if(cur->key_len == key_len && memcmp(cur->key, key, key_len) == 0) { //key exists
            write_all(reply_fifo, cmd->arg1, cmd->arg1_len);
            write_all(reply_fifo, " ", 1);
            write_all(reply_fifo, cur->value, cur->value_len);
            write_all(reply_fifo, "\n", 1);
            return 0;
        }
        cur = cur->next;
    }
    return 0;
}

int kill_handler(Command *cmd) {
    // TODO: Implement the KILL command handler.
    return 0;
}

int update_handler(Command *cmd) {
    // TODO: Implement the UPDATE command handler.
    return 0;
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
