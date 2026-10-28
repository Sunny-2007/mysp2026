# 輸入處理實作指引（Step by Step）

> 目標：讓每個 node 都能從任意 fd（stdin、FIFO、pipe、consultant stdout）**完整、正確、有效率**地讀到一行一行的指令。
> 這份只講**每個函式要負責什麼、輸入輸出是什麼、要注意什麼**，不附實作。code 請自己寫（spec 規定用 AI agent 寫作業算作弊）。
> 函式名稱都只是建議，可以自己改。

## 總覽：資料流

```
fd ──read()──> Conn.buf（動態 buffer）──找 '\n'──> 一行 ──parse──> Command ──dispatch──> handler
                  ^                                                              
                  └── 剩下的半行留著，等下一次 read
```

| Step | 函式 | 一句話 |
|---|---|---|
| 0 | — | 先讓程式編得過 |
| 1 | `Conn` 結構 | 決定 buffer 要記哪些狀態 |
| 2 | `conn_init` / `conn_free` | 建立 / 釋放一個 Conn |
| 3 | `conn_reserve` | 確保 buffer 至少還有 N bytes 空間 |
| 4 | `conn_read` | `read()` 一次，接到 buffer 後面 |
| 5 | `conn_next_line` | 從 buffer 拿出下一行完整的指令 |
| 6 | `conn_compact` | 把剩下的半行搬到開頭 |
| 7 | `parse_command` | 把一行切成 `Command` |
| 8 | `drain_lines` | 把 buffer 裡所有完整的行都處理掉 |
| 9 | `add_fd` / `remove_fd` | 管理 `fds[]` 和 `conns[]` |
| 10 | `main` loop | `poll` → 讀 → 處理 |

每一步做完都有 ✅ **Checkpoint**，**過了再往下**。

---

## Step 0：先讓程式編得過

現在的 `sp.c` 混了很多 hw1 的 code，先清乾淨，不然後面無法測試。

- [ ] 刪掉 hw1 留下的東西：`parse_account_command`（還寫成 `tatic`）、舊的 `pop_command`、`drain_commands` 裡的 `req`、`send_text`、`handle_command`、`MAX_MSG_LEN`。
- [ ] `sp.h` 用到 `pid_t`，要 `#include <sys/types.h>`。
- [ ] 要用 `bool` 的話 `#include <stdbool.h>`。
- [ ] `POLL_IN` → `POLLIN`。
- [ ] `remove_fd` 宣告回傳 `int` 但沒有 `return`。
- [ ] 每個 handler 都宣告回傳 `int`，先暫時都 `return 0`。
- [ ] Makefile 的編譯指令建議加上 `-Wall -Wextra -g`，讓 compiler 幫你抓錯。開發時也可以加 `-fsanitize=address,undefined` 抓記憶體錯誤（但交作業前要拿掉，它會讓記憶體用量暴增）。

✅ **Checkpoint 0**：`make` 沒有 error，最好連 warning 都沒有。

---

## Step 1：設計 `Conn`

你現在的 `Conn` 已經有 `buf`（指標）、`len`、`cap`，方向是對的。要補兩件事：

| 欄位 | 意思 | 備註 |
|---|---|---|
| `fd` | 從哪個 fd 讀 | 已有 |
| `buf` | `malloc` 出來的空間 | 已有 |
| `cap` | `buf` 配置了多大 | **改成 `size_t`**，跟 `len` 同型別，避免比較時 signed/unsigned 混用 |
| `len` | `buf` 裡目前有幾個有效 byte | 已有 |
| `start` | 下一行從哪裡開始（已處理到哪） | **新增**，避免每處理一行就 `memmove`（見 Step 5、6） |
| `scanned` | 從 `start` 之後，已經確認**沒有** `'\n'` 的位置 | **新增**，避免每次 `read` 完都從頭 `memchr` |

**想清楚這些不變式（invariant），之後 debug 會輕鬆很多：**
- `0 ≤ start ≤ scanned ≤ len ≤ cap`
- `buf[start .. len)` 是還沒處理的資料
- `buf[start .. scanned)` 裡面**一定沒有** `'\n'`

