# SP HW2 — Distributed Key-Value Store 重點整理

> 原文：`System Programming HW2.pdf`。這份是整理版，有疑問以原文為準。


## 0. Deadlines

| | 時間 (UTC+8) |
|---|---|
| Soft deadline | 11/5 (四) 23:59 |
| Hard deadline | 11/12 (四) 23:59 |

| 遲交天數 | 日期 | 分數倍率 |
|---|---|---|
| 1–2 天 | 11/6–11/7 | ×0.85 |
| 3–5 天 | 11/8–11/10 | ×0.70 |
| 6–7 天 | 11/11–11/12 | ×0.50 |

- 以 NTU COOL 上**最後一次**提交的時間算，soft deadline 後重交（就算只改註解）也算遲交。

---

## 1. Overview

- 寫一個 `sp`，執行起來會長成一棵 **process tree**：每個 process 是一個 **node**，各自有自己的 **local table**（key-value）。
- Judge 啟動 root，也就是 **SP main**，然後對**任何 node** 直接下指令（整個過程是非同步的）。
- **fetch** 時，找不到 key 就問 parent，一路往上問到 SP main；SP main 也沒有就去問 TA 提供的 **consultant**。
- 練習重點：
  - process creation（`fork` + `exec`）
  - 同時監聽多個 channel 而不 block（judge、parent、children）→ 需要 polling
  - 移除 subtree 時**不能留下 zombie 或 FIFO**
  - **自己設計 parent/child 之間的 pipe protocol**（傳什麼、怎麼 parse）

### 1.1 Terms

| Term | 意思 |
|---|---|
| Node | 執行 `sp` 的 process |
| SP main | Root node，由 judge 執行 |
| Command channel | Node 讀指令的地方：SP main 是 **STDIN**，其他 node 是 **command FIFO** |
| Reply channel | Node 寫 fetch 答案的地方：SP main 是 **STDOUT**，其他 node 是 **reply FIFO** |
| Local table | 該 node 收到的 `store` 所存的所有 pair |
| Ancestors | 從該 node 到 root 路徑上的所有其他 node |

### 1.2 通訊架構

```
Judge ──stdin/stdout──> SP main ──stdio(pipe)──> consultant
  │                       │
  │                       │ pipes（自己設計的 protocol）
  │                       v
  └──FIFOs (cmd/reply)──> node A ── pipes ──> node B ...
```

- Judge 跟**每個 node 都直接溝通**：SP main 走 STDIO，其他 node 走一對 FIFO。
- Node 之間（parent ↔ child）走你自己開的 pipe。

---

## 2. 基本規則

### 2.1 Build / 啟動
- `make` 要在提交目錄下編出名為 `sp` 的執行檔。
- Judge 用 `./sp`（**沒有任何 command-line argument**）啟動 SP main。
- 新 node 一定要 `fork()` 再 `exec()` 你的 `sp`（新的 address space，**不能繼承 parent 的記憶體**）。
  - 如果用 `./sp` exec，**任何 node 都不要 `chdir`**。
  - 新 node 怎麼知道自己的角色和 FIFO 路徑由你決定（argv、環境變數、pipe 都可以）。

### 2.2 環境變數

| Variable | 用途 |
|---|---|
| `JUDGE_CONSULTANT` | SP main 一開始使用的 consultant 的**絕對路徑**（不要 hard-code 自己電腦上的路徑） |

### 2.3 Command processing（每個 node 都要遵守）
- Command channel 上**每一行（以 `'\n'` 結尾）= 一個 command**。
- 依照**到達順序**處理。
- Reply channel **只能寫 fetch 的答案**，一行一個，**每個都要馬上送出**（不能把多個 reply 攢起來一次送）。
- 一直跑，直到收到 `kill` 才結束。
- **SP main 讀到 stdin EOF → 當作收到 `kill`。**
- ⚠️ **STDOUT 是 protocol channel**：debug 訊息一律印到 **STDERR**。（child 會繼承 SP main 的 stdout，所以任何 node 亂印都會污染 SP main 的 reply。）

---

## 3. Keys & Values

### 3.1 格式
- 都是 UTF-8 文字，**保證沒有** ASCII control characters（newline、CR、tab、NUL）。
- **Key**：不含空白，不會是空的。
- **Value**：**key 後面那「一個」空白之後的所有內容**，可以有空白（**開頭、結尾都可以**），不會是空的。

> 例：收到 `store sphw2  is fun buthardd   \n`
> - key = `sphw2`
> - value = ` is fun buthardd   `（開頭 1 個空白、結尾 3 個空白都要保留）

