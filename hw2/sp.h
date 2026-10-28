#ifndef SP_H
#define SP_H

#include <stdio.h>
#include <sys/types.h>
#define MAX_FDS 512
#define STDIN 0
#define BUF_SIZE 4096
#define ERR_EXIT(msg)          \
    do {                       \
        perror(msg);           \
        exit(EXIT_FAILURE);    \
    } while (0)


typedef struct entry {
    char* key;
    size_t key_len;
    char* value;
    size_t value_len;
    struct entry* next;
} Entry;


typedef struct {
    char *buf;
    size_t len;
    enum { CREATE, STORE, DELETE, FETCH, KILL, UPDATE, UNKNOWN } type;
    char *arg1;
    size_t arg1_len;
    char *arg2;
    size_t arg2_len;
} Command;

typedef struct {
    int    fd;
    pid_t  pid;
    char*  buf;
    size_t len; //buf 裡目前有幾個 byte
    size_t cap; //size of buf
    size_t start; //next start
    size_t scanned; //have scanned
    enum {COMMAND, PARENT_PIPE, CHILD_PIPE, CONSULT} role;
    int child_slot;
}Conn;

typedef struct {
    int   in_use;
    pid_t pid;
    int   write_fd; //parent to child
    int   read_fd; //child to parent
}Child;

typedef struct {
    char* hash;
    char* origin; //pending 是從 child 還是 judge 來
    int slot; //還給哪個child
    char* key; //要詢問的key
}Pending;

enum input_status {
    INPUT_ERROR = -1,
    INPUT_EOF = 0,
    INPUT_DATA = 1,
    INPUT_RETRY = 2
};
void conn_init(Conn* c, int fd);
void conn_free(Conn* c);
int parse_command(char *line, size_t len, Command *cmd);
int command_handler(Command *cmd);
int create_handler(Command *cmd);
int store_handler(Command *cmd);
int delete_handler(Command *cmd);
int fetch_handler(Command *cmd);
int kill_handler(Command *cmd);
int update_handler(Command *cmd);
int drain_lines(Conn* c);
int add_fd(int fd, short events);
int remove_fd(int i);
int set_cloexec(int fd);
void shutdown_node(void);
void free_entry(Entry* ent);
Entry* new_entry(char* key, size_t key_len, char* value, size_t value_len);
int write_all(int fd, const char* ptr, size_t len);
#endif // SP_H