`pid` 欄位：之後 child / consultant 的 Conn 會用到，現在先留著。

✅ **Checkpoint 1**：用紙筆模擬一次：buffer 裡有 `"ab\ncd"`，處理完第一行之後，`start`、`scanned`、`len` 各是多少？

---

## Step 2：`conn_init` / `conn_free`

**`conn_init(Conn *c, int fd)`**
- 設定 `fd`，其他數字欄位都歸零。
- `buf` 可以先設 `NULL`、`cap = 0`，等第一次讀的時候再配置；也可以一開始就配置一塊初始空間（例如 4096）。兩種都可以，選一種就好。

**`conn_free(Conn *c)`**
- `free(buf)`，把指標設成 `NULL`、數字歸零。
- 要不要在這裡 `close(fd)`？自己決定，但**全程式只能有一個地方負責 close**，不然會 close 兩次（第二次可能關到別人剛拿到的同號 fd）。

✅ **Checkpoint 2**：對同一個 Conn 呼叫 `conn_init` → `conn_free` → `conn_free` 不會 crash（`free(NULL)` 是安全的）。

---

## Step 3：`conn_reserve(Conn *c, size_t need)`

**責任**：保證 `cap - len >= need`。不夠的話就讓 buffer 長大。

**步驟**
1. 空間夠就直接回傳成功。
2. 不夠：算新的 `cap`，**從目前的 cap 一直加倍**，直到夠為止（`cap` 是 0 的話先給一個初始值）。
3. `realloc` 到新大小。
   - ⚠️ **回傳值先存到暫存變數**，確認不是 `NULL` 才換掉 `buf`。失敗的話原本的 `buf` 還在，不能弄丟。
4. 更新 `cap`。

**回傳**：成功 0、失敗 -1（或 `bool`）。

**注意**
- `realloc` 之後 `buf` 可能換了位址 → **任何指向舊 `buf` 的指標都失效**。所以 `start`、`scanned` 用 index，不要用指標。
- 加倍（不是每次 +4096）才能讓大 value 不會變成平方時間。

**加分（Step 10 之後再做也可以）**：在 reserve 之前，如果 `start > 0`，可以先 compact（Step 6），說不定就不用長大了。

✅ **Checkpoint 3**：寫一個小測試：從 `cap = 0` 開始，連續 reserve 1、100、5000、1000000，每次印 `cap` 到 stderr，確認是在加倍、而且一定 ≥ `len + need`。

---

## Step 4：`conn_read(Conn *c)`（取代原本的 `append_input`）

**責任**：對 `c->fd` 呼叫**一次** `read()`，把讀到的東西接在 `buf + len` 後面。

**步驟**
1. 呼叫 `conn_reserve`，確保至少有一塊合理的空間可以讀（例如 4096 或更大）。
2. `read(fd, buf + len, cap - len)`。
3. 依照結果回傳：

| `read` 回傳 | 意思 | 你回傳 |
|---|---|---|
| `> 0` | 讀到 n bytes → `len += n` | `INPUT_DATA` |
| `0` | 對方已關閉（EOF） | `INPUT_EOF` |
| `-1` 且 `errno == EINTR` | 被 signal 打斷（之後 SIGCHLD 很常見） | 重讀一次，或回傳「沒事，下次再讀」 |
| `-1` 其他 | 真的錯了 | `INPUT_ERROR`（用 `perror` 印到 stderr） |

**注意**
- **只 `read` 一次**。fd 是 blocking 的，poll 只保證「現在讀一次不會卡住」，再讀第二次可能就卡住整個 node。
- 不要用 `sizeof(c->buf)`：`buf` 現在是指標，`sizeof` 只會得到 8。
- 不要用 `strlen`：buffer 不是 C 字串。
- 不要混用 `fgets` / `scanf` / `getchar`（stdio 有自己的 buffer，會跟 poll 打架）。