### 3.2 記憶體
- 任何時刻，正確新增／刪除的情況下，每個 node 的 local table 加起來 **< 該測資 soft memory limit 的 1/4**。
  - 這個量包含 local table、正在處理或轉送中的 lookup，以及（SP main 的）consultant runs。
  - 也就是說，parse 和轉送一個 pair 時，最多還有空間**多放 3 份副本**。

### 3.3 長度
- Group 1–3：key、value 都 **< 1024 bytes**。
- Group 4：**除了 memory limit 外沒有長度上限** → **不能假設固定大小的 buffer 夠用**。

### 3.4 Tree 大小
- 任何時刻 **≤ 32 個 node**（含 SP main），**深度 ≤ 8**（SP main 是第 1 層）。

---

## 4. 六個 Commands

| Command | 格式 | 誰會收到 | 要回覆嗎 |
|---|---|---|---|
| create | `create <command-fifo> <reply-fifo>` | 任何 node | ✗ |
| store | `store <key> <value>` | 任何 node | ✗ |
| delete | `delete <key>` | 任何 node | ✗ |
| fetch | `fetch <hash> <key>` | 任何 node | ✓ `<hash> <value>` |
| kill | `kill` | 任何 node | ✗ |
| update | `update <consultant-path>` | **只有 SP main** | ✗ |

### 4.1 `create <command-fifo> <reply-fifo>`
在**收到指令的 node 底下**新增一個 child。兩個 FIFO 路徑都是**絕對路徑、不含空白**。

**流程**
1. `fork()`，child 再 `exec()` 你的 `sp`。新 node 必須是 receiving node 的 **direct child**，每個 `create` **剛好產生一個 process**。
2. 新 node 先用 `mkfifo()` **把兩個 FIFO 都建好，才能 open 任何一個**。（所在目錄保證存在，路徑本身保證不存在。）
3. 新 node 用 read 開 command FIFO、用 write 開 reply FIFO（順序不限）。Blocking `open()` 會在 judge 開了另一端之後才回傳，而 judge 要等兩個 FIFO 都存在才會開。
4. Local table 一開始是空的。
5. 之後 judge 直接透過 FIFO 對新 node 下指令。

**其他**
- Judge 會在送出**任何其他指令給任何 node 之前**先開好兩個 FIFO，而且會一直開著，直到該 node 被移除。
- FIFO 路徑可以在新 node 開好兩個 FIFO 之後**立刻刪掉**；否則就要在 node 被 kill 時刪。
- 如果收到 `create` 的是 SP main，指令走 STDIN，但新 node 一樣用 FIFO。

**Tips**
- 在 judge 開另一端之前，用 `O_NONBLOCK` open **可能會失敗**。
- 用 `O_CLOEXEC` open（或 open 完馬上設 `FD_CLOEXEC`），之後建立的 node 才不會繼承這些 fd。

**Knowledge check**
- Parent-child 用 pipe 溝通需要什麼？要開幾條 `pipe()`？
- 用了 `pipe()` 之後 fd 要怎麼管理？Child 被 reap 之後要 close fd，怎麼讓這件事比較好處理？

### 4.2 `store <key> <value>` / `delete <key>`
- `store`：把 pair 放進 **receiving node 自己**的 local table。key 已存在就**覆蓋**。
- `delete`：從 receiving node 的 local table 刪掉 key。不存在就什麼都不做。
- 其他 node 的同名 key **不受影響**。
- 兩者都**不需要回覆**，judge 也不會等。

**Knowledge check（group 4 長度無上限）**
- 一次 `read()` 有可能收不完整個字串嗎？
- Local table 放不下這個 pair 時會怎樣？

**Tips**
1. 用一個 intermediate buffer 暫存從 command channel 讀到的 bytes。
2. 用動態配置的 key-value storage。

### 4.3 `fetch <hash> <key>`
- `<hash>`：**1–16 位 lowercase hex**（相當於 64-bit integer），由 judge 選定。**同時進行中的 fetch 不會用同一個 hash。**
- 找答案的順序：
  1. 查 receiving node 自己的 local table。
  2. Miss → 問 parent，parent 查自己的 table，miss 再問它的 parent……一路到 SP main。**最近一個有這個 key 的 ancestor** 的值就是答案。
  3. SP main 也 miss → 跑 consultant（見 4.4）。
- 答案沿原路**往下傳回** receiving node，由它寫到自己的 reply channel：
  ```
  <hash> <value>
  ```
