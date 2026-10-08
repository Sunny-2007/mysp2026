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


3. read 和 update 的鎖用途不同

| 指令 | 鎖 | 持有多久 |
|---|---|---|
| `read` | `F_RDLCK` | 讀完馬上 `F_UNLCK` |
| `update` | `F_WRLCK` | 一直到 `close` 或斷線（在 `release_update` 裡解） |

```
Client A (fd 5): update 902001
    record_owner[0] = 5

Client B (fd 6): read 902001
    record_owner[0] belongs to fd 5
    Return Locked without calling F_SETLK.

Client B: read 902002
    record_owner[1] == -1
    Attempt F_RDLCK; if successful, read and release it.
    record_owner[1] stays -1.
```



## request 結構的用途

`request` 是**每個 client 各自的狀態**，用來記住這個 client 在跨指令之間需要記得的事。

handler 每處理完一行指令就 return，區域變數（像 `rec`、`idx`）也跟著消失。但有些資訊要撐過好幾行指令：

```
update 902001    ← 記住：正在 update 902001、餘額 500
add 100          ← 要知道是哪個帳號、目前多少錢
close            ← 要知道要解哪個帳號的鎖
```

這些資訊存在 `requestP[fd]`，每個連線的 fd 對應一格。用 `select` 同時服務多個 client 時，彼此的狀態才不會混在一起。

| 欄位 | 用途 |
|---|---|
| `conn_fd` | 這個 client 的 socket，回訊息時用 |
| `buf`、`buf_len` | 還沒處理完的輸入（半行、或一次收到好幾行）。TODO 3 會用到 |
| `state` | 目前在哪個狀態（READY、WAIT_UPDATE、…），決定下一行指令交給哪個 handler |
| `account_index` | 目前持有的帳號（update 中，或 transfer 的來源帳號）；READY 時為 `-1` |
| `current_balance` | 持有中帳號的餘額（update 時用） |
| `peer_fd` | transfer 時的對方 client（TODO 4） |
| `target_index`、`target_balance` | transfer 的目標帳號和餘額（TODO 4） |
| `amount` | transfer 的金額（TODO 4） |
| `close_pending` | 通知對方失敗，標記成待關閉（TODO 4） |

**判斷要不要存進 req**：下一行指令還需要這個資訊嗎？
- 需要，存進 `req`（例如 update 的帳號和餘額）
- 不需要，用區域變數就好（例如 read 讀到的 `rec`）

所以 read 不該設定 `account_index`：read 做完之後沒有後續指令需要它，而 READY 狀態的約定是 `account_index == -1`。

## listen fd 與 client fd

| | listen fd | client fd |
|---|---|---|
| 數量 | 整個 server **只有一個** | **每個 client 一個** |
| 在哪建立 | `init_server` 裡的 `socket` → `bind` → `listen` | 每次 `accept` 回傳一個新的 |
| 用途 | **只用來接受新連線** | **和那個 client 收發資料**（`read`、`write`） |
| 存在哪 | `svr.listen_fd` | `requestP[fd].conn_fd` |
| select 說它可讀，代表 | 有新的 client 在排隊等連線 | 這個 client 送資料來了，或斷線了 |

比喻：listen fd 是餐廳門口的**帶位員**，只負責迎接新客人；client fd 是**每一桌**，點餐上菜都在這桌進行。

```c
// init_server：建立 listen fd（只做一次）
svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
bind(svr.listen_fd, ...);      // 綁定 port
listen(svr.listen_fd, 1024);   // 開始接受連線，1024 是排隊上限

// serve_clients：每次 accept 產生一個 client fd
int fd = accept(svr.listen_fd, ...);   // 從 listen fd 取出一個新連線
requestP[fd].conn_fd = fd;             // 之後用這個 fd 和這個 client 溝通
```

- 不要對 listen fd 呼叫 `read` 或 `write`，它沒有連到任何 client
- select 時兩種都要放進 `master`：listen fd 用來知道有新連線，client fd 用來知道誰送了指令

### 為什麼 listen fd「可讀」代表有新連線

select 的「可讀」真正的意思是：**對這個 fd 做對應的讀取操作，不會卡住**。

- client fd 對應的讀取操作是 `read`，所以「可讀」代表 buffer 裡有資料（或對方斷線，`read` 會馬上回傳 0）
- listen fd 不能 `read`，它對應的操作是 `accept`，所以「可讀」代表 **`accept` 會馬上回傳**

而 `accept` 什麼時候會馬上回傳？要看 kernel 怎麼處理連線：

1. client 呼叫 `connect`，**kernel 自動**和它完成 TCP 三次握手，不需要 server 程式參與
2. 握手完成的連線，被 kernel 放進這個 listen socket 的 **accept 佇列**（排隊上限就是 `listen` 的第二個參數）
3. server 呼叫 `accept`，從佇列取出一個連線，回傳新的 client fd

所以：
- 佇列是空的：`accept` 會卡住等待，select 不會把 listen fd 標成可讀
- 佇列裡有連線：`accept` 會馬上回傳，select 會把 listen fd 標成可讀

這也解釋了舊程式的現象：A 還在線上時 B 連進來，B 的握手已經由 kernel 完成、排在佇列裡了，但程式卡在 `read(A)`，沒有人呼叫 `accept`，所以 B 收不到歡迎訊息。