✅ **Checkpoint 4**：在 main 裡暫時寫一個簡單迴圈：一直 `conn_read(stdin 的 Conn)` 直到 EOF，每次把 `len` 印到 stderr。
```
printf 'hello\nworld\n' | ./sp
python3 -c "print('x' * 10_000_000)" | ./sp     # 大資料：最後 len 要是 10000001
```

---

## Step 5：`conn_next_line(Conn *c, char **line, size_t *line_len)`

**責任**：如果 `buf[start .. len)` 裡有完整的一行，就把它交出來；沒有就說沒有。

**步驟**
1. 從 `scanned` 開始（**不是**從 `start`，更不是從 0）用 `memchr` 找 `'\n'`，範圍是 `len - scanned`。
2. **沒找到**：把 `scanned` 設成 `len`（這段都確認過沒有換行了），回傳 0。
3. **找到了**，假設 `'\n'` 在 index `nl`：
   - 這一行是 `buf[start .. nl)`，長度 `nl - start`。
   - 把 `buf[nl]` 改成 `'\0'` → 這一行可以直接當 C 字串用，**不用另外複製**。
   - `*line` 指向 `buf + start`、`*line_len` 設成長度。
   - 把 `start` 和 `scanned` 都移到 `nl + 1`。
   - 回傳 1。

**注意**
- 交出去的 `*line` 指向 `buf` 內部 → **在下一次 `conn_read` / `conn_reserve` / `conn_compact` 之前有效**。處理完這一行之前不要呼叫它們。如果某個指令要「留著等回應」（例如 fetch 要等 parent），那個 handler 要自己 `malloc` 一份需要的欄位。
- 為什麼要另外記 `line_len`？雖然 spec 保證不會有 NUL，但拿著長度就不用再 `strlen`（大 value 時 `strlen` 也要時間）。
- 這個函式**不搬資料**，只移動 index。

✅ **Checkpoint 5**：Step 4 的迴圈改成「讀一次 → 一直 `conn_next_line` 直到 0」，把每一行用 `[` `]` 包起來印到 stderr，才看得到前後空白：
```
printf 'a\nb\nc\n' | ./sp                    # 一次讀到三行 → [a] [b] [c]
(printf 'sto'; sleep 1; printf 're k v\n') | ./sp   # 半行分兩次到 → [store k v] 只出現一次
printf 'x  y  \n' | ./sp                     # → [x  y  ]，空白都在
printf '\n' | ./sp                           # 空行 → []，不會 crash
```

---

## Step 6：`conn_compact(Conn *c)`

**責任**：把已經處理掉的部分丟掉，剩下的半行搬到 buffer 開頭。

**步驟**
1. `start == 0` → 不用做。
2. `memmove(buf, buf + start, len - start)`。（不能用 `memcpy`，來源和目的可能重疊。）
3. `len`、`scanned` 都減掉 `start`，`start` 設成 0。

**什麼時候呼叫**：一批行**全部處理完**之後呼叫一次（Step 8），不是每一行處理完就呼叫。

**加分：縮小 buffer**
- 處理完一個超大的指令後，`cap` 可能停在幾十 MB，但裡面幾乎是空的。
- 如果 `len` 很小、`cap` 很大（例如超過某個門檻，而且 `len` 不到 `cap` 的 1/4），可以 `realloc` 縮小。
- Spec 的 memory 是**每個時間點都算 peak**，group 4 的 32 MiB 測資會用到。

✅ **Checkpoint 6**：Checkpoint 5 的測試全部再跑一次，結果不變。再加上：
```
python3 -c "print('store big ' + 'x' * 10_000_000)" | time ./sp
```
應該在 1 秒內結束（如果很慢，八成是有地方每次都從頭掃，或是每行都 memmove）。

---

## Step 7：`parse_command(char *line, size_t len, Command *cmd)`

**責任**：把一行切成 `cmd->type` 和參數。**只切，不執行。**

### 7.1 先擴充 `Command`
你現在的 `Command` 只有 `buf`、`len`、`type`。建議加上兩個參數欄位（每個都是「指標 + 長度」），因為每個指令最多兩個參數：