- 每個 fetch **一定有值**，沒有「找不到」的情況，回覆格式也只有上面這一種。
- 必須在時限內回覆（deadlock 一定會超時）。

**🚫 Do not cache**
- 每次 miss 都要重新往上走；每次 miss 到 SP main 都要**重新跑 consultant**。
- Consultant 對同一個 key 每次可能給不同答案，cache 遲早會錯。

**Many lookups at once**
- 一個 node 可能在回答第一個 fetch 之前又收到好幾個 fetch，同一條 ancestor path 上也可能有別的 node 同時在查。
- 待回答的 fetch **可以用任何順序回答**（judge 靠 hash 對應）。
- Tip：同一對 parent-child 之間可能同時有好幾個 lookup，有的往上、有的往下 → 在你的 protocol 裡**給每個 lookup 一個 identifier**，才知道它目前的狀態和下一步要送去哪。

**Knowledge check**
1. 往上很簡單（每個 child 只有一個 parent），那答案往下時，process 怎麼知道要送給哪個 child？
2. 有什麼資訊可以辨識 fetch 請求是從 tree 裡哪個 node 來的？
3. 知道要送給哪個 node 之後，能找出那條唯一的路徑嗎？
4. 你的 protocol 應該從什麼時候開始做 identification？

### 4.4 The consultant（只有 SP main 會跑）
- 一開始用 `JUDGE_CONSULTANT`，之後可以被 `update` 換掉。
- 每次查詢：
  1. `fork()`，`exec()` 目前的 consultant 路徑，**不帶參數**，STDIO 接到 pipe。
  2. 寫入 consultant 的 STDIN：
     ```
     <key>\n
     ```
  3. 從 consultant 的 STDOUT **讀一行**，reap 它，close 對應的 fd。
- 回覆有兩種：

| 回覆 | 意思 |
|---|---|
| `Err: <reason>` | 失敗 |
| （沒寫完一整行就 exit） | 也算失敗 |
| `Ok: <value>` | 成功；`Ok: ` 後面全部是 value（規則跟 local table 的 value 一樣） |

- 失敗 → **用同一個 consultant、同一個 key 再跑一次**，直到收到 `Ok`。
- 有些 consultant 會故意失敗，但保證之後某一次會成功；exit status 不重要。
- 保證每個 consultant 都會**先讀完整行 key** 才回答或結束，而且不會卡住。
- 每個 consultant 都是 SP main 的 child；多個 miss 同時到 SP main 時，可以**同時跑好幾個** consultant。

### 4.5 `kill`
移除 receiving node 和它**所有 descendants**。

**完成時要滿足**
- Subtree 裡每個 process 都已結束。
- 每個 process 由**自己的 parent** reap（receiving node 由它的 parent reap，SP main 由 judge reap）。
- Parent 一發現 child 結束就**立刻 reap**（不能等下一個指令），而且**所有 child 都 reap 完之前不能 exit** → 永遠不會產生 zombie。
- Subtree 裡（或為 subtree 建立的）**所有 FIFO 都已刪除**。
- 如果 receiving node 是 SP main：不能有任何 consultant 還在跑或還沒被 reap。
- `kill` 送給 SP main = 測試結束；SP main 要等**整棵樹都結束後**才以 **exit status 0** 結束。

**範例流程（kill 送給 node A，A 底下有 B）**
1. Judge 寫 `kill` 給 A。
2. A 通知 child 結束，例如 close 給 B 的 pipe（或送你 protocol 裡的訊息）。
3. B 刪掉自己建立的 FIFO，然後 exit。
4. A 看到 B 的 pipe EOF（或收到 protocol 訊息），用 `waitpid()` reap B。
5. 所有 child 都 reap 完後，A 刪掉自己的 FIFO，然後 exit。
6. SP main 看到 A 的 pipe EOF，reap A，清掉 A 用的 fd。

**Tips**
- 要叫 child 結束：可以 close 你這端的 write end，child 就會看到 EOF。
- 要知道 child 結束了：可以偵測它的 pipe EOF，再 `waitpid()`。

**⚠️ 多餘的 fd 會讓 kill 卡住**
- 如果某個 child 繼承了別條 pipe 的 write end，那條 pipe 的 reader 就**永遠看不到 EOF**，tree 就收不掉。
- `fork()` 後**立刻 close 用不到的 pipe 端**，新 process 不該繼承的 fd 要設 `FD_CLOEXEC`。

