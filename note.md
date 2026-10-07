## 可以用的函式

```send_text```: 回傳訊息到 client 端
e.g. ```send_text(req->conn_fd, ">>> Locked.\n");```
```snprintf```: 把格式化的文字傳到一個字串
e.g.
```
char msg[MAX_MSG_LEN];
snprintf(msg, sizeof(msg), ">>> Account %d balance: %d\n", 902001, 500);
// msg 現在是 ">>> Account 902001 balance: 500\n"
send_text(req->conn_fd, msg);
```

## 可以用的常數
```MAX_MSG_LEN```: 訊息最長的長度