| type | arg1 | arg2 |
|---|---|---|
| `CREATE` | command FIFO 路徑 | reply FIFO 路徑 |
| `STORE` | key | value |
| `DELETE` | key | — |
| `FETCH` | hash（**存字串，不要轉數字**） | key |
| `KILL` | — | — |
| `UPDATE` | consultant 路徑 | — |

`buf`/`len` 可以保留成「整行原文」，之後轉送時會用到。

### 7.2 切法
1. 找到第一個空白（`memchr(line, ' ', len)`）。沒有空白 → 整行就是指令名稱（例如 `kill`）。
2. 比對指令名稱：**要同時比長度和內容**，不然 `storex` 會被當成 `store`。（`strncmp` 只比前 n 個字，要自己多檢查長度。）
3. 依照 type 切參數：
   - **一般參數**（create、delete、fetch、update，還有 store 的 key）：到下一個空白或行尾為止。
   - **store 的 value**：key 後面**只跳過一個空白**，之後**全部**都是 value，到行尾為止。
     - `store k  hi ` → value 是 ` hi `（開頭和結尾的空白都保留）。
4. 要不要把參數之間的空白改成 `'\0'`？
   - 改的話，每個參數都能直接當 C 字串用（`mkfifo`、`access` 這些系統呼叫需要 C 字串）。
   - **value 的結尾已經是 Step 5 放的 `'\0'`**，不用再處理。
   - 但如果之後要整行原文轉送，被改過的那行就不是原文了 → 想清楚要不要保留原文，或者轉送時用欄位重新組。
5. 不認得的指令 → `UNKNOWN`。spec 保證格式正確，不用防呆，但**不能在 reply channel 印任何錯誤訊息**（要印就印到 stderr）。

### 7.3 不要用 `strtok`
- 它會把連續空白當成一個，value 的空白就壞了。
- 它用全域狀態，巢狀使用會出事。

✅ **Checkpoint 7**：把 Checkpoint 5 的印出改成印出 parse 結果，例如 `type=STORE arg1=[k] arg2=[  hi ]`：
```
printf 'store k v\n'                 # arg1=[k] arg2=[v]
printf 'store k  hi \n'              # arg2=[ hi ]（開頭 1 個空白）
printf 'store sphw2  is fun buthardd   \n'   # spec 的例子
printf 'fetch 3f k\n'                # arg1=[3f] arg2=[k]
printf 'create /tmp/a.cmd /tmp/a.reply\n'
printf 'kill\n'
printf 'update /bin/ls\n'
printf 'storex k v\n'                # UNKNOWN
```
（每行都 `| ./sp`，或全部串在一起一次送進去。）

---

## Step 8：`drain_lines(Conn *c)`

**責任**：一次 `read` 之後，把 buffer 裡**所有**完整的行依序處理完。

**步驟**
1. 迴圈：`conn_next_line` 拿到一行 → `parse_command` → `command_handler`。
2. 拿不到完整的行就停。
3. 停下來之後呼叫**一次** `conn_compact`。

**注意**
- 一定要**依照順序**處理（spec：commands take effect in the order they arrive）。
- **`kill` 之後**：這個 node 要開始結束，buffer 裡後面的行就不用處理了。想好 handler 要怎麼告訴 `drain_lines`「停下來」（例如回傳值）。
- 這個函式只負責「judge → node」的指令。之後 parent/child/consultant 的 pipe，可以共用 Step 2–6，但 parse 和 dispatch 換成你自己 protocol 的版本。

✅ **Checkpoint 8**：
```
printf 'store a 1\nstore b 2\nkill\nstore c 3\n' | ./sp
```
stderr 應該看到前三個指令依序被處理，`store c 3` 沒有被處理。

---

## Step 9：`add_fd` / `remove_fd`

你的設計是 `fds[i]` 和 `conns[i]` 共用 index，所以**兩個陣列永遠要一起動**。

**`add_fd(int fd, short events)`**
- 放到 `fds[nfds]`，**同時** `conn_init(&conns[nfds], fd)`。
- 回傳新的 index（你現在回傳的是 `nfds++` 之後的值，等於 index + 1，要確認你要的是哪個）。