**Knowledge check**
- Parent 要 reap child，而且 child 要比 parent 先結束，收到 kill 時的執行順序是什麼？
- 你的 protocol 怎麼把 kill 傳給所有 descendants？
- 怎麼確保 leaf node 比任何 parent 先結束並被 reap？

### 4.6 `update <consultant-path>`
- **只會送給 SP main**。路徑是絕對路徑、不含空白。
- 如果路徑是你**可以執行的 regular file** → 之後每次跑 consultant 都用它。
- 否則 → **忽略**這個指令，繼續用目前的 consultant。
- 一旦接受某個路徑，judge 保證那個執行檔在下一次 `update` 之前都有效 → **只要在收到指令時檢查一次**。
- 已經在跑的 consultant 正常跑完。
- Knowledge check：要怎麼在 runtime 檢查「regular file」和「有執行權限」？

---

## 5. General Rules

### 5.1 Ordering
- 送給**同一個 node** 的指令，依 judge 寫入的順序生效。
- 送給**不同 node** 的指令之間沒有順序（node 可能在處理新指令時，它的 parent 還在處理舊的）。
- Judge 不會問依賴於「不同 node 間交錯順序」的結果。Judge 保證：
  - 送 `store k v` 給 A 之後，要等到「送給 A 的某個較晚 fetch 的 reply」證明 A 處理完那個 store，才會對 A 的 descendant fetch `k`。
  - Subtree 裡還有它送的 fetch 沒被回答時，不會 kill 那個 subtree。
  - 送出 `kill` 之後，等 kill 生效才會送下一個指令，之後才可能重用被移除 node 的 FIFO 路徑。
  - 還有需要舊 consultant 的 fetch 沒回答時，不會送合法的 `update` 換掉它。

### 5.2 Time limits（public tests 是 5 秒）
時限內必須：
- 所有新 node 都建好、兩個 FIFO 都開好。
- 每個 fetch 都被回答。
- 所有 kill 都生效（太慢的話，留下的 process 算 leftover）。

### 5.3 Restrictions
- Key-value pair **只能存在收到該 `store` 的 node** 的 local table。
- `<value>` 可以在 node 之間傳遞來回答 fetch，但**之後任何 process 都不能保留它**（memory、file、shared memory 都不行）。
- 🚫 **不能用 `system()` 或 `popen()`**。每個 `create`、每次跑 consultant 都**只能產生一個 process**。

---

## 6. Grading

### 6.1 配分
| Group | 內容 | 總分 | Public (30%) | Private (70%) | 折合 7 分 |
|---|---|---|---|---|---|
| 1 | 只有 SP main：`store`、`delete`、`fetch`、`kill`，沒有 miss | 7 | 2.1 | 4.9 | 0.49 |
| 2 | `create`、透過 ancestors 查詢、多個 lookup 同時進行 | 21 | 6.3 | 14.7 | 1.47 |
| 3 | Miss 到 consultant、`update` | 29 | 8.7 | 20.3 | 2.03 |
| 4 | SP main 以下的 `kill`、長 key/value、resource hygiene | 43 | 12.9 | 30.1 | 3.01 |
| | **Total** | **100** | 30 | 70 | **7** |

- 每個 group 都可能用到前面 group 的所有指令，每個測試最後都會移除整棵樹。
- 最終分數 = score × 7 / 100。
- 有些測試會跑很多次（抓 timing bug），**每次都要過**，以最差的那次計分。
- Public 全過 ≠ private 全過。

### 6.2 該次 run 拿 0 分的情況
- Fetch 的 `<value>` 錯、時限內沒回覆、或回覆寫在錯的 channel。
- Reply channel 出現**沒有對應任何 fetch** 的行。
- `create` 沒有剛好產生一個新 process，或新 process 不是 receiving node 的 child。
- `create` 產生的 child 沒有執行你的 `sp`，或沒開好兩個 FIFO。
- SP main 沒有「每個到達它的 miss 剛好跑一次 consultant，每次失敗再多跑一次」。
- Node 不再讀指令、node 還活著但 reply channel 被關掉、或寫了不合法的 reply。
- Judge 檢查時 process tree 結構錯誤（在先前的 fetch 都回答完後不定時檢查，kill 的 leftover 另外算）：
  1. 有 consultant 跑完沒被 reap。
  2. SP main 底下出現不是 node 的其他 process。
  3. Node 意外結束，或它的 parent 不是建立它的那個 node。
- 超過 hard resource limit。
- 對 SP main 送 `kill` 後，exit status 不是 0。
- Group 4 中，`kill` 留下任何東西。

