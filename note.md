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