**`remove_fd(int i)`**
1. `close` 那個 fd、`conn_free(&conns[i])`。
2. 把最後一個（`nfds - 1`）**搬到第 `i` 格：`fds` 和 `conns` 都要搬**。
3. `nfds--`。

**注意**
- `Conn` 裡有 `malloc` 出來的指標，搬過去之後**最後一格不要再 free**（不然同一塊會被 free 兩次）。搬完把最後一格的 `buf` 設成 `NULL` 比較保險。
- 在 Step 10 的迴圈裡 remove 之後，第 `i` 格已經換成別人了 → **`i` 不要 `++`**，要再檢查一次這一格；或者改成從後面往前掃。

✅ **Checkpoint 9**：暫時加 3 個 fd（例如 `open("/dev/null")` 三次），remove 中間那個，印出 `fds[i].fd` 和 `conns[i].fd`，確認兩邊對得上。

---

## Step 10：`main` 的 poll 迴圈

**步驟**
1. 初始化：SP main 把 `STDIN_FILENO` 用 `POLLIN` 加進去（之後 FIFO / pipe 也是同樣方式加入）。
2. 迴圈：
   1. `poll(fds, nfds, -1)`：**timeout 給 -1**（沒事就睡，不要 busy loop 吃 CPU）。
   2. `poll` 回傳 -1 且 `errno == EINTR` → `continue`；其他錯誤 → 印 stderr。
   3. 掃每一個 `i`，看 `fds[i].revents`：
      - 有 `POLLIN` → `conn_read` 一次 → 依結果處理：
        - `INPUT_DATA` → `drain_lines`
        - `INPUT_EOF` → 這個來源結束了（見下方）
        - `INPUT_ERROR` → 印 stderr，看情況處理
      - 只有 `POLLHUP` / `POLLERR`、沒有 `POLLIN` → 當成 EOF 處理。
      - ⚠️ `POLLHUP` 和 `POLLIN` **可能同時出現**（對方寫完就關了，但資料還沒讀完）→ **先讀，讀到 EOF 再處理關閉**。
3. **EOF 怎麼處理**
   - SP main 的 stdin EOF → **當成收到 `kill`**（spec 2.2.4）。
   - 如果 buffer 裡還剩沒有 `'\n'` 的半行：spec 說只有以 `'\n'` 結尾的才算指令，所以丟掉就好。
   - 其他 fd 的 EOF 怎麼處理，之後寫 kill / child / consultant 時再決定。

**注意**
- `revents` 用 `&` 檢查（`revents & POLLIN`），不要用 `==`，因為可能同時有好幾個 flag。
- Step 9 說的：remove 之後 index 會變，迴圈要小心。

✅ **Checkpoint 10**（最終）：
```
printf 'store a 1\nfetch 1 a\n' | ./sp          # 指令依序被處理，EOF 後程式結束
(printf 'store k v\n'; sleep 2; printf 'kill\n') | ./sp   # 等待時用 top 看 CPU：應該是 0%
python3 -c "print('store big ' + 'x' * 10_000_000)" | /usr/bin/time -l ./sp   # macOS：看 maximum resident set size
```
最後跑 `make test`，看 group 1（single_node）有沒有因為輸入處理出錯。（fetch 的回覆要等你寫完 handler 才會對。）

---

## 最後檢查清單

- [ ] 沒有任何固定大小的 buffer 用來裝指令或 value
- [ ] `realloc` 有檢查 `NULL`，而且沒有保留舊 `buf` 的指標
- [ ] `memchr` 從 `scanned` 開始找，不是從頭
- [ ] 一批行處理完才 `memmove` 一次
- [ ] value 開頭和結尾的空白都保留
- [ ] 一次 `POLLIN` 只 `read` 一次
- [ ] `poll` timeout 是 -1
- [ ] 處理了 `EINTR`
- [ ] stdin EOF → 當作 `kill`
- [ ] `fds[]` 和 `conns[]` 永遠一起搬
- [ ] STDOUT 只會有 fetch 的答案，所有 debug 都印到 stderr
- [ ] 交作業前把 debug 印出和 `-fsanitize` 拿掉