### 6.3 Resource limits
四個面向：process 數量、open fd 數量、每個 process 的 peak memory（**每個時間點都算**）、CPU time。

- Public tests：soft memory **64 MiB / process**（最長 value 那筆是 **32 MiB**），soft CPU **5 秒**，每個 hard limit = **2 × soft**。各 public test 的檔案有列出自己的 limits。
- 超過 hard limit → 該次 run 失敗。
- 超過 soft limit：1 個面向 **−50%**，2 個以上 **−75%**。

### 6.4 Leftovers
`kill` 該刪而沒刪的東西（process、沒被 reap 的 child、FIFO 路徑）都算 leftover。
- Group 1–3：每個 leftover **−10%**。
- Group 4：**直接 fail**。
- 扣分總和不會超過該測試的分數。

### 6.5 Build
- Judge 在跑任何測試前先 `make` 一次；build 失敗或超時 → **整份作業 0 分**。

---

## 7. Public test & Submission

- 跑 public tests：`make test`（public 全過會顯示 100 / 100）。
- Judge dashboard 在畫面上時，會丟掉你的程式和 consultant 輸出的所有東西 → 想看 stderr 用 `make test > report.txt`。
- 打包：`make submission`（記得填 student id）。
- 交到 NTU COOL：`<student_id>.zip`，檔案**直接放在 zip 最上層**，不要包一層資料夾：
  ```
  b14902067.zip
  ├── sp.c
  ├── sp.h
  ├── Makefile
  └── （其他需要的 .c / .h）
  ```
- ⚠️ **矛盾**：template 的 `make submission` 會把檔案包在 `<student_id>/` 資料夾裡，跟 spec 說的「不要包一層資料夾」相反。交之前用 `unzip -l` 看一下，或去 GitHub Issues 問清楚。
- `Makefile` 的 `SUBMISSION_FILES` 預設只有 `sp.c sp.h`，有其他 source 要自己加上去。
- **不要交**：binary（含 `sp`）、object file、judge 檔案、test plan、public test 產生的檔案、core dump、FIFO……
- `make` 要能從你所有的 source 編出 `sp`；`make clean` 要刪掉 build 產生的檔案。改 Makefile 時保留 `test` target。

---

## 8. FAQ 摘要

| Q | A |
|---|---|
| Error message 怎麼寫？ | 用 `perror()` / `strerror(errno)`，要知道**為什麼**失敗（路徑錯、方向錯、FIFO 不存在……）。問 TA 時至少附上錯誤回傳值。 |
| `mkfifo()` 回 "Operation not supported"？ | 多半是 WSL 的目錄不支援，可以先在 `/tmp/` 下測（記得刪掉）。最後一定要在工作站上跑。Node 必須在 `create` 給的路徑建 FIFO。 |
| Node 可以 cache 轉送過的 value 嗎？ | **不行。** 每次 miss 都要重新往上走，到 SP main 就要重跑 consultant；judge 會數 consultant 執行次數。 |
| 可以印進度訊息嗎？ | 不能印到 STDOUT，印到 **STDERR**。交之前把不需要的 diagnostic 拿掉。 |
| 要改 Makefile 嗎？ | 有多個 source 時要改。保留 `test` target。 |
| `kill` 一直不結束 / 有 leftover process？ | 通常是某個 process 還拿著 pipe 的 write end。`fork` 後 close 用不到的端、設 `FD_CLOEXEC`、child 一結束就 `waitpid()`。 |
| 問題要去哪問？ | Template repo 的 GitHub Issues（不是你自己的 repo）。 |

TA email：Class 1 `swnosp@csie.ntu.edu.tw`；Class 2 `ntusp2026@gmail.com`

---

## Appendix：測資 `steps` 怎麼看（非 spec 內容，是看 `tests/public` 推出來的）

- `store main color blue` 的 `main`、`a`、`n2`…… 是 **judge 這邊的 node 名稱**，用來指定要送給哪個 node。**你的 `sp` 收到的那一行不含 node 名稱**（例如收到 `store color blue`）。
- `create main a` = judge 叫 `main` 建立一個叫 `a` 的 child，實際送出的是 `create <cmd-fifo> <reply-fifo>`。
- `fetch main k` 實際送出的是 `fetch <hash> k`，hash 由 judge 產生。
- `wait`、`reseed` 看起來是 judge 自己的控制步驟（等待回覆、重設 consultant 狀態），不會送給你的程式。
- `update other`、`update !missing` 的參數應該是 judge 這邊的 consultant 代號，實際會被換成絕對路徑。
