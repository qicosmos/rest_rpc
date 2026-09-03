# rest_rpc 单连接请求复用设计

## 1. 背景

`rest_rpc_header` 已包含 `uint64_t seq_num`，但当前 client 发送请求时没有设置该字段，server 返回响应时也没有回传它。与此同时，每个 `call()` 都会直接从同一个 socket 读取响应。

这会带来两个问题：

1. 同一连接上不能安全地同时存在多个读操作。
2. 请求按照 `1、2、3` 发出、响应按照 `3、2、1` 返回时，client 无法判断每个响应属于哪个 `co_await`。

本设计使用协议头中的 `seq_num` 关联请求和响应，使多个 RPC 可以复用同一条 TCP 连接，并允许响应乱序到达。

## 2. 目标

- 同一 client 连接允许存在多个并发的在途 RPC。
- 请求和响应通过 `seq_num` 一一关联，不依赖响应顺序。
- 请求按照 `1、2、3` 发出、响应按照 `3、2、1` 返回时，三个等待中的协程都能得到各自的结果。
- 保持现有 `call()`、`call_for()` 和 `rpc_context::response()` API 不变。
- 保持跨端序、延迟响应和发布订阅能力。
- 连接内状态不引入 mutex。

## 3. 非目标

- 本次不把 server 的普通 handler 改成并行执行。现有 server 读循环仍可以依次执行普通 handler；`rpc_context` 延迟响应或其他兼容协议的服务端可以产生乱序响应。
- 本次不增加跨连接的请求迁移和自动重试。RPC 是否可以安全重试取决于业务幂等性。
- 本次不改变序列化格式以及 `function_id` 的生成方式。

### 3.1 本次落地范围

结合“尽量复用现有接口和代码”的约束，本次实现不新增公开配置项，也不改变 `call()`、`call_for()`、`subscribe()`、`rpc_context::response()` 和 `rpc_connection::response()` 的既有调用方式。落地内容包括：非零 seq 分配与回传、client 单读循环、client/server 单写队列、pending 按 seq 分发、单请求超时、迟到响应隔离、连接错误批量完成、重连 generation 隔离、旧 peer 的安全降级，以及 `rpc_context` 副本共享一次响应状态。

后文中的 body/队列可配置上限、半帧读写超时、显式取消协议和 coroutine-local handler context 是完整加固设计。它们需要先确定默认值或新增配置/API，不在本次兼容性实现中隐式加入；相关异常和测试要求保留在文档中，作为后续加固边界，避免把“已考虑”误写成“已实现”。本次仅为迟到 seq 记录采用内部固定上限，防止服务端长期不返回造成该状态无限增长。

## 4. 线程模型与约束

rest_rpc 的运行模型是一个线程运行一个 `io_context`，一个 socket 及其连接状态只在所属 `io_context` 线程内访问。因此：

- `seq_num` 使用普通 `uint64_t`，不需要 atomic。
- pending map 和写队列使用普通容器，不需要 mutex。
- 响应分发和超时清理都在同一 `io_context` 上执行，不存在容器并发访问。

单线程不代表可以重叠发起同类异步 I/O。一个协程在 `async_write` 处挂起后，另一个协程仍可能在前一个写操作完成前运行。因此每条连接仍需满足：

- 任意时刻只有一个异步读操作。
- 任意时刻只有一个异步写操作。

实现上使用一个常驻读循环和一个无锁写队列满足这两个约束。

## 5. 协议约定

### 5.1 RPC 请求

- client 为每个 RPC 分配非零 `seq_num`。
- 第一条请求使用 1，之后单调递增。
- 发生无符号整数回绕时跳过 0，并跳过仍存在于 pending map 中的编号。
- `seq_num` 在跨端序模式下继续使用现有 `htonll/ntohll` 转换。

### 5.2 RPC 响应

- server 必须把请求头中的 `seq_num` 原样写入对应响应头。
- 普通同步响应和 `rpc_context` 延迟响应遵循相同规则。
- `rpc_context` 创建时捕获当前请求的 `seq_num`，之后即使响应发生在线程切换或延迟之后，也使用捕获的编号。

### 5.3 发布订阅

- `seq_num == 0` 保留给不按 RPC 请求关联的消息。
- 发布订阅继续使用 `msg_type == 1` 和 `function_id == topic_id` 分发。
- 发布订阅响应不进入 RPC pending map。

### 5.4 旧版本兼容

旧 server 会把响应的 `seq_num` 保持为 0。简单地在“只有一个 pending”时接受 seq 0 并不完全安全：如果旧请求已经超时，旧 server 的迟到响应可能被错误地交给后续唯一的 pending 请求。

因此每条连接需要维护 peer capability 状态：

```cpp
enum class peer_mode {
  unknown,
  multiplex,
  legacy,
};
```

处理规则如下：

- 收到合法的非零 seq 响应后，连接进入 `multiplex`，之后严格按照 seq 分发。
- `unknown` 状态收到 seq 0，且恰好只有一个 pending RPC时，可以交给该请求，并进入 `legacy`。
- `unknown` 或 `legacy` 状态收到 seq 0，但存在多个 pending RPC时，响应归属不明确，按协议错误关闭连接。
- `legacy` 状态最多允许一个在途 RPC。
- `legacy` 状态发生请求超时或取消后关闭连接，防止该请求的迟到 seq 0 响应污染下一次调用。
- `unknown` 状态在确认 peer 支持非零 seq 前发生超时后，将该连接标记为不再接受 seq 0；后续收到 seq 0 时按协议错误关闭。这样既不会把旧 server 的迟到响应交给新请求，也允许尚未完成能力确认的新 server 继续用非零 seq 证明其支持 multiplex。

新 client 与旧 server 之间只有严格的单请求顺序调用可以继续工作；连接复用要求 client 和 server 同时升级。后续可以通过启用 `version` 字段或握手能力位显式协商 multiplex，从而移除这种运行时推断。

### 5.5 响应 seq 的严格性

在 `multiplex` 模式下，非零响应 seq 分为三类：

- seq 存在于 pending map：正常完成该请求。
- seq 存在于 `abandoned_seqs`：这是已超时或取消请求的迟到响应，移除记录并丢弃响应。
- seq 两处都不存在：可能是重复响应、从未发出的 seq 或 peer 状态损坏，按协议错误关闭连接。

不能把所有未知 seq 都静默丢弃，否则重复响应和错误 server 实现会被掩盖。`abandoned_seqs` 必须有容量上限；超过上限说明服务端持续积压或不返回请求，最安全的处理是关闭连接并清空该连接状态。

seq 即将从 `UINT64_MAX` 回绕时应主动重连并从 1 重新开始，而不是在同一连接内复用旧编号。虽然实际很难发生，但这样可以从协议上排除极晚响应与新请求发生 seq 碰撞。

### 5.6 协议字段校验

读循环在分配 body 前必须完成以下校验：

- `magic == REST_MAGIC_NUM`。
- `version` 是本端支持的版本；兼容期可以接受 0 和新的 multiplex 版本。
- `serialize_type` 是支持的序列化类型。
- `msg_type` 是普通 RPC 响应或已定义的发布订阅类型，未知类型不能按普通 RPC 处理。
- RPC 响应的 `body_len >= 1`，因为 body 至少包含一个 `rpc_errc` 字节。
- `body_len <= max_body_size`，并且从 `uint64_t` 转换为 `size_t` 前检查溢出。
- 当前实现不支持附件，因此要求 `attach_length == 0`。如果允许非零附件，就必须完整读取或跳过附件，否则下一帧会错位。
- 发布订阅控制帧的 body 长度必须符合该消息类型的定义；不能在不消费 body 的情况下直接读取下一个 header。

任何会破坏 TCP 字节流边界或表明双方协议不一致的校验失败，均按连接级 `protocol_error` 处理。

## 6. Client 设计

### 6.1 连接内状态

每个 client socket 维护以下状态：

```cpp
uint64_t next_seq_num;
unordered_map<uint64_t, pending_request> pending_requests;
deque<outgoing_frame> write_queue;
bool write_in_progress;
unordered_map<uint32_t, subscription_state> subscriptions;
```

`pending_request` 保存一个与等待协程关联的完成操作。网络读循环只负责产生未类型化的响应：

```cpp
struct raw_response {
  rpc_errc transport_error;
  rest_rpc_header header;
  std::string body;
};
```

每个 pending 请求还保存独立的 `steady_timer` 和请求状态：

```cpp
enum class request_state {
  queued,   // 请求帧还在写队列中
  writing,  // 正在写 socket
  waiting,  // 已经写出，等待响应
};

struct pending_request {
  request_state state;
  steady_timer timer;
  completion_handler completion;
};
```

这些字段仅由 socket 所属 `io_context` 线程访问，因此状态转换不需要 atomic 或 mutex。

具体返回类型的反序列化仍在发起 `call<R>()` 的协程中进行。这样读循环不需要保存或判断 RPC 的返回类型。

### 6.2 调用流程

一次 `call<R>()` 的流程如下：

1. 在 socket 所属 executor 上分配新的非零 seq。
2. 完成参数序列化，并生成拥有独立内存的完整请求帧。
3. 在发送前把等待操作注册到 `pending_requests[seq]`。
4. 把请求帧加入连接写队列。
5. 当前协程挂起，等待响应、超时或连接错误。
6. 读循环收到对应 seq 后，从 map 移除 pending 项并恢复该协程。
7. 恢复后的协程检查错误码，并按照 `R` 反序列化响应体。

必须先注册 pending 项再发送，避免响应到达时 map 中还没有对应等待者。

计时从 `call_for()` 开始执行时启动，包含写队列等待、网络写入和等待服务端响应的全部时间。`call()` 继续使用现有的默认超时时间。

### 6.3 单一读循环

连接成功后启动一个读循环，并且该连接生命周期内不再由 `call()` 直接读取 socket：

```text
读取固定长度 header
  -> 校验 magic 并处理端序
  -> 读取 body
  -> msg_type == pubsub：按 topic_id 分发
  -> seq_num != 0：按 seq_num 分发
  -> seq_num == 0：执行旧版本降级规则
  -> 未找到 seq：作为已经超时或取消请求的迟到响应丢弃
```

header 和 body 使用读循环当前迭代的局部对象。响应被移动给对应 pending 请求，不能复用一个连接级 body buffer，否则后续读取可能覆盖尚未反序列化的数据。

### 6.4 写队列

每个待发送项持有完整帧的所有权，避免异步写期间引用调用栈上的 header 或序列化临时对象。

RPC 写队列项同时记录对应的 seq。write pump 准备发送队首前检查该 seq 是否仍存在于 pending map；如果请求已经在队列等待期间超时或取消，则直接丢弃该帧，不再把已经失效的请求发送给 server。

写入流程：

1. 调用方把帧追加到 `write_queue`。
2. 如果当前没有写操作，启动 write pump。
3. write pump 对队首执行一次 `asio::async_write`。
4. 写完后移除队首，并继续写下一帧。
5. 队列为空时退出，将 `write_in_progress` 设为 false。

整个流程只在所属 `io_context` 线程运行，因此不需要锁。

### 6.5 Executor 与 API 调用约束

pending map 无锁的前提必须由实现保证，而不能只依赖调用方碰巧正确：

- `connect/call/call_for/subscribe` 的连接状态修改必须发生在 `client.get_executor()` 上。
- 如果 API 允许从其他线程或 executor 调用，入口应先把内部操作调度到 client executor；否则必须在公开文档中明确禁止，并在 Debug 构建中断言。
- `close()` 和析构可能从其他线程调用，应只投递关闭动作，不直接访问连接容器。
- 完成 pending 请求前先把它从 map 移出，再恢复用户协程，防止协程恢复后立即重入 client 并修改同一容器。

不支持两个并发的 `connect()`。连接中的 client 再次调用 `connect()` 视为显式重连：先以 `socket_closed` 完成旧连接的全部 pending 请求和订阅，再建立新连接。不能把旧连接的 pending 请求带到新连接。

### 6.6 Client 对象生命周期

读循环、写队列项、timer 回调和 pending 完成操作不能捕获裸 `rpc_client*`。它们应持有独立的共享连接状态；client 析构时执行一次幂等关闭：

1. 把连接状态标记为 closing。
2. 取消读写和所有 timer。
3. 从容器移出 pending 项。
4. 以 `socket_closed` 恢复所有等待协程。
5. 最后释放 socket 和共享状态。

旧连接异步操作的完成回调必须携带普通的 connection generation。generation 与当前连接不一致时只释放旧操作自身，不能关闭或修改已经重连成功的新 socket。

### 6.7 本地异常安全

- 参数序列化、内存分配或完整帧构造失败发生在 pending 注册前时，异常直接返回给调用方，连接不受影响。
- 如果异常发生在 pending 注册后，使用作用域清理保证 map、timer 和写队列引用不会残留。
- 返回值反序列化发生在 pending 已移出 map 之后；反序列化异常只影响当前调用，不能终止读循环或关闭连接。
- server 返回非 `ok` 的 `rpc_errc` 时，client 不按正常返回类型 `R` 反序列化错误正文。
- 完成处理器应通过 executor 调度，读循环本身只做帧解析和分发，避免用户协程中的耗时反序列化或异常重入读循环。

## 7. Server 设计

server 读取并解析请求头后，需要在请求上下文中保留 seq：

```text
request header.seq_num
  -> router/当前请求上下文
  -> 普通 response 或 rpc_context 延迟 response
  -> response header.seq_num
```

普通响应直接把当前请求 header 的 seq 传给 `rpc_connection::response()`。

`rpc_context` 构造时同时捕获：

- 当前 `rpc_connection`；
- 当前请求的 `seq_num`。

之后调用 `rpc_context::response()` 时使用捕获的 seq，而不是读取可能已经属于另一个请求的 thread-local 当前值。

server 连接同样使用单线程无锁写队列。原因不是多线程竞争，而是普通响应、延迟响应和 publish 可能在不同协程中重叠发起 `async_write`。

### 7.1 Handler 与延迟响应异常

- 请求反序列化失败、handler 抛出异常、普通响应序列化失败时，server 使用同一个 seq 返回对应 RPC 错误，不关闭连接。
- `rpc_context` 延迟响应在序列化阶段抛出异常时，也应尽可能用捕获的 seq 返回错误；如果错误响应本身无法构造，则 client 最终按请求超时处理。
- client 已断开后调用延迟响应，server 写操作返回连接错误；不能因为持有 `rpc_context` 而访问已经销毁的连接对象。
- server 写响应前如果连接已经关闭，应立即返回 `socket_closed`，不能继续向写队列追加数据。

`rpc_context` 的“一次响应”状态必须在它的所有副本之间共享，或者把 `rpc_context` 设计为只能移动。否则复制同一个 context 后，每个副本都有独立的 `has_response_`，可能对同一个 seq 返回多次响应。第二次响应必须返回 `has_response`，不能写入 socket。

### 7.2 请求上下文边界

当前 `rpc_context` 通过 thread-local 数据获得连接。一个 `io_context` 虽然只有一个线程，但同一线程上可以交错运行多个连接协程，因此 thread-local 并不等同于 coroutine-local。

在不改 API 的前提下，至少必须保证：

- router 调用 handler 前设置当前连接和 seq。
- `rpc_context` 必须在 handler 第一次挂起前构造，并立即复制连接和 seq，之后不再读取 thread-local 值。
- 每次 route 开始和结束都重置 `delay`、连接和 seq；异常路径也通过 RAII request scope 重置，不能把前一个请求的 delay 状态泄漏给下一个请求。

如果要允许 handler 在一次 `co_await` 之后才创建 `rpc_context`，现有 thread-local 方案无法保证取到正确请求，后续需要把 context 显式作为 handler 参数或实现真正的 coroutine-local 请求上下文。这是现有延迟响应 API 的限制，必须写入公开文档并增加误用测试。

### 7.3 Server 背压

慢 client 可能使 server 响应写队列持续增长。每条连接应限制：

- 写队列帧数；
- 写队列总字节数；
- 延迟响应的最大未完成数量。

超过上限时不能无限分配内存。由于响应已经产生但无法安全丢弃单个 TCP 帧，推荐关闭该慢连接，并让尚未完成的 server 写操作返回连接错误。

## 8. 超时、取消与连接错误

### 8.1 单请求超时

每个 pending 请求拥有独立的 `steady_timer`。timer 到期时在连接所属 `io_context` 上执行以下操作：

1. 在 `pending_requests` 中查找该 seq。
2. 如果已经不存在，说明响应、连接错误或取消先完成，timer 不再执行任何动作。
3. 如果仍然存在，则先从 map 移除，保证该请求只能完成一次。
4. 恢复对应协程并返回 `rpc_errc::request_timeout`。
5. 不关闭 TCP 连接，也不影响其他在途请求。

根据请求当前所处阶段，超时后的处理不同：

- `queued`：写队列稍后发现 seq 已不在 pending map，跳过该请求帧。
- `writing`：不取消 socket 上的写操作，避免影响同连接其他请求；写完后的响应将被视为迟到响应。
- `waiting`：请求已被 server 接收，client 只能停止等待，不能撤回服务端工作。

迟到响应到达时，读循环找不到对应 seq，直接丢弃并继续读取后续响应。它不能被交给下一个请求。

超时与响应可能在非常接近的时间到达，但两者都在同一个 `io_context` 线程执行。以谁先从 `pending_requests` 成功移除该 seq 为准：

```text
响应先执行：移除 pending -> cancel timer -> 返回响应
超时先执行：移除 pending -> 返回 request_timeout -> 迟到响应丢弃
```

因此不需要锁，也不会发生同一个 `co_await` 被恢复两次。

这是连接复用下比“一个请求超时就关闭整条连接”更合适的语义。

### 8.2 协程取消

取消某个调用时取消该请求的 timer，并删除对应 pending 项，但不关闭连接。尚未发送的帧由 write pump 跳过；已经写出的请求无法撤回，其迟到响应按照未知 seq 丢弃。

### 8.3 写错误、读错误和协议错误

这些错误表示连接本身不可继续使用：

- 关闭 socket；
- 停止读写循环；
- 清空写队列；
- 用相应错误完成所有 pending RPC；
- 唤醒所有等待中的订阅操作。

完成所有 pending 请求时，必须先把它们从 map 移出并取消各自的 timer，再调用完成处理器，避免完成处理器恢复协程后重入连接状态。

### 8.4 服务端长时间不返回

只要 client socket 仍然可读写，服务端长时间不返回不会被 TCP 自身识别为断线。它按照单请求超时处理：

- 到达 `call_for()` 的期限后，该 seq 返回 `rpc_errc::request_timeout`。
- 同连接上的其他 seq 继续正常收发。
- server 后续返回的迟到响应被丢弃。
- server 上已经开始的 handler 不会自动停止。

当前协议没有“取消远端请求”消息，因此 client 超时只能停止本地等待，不能保证取消 server 的计算。如果未来需要释放服务端长任务，应单独设计包含目标 seq 的 cancel 消息，并由业务 handler 配合取消。

为了避免调用方持续创建超长超时请求导致 pending map 无限制增长，实现应提供或预留最大在途请求数。达到上限时应在发送前拒绝新请求；该能力可作为独立配置加入，不影响本次 seq 分发模型。

### 8.5 半包、慢包与写阻塞

请求 timer 只能结束某个 seq，不能解决 TCP 流已经卡在半帧的问题。

例如 client 已经读到一个响应 header，但 server 声明的 body 只发送了一部分后停止发送。此时单读循环无法跳过该 body 去读取后续响应，整条连接已经发生队头阻塞。因此需要连接级帧组装超时：

- 尚未收到任何新帧字节时可以保持空闲读，不因为没有 publish 或 RPC 响应而关闭连接。
- 一旦开始接收某个 header 或已经得到 header 并开始读取 body，就启动可配置的 `frame_read_timeout`。
- 在期限内未完成整帧，关闭连接并以 `read_error` 或 `protocol_error` 完成全部 pending 请求。
- 使用 `async_read_some` 加缓冲解析器可以区分“完全空闲”和“半帧卡住”；只对整个 `async_read(header)` 设置超时会误伤正常空闲连接。

写方向也存在相同问题：peer 不读取时，当前 `async_write` 可能长时间不完成，并阻塞其后的所有请求。write pump 需要可配置的 `frame_write_timeout`：

- queued 请求可以在自身超时后直接跳过。
- 已经开始写的帧不能只取消一半后继续使用连接，否则 peer 会收到残缺帧。
- 当前帧写超时或发生部分写错误时，必须关闭整条连接并结束所有 pending 请求。

TCP 多路复用只能解决响应乱序关联，不能消除单条字节流的半帧队头阻塞。

### 8.6 连接半开与连续超时

peer 进程失联但 TCP 尚未报告错误时，每个请求会分别超时。连接在 `multiplex` 模式下可以暂时保留，但需要防止它永久处于假健康状态：

- 记录连续请求超时数、最后一次成功响应时间和 `abandoned_seqs` 数量。
- 达到可配置阈值后主动关闭连接，让上层决定是否重连。
- TCP keepalive 或应用层 ping 可以辅助发现半开连接，但不能替代每请求 deadline。
- 库本身不自动重试已经发出的 RPC，避免非幂等请求被重复执行。

### 8.7 本地调用状态

- 未连接、已经关闭或正在关闭时调用 RPC：立即返回 `socket_closed`，不分配 seq，不入队。
- duration 小于等于 0：立即返回 `request_timeout`，不序列化、不发送请求。
- 正在连接时调用 RPC：默认立即返回 `socket_closed`；如果以后支持等待连接完成，应做成明确配置，不能隐式无限等待。
- 达到最大在途请求数或最大写队列字节数：发送前拒绝，不能先入队再等待内存释放。
- 显式重连：旧连接所有 pending 请求返回 `socket_closed`，新连接不自动重试它们。

现有 `rpc_errc` 没有表达过载、消息过大和本地取消的专用值。实现前应决定是新增并追加 `too_many_requests`、`message_too_large`、`request_cancelled`，还是通过独立的本地错误类型返回；不要把这些情况错误地伪装成 `request_timeout`。

### 8.8 服务端 RPC 错误

以下错误属于某一个请求，响应必须携带原请求 seq，且不能关闭健康连接：

- `no_such_function`；
- 请求参数反序列化失败；
- handler 抛出标准或未知异常；
- handler 返回值序列化失败；
- 业务主动返回的错误。

client 收到非 `ok` 错误后只返回错误码或错误正文，不再把正文反序列化为正常返回类型。未知的远端错误码保留为该请求的远端错误，以便新旧版本向前兼容；只要帧结构合法，就不应因此关闭连接。

### 8.9 发布订阅异常

订阅等待与 RPC deadline 语义不同：长时间没有 publish 是正常状态，不能自动视为请求超时。需要处理：

- 连接关闭时唤醒所有订阅等待者。
- 未知 topic 的 publish 按协议策略记录并丢弃，不能交给任意 RPC。
- 慢订阅者需要有消息数和总字节数上限，并明确采用丢弃旧消息、丢弃新消息或关闭连接的策略。
- 同一 topic 多个并发 `subscribe()` 等待者必须定义是一条消息唤醒一个等待者，还是广播给全部等待者。
- 当前 server 每条连接只保存一个 `topic_id`，本次不能暗示已经支持多 topic；若保持现状，应明确限制一条连接只能订阅一个 topic。

### 8.10 异常处理矩阵

| 场景 | 影响范围 | 当前请求结果 | 是否关闭连接 | 其他 pending 请求 |
| --- | --- | --- | --- | --- |
| 单请求等待超时 | 单个 seq | `request_timeout` | 否 | 继续等待 |
| 单个调用被取消 | 单个 seq | 取消 | 否 | 继续等待 |
| 已记录在 abandoned 集合中的迟到响应 | 单个响应 | 丢弃 | 否 | 不受影响 |
| 从未发出或重复的非零 seq | 整条连接 | `protocol_error` | 是 | 全部结束 |
| socket 写错误 | 整条连接 | `write_error` | 是 | 全部结束 |
| socket 读错误或 EOF | 整条连接 | `read_error` | 是 | 全部结束 |
| 本地主动 close 或重连 | 整条连接 | `socket_closed` | 是 | 全部结束 |
| header/body 半帧超时 | 整条连接 | `read_error` | 是 | 全部结束 |
| 当前帧写入超时 | 整条连接 | `write_error` | 是 | 全部结束 |
| magic、版本、类型、长度、附件错误 | 整条连接 | `protocol_error` | 是 | 全部结束 |
| `seq=0` 且有多个 pending RPC | 整条连接 | `protocol_error` | 是 | 全部结束 |
| 返回值反序列化失败 | 单个 seq | 保持现有异常语义 | 否 | 不受影响 |
| server 返回业务/RPC 错误 | 单个 seq | 原错误码 | 否 | 不受影响 |
| 未连接时调用 | 单个调用 | `socket_closed` | 已关闭 | 不受影响 |
| 本地序列化或分配失败 | 单个调用 | 抛出异常 | 否 | 不受影响 |
| pending/写队列超过上限 | 单个调用 | 过载错误 | 否 | 不受影响 |
| 响应 body 超过上限或长度溢出 | 整条连接 | `protocol_error` | 是 | 全部结束 |

### 8.11 完成优先级与 exactly-once

响应、请求 timer、取消和连接关闭都可能尝试结束同一个 pending 请求。统一使用“先从 map 提取，提取成功者负责完成”的规则：

```cpp
auto node = pending_requests.extract(seq);
if (node.empty()) {
  return; // 已由另一条路径完成
}
node.mapped().timer.cancel();
post_completion(std::move(node.mapped()), result);
```

同一 executor 保证操作串行；map 的所有权转移保证完成处理器只调用一次。边界时刻响应和 timeout 谁先进入 executor 队列，用户就观察到谁的结果，这是允许的竞态语义。

### 8.12 重连

重连前必须先使旧连接上的所有 pending 操作失败，并取消旧读写操作。新连接启动新的读循环；旧连接的任何完成回调都不能操作新连接状态。

由于连接生命周期操作也运行在同一 executor 上，可以使用普通递增的 connection generation 区分旧、新连接，不需要 atomic。

连接本身建议使用显式状态机：

```text
disconnected -> resolving -> connecting -> connected -> closing -> disconnected
```

- DNS 超时返回 `resolve_timeout`；DNS 系统错误原样返回。
- resolver 返回空 endpoint 列表时返回连接错误，不能直接解引用 `begin()`。
- TCP connect 超时返回 `connection_timeout`；拒绝连接等系统错误原样返回。
- `TCP_NODELAY` 等连接选项设置失败时关闭新 socket 并返回该系统错误，不能留下“已连接但初始化未完成”的状态。
- 只有完成全部连接初始化后才启动读循环并发布 `connected` 状态。
- 连接失败后必须回到 `disconnected`，允许下一次 `connect()` 使用全新的 socket。

### 8.13 Server 主动关闭与空闲超时

server 停止、连接空闲回收或管理员主动关闭连接时，client 的读循环最终得到 EOF/连接重置，并以连接级错误结束所有 pending 请求。

如果 server 在 handler 尚未完成时触发连接最大空闲时间：

- client 上所有 pending 请求结束；
- handler 后续返回的普通或延迟响应写入失败；
- server 必须保证 write completion 和 `rpc_context` 只看到关闭状态，不访问已释放对象。

是否把“正在执行 handler”算作连接活跃需要由 server 空闲策略明确规定。若目标是只清理真正空闲连接，应把 active request 数纳入判断；仅根据最后一次网络读写时间可能误杀长时间运行的 handler。

### 8.14 资源上限

为避免正常接口被慢 peer 或恶意输入拖垮，至少需要以下可配置限制：

| 限制 | Client | Server | 超限策略 |
| --- | --- | --- | --- |
| 最大请求/响应 body | 是 | 是 | 接收超限关闭连接；本地发送超限直接拒绝 |
| 每连接最大 pending RPC 数 | 是 | 可选 | 发送前返回过载错误 |
| 每连接写队列总字节数 | 是 | 是 | client 拒绝新请求；server 关闭慢连接 |
| 最大 abandoned seq 数 | 是 | 否 | 关闭连接，防止迟到请求状态无限增长 |
| 订阅缓存消息数/字节数 | 是 | 是 | 使用明确的丢弃或断开策略 |
| 单帧组装时间 | 是 | 是 | 关闭半包连接 |
| 单帧写入时间 | 是 | 是 | 关闭阻塞连接 |

限制值应在入队或分配前检查。对来自网络的 `uint64_t` 长度先做上限和 `size_t` 溢出检查，再调用 `resize/reserve`，避免异常分配或进程内存耗尽。

## 9. 关键时序

```text
client call(1) -- seq=1 --\
client call(2) -- seq=2 ----> 单写队列 ---> server
client call(3) -- seq=3 --/

server response(3, seq=3) ---> client 单读循环 ---> pending[3] ---> call(3) 恢复
server response(2, seq=2) ---> client 单读循环 ---> pending[2] ---> call(2) 恢复
server response(1, seq=1) ---> client 单读循环 ---> pending[1] ---> call(1) 恢复
```

恢复顺序可以是 `3、2、1`，但每个调用拿到的值仍分别是 `1、2、3`。

## 10. 测试方案

### 10.1 必须新增的协议级测试

使用现有 `rpc_server`、`rpc_context` 和连接所属 executor 上的
`steady_timer` 构造延迟响应：

1. 测试主体写在一个 `asio::awaitable<void>` 协程函数中。
2. 使用 `co_await (call(1) && call(2) && call(3))` 同时启动三个调用。
3. handler 在创建 `rpc_context` 后立即返回，并分别延迟 300、200、100ms
   响应，使响应顺序固定为 3、2、1。
4. 验证三个调用分别得到自己的值，而不是按响应到达顺序错配。
5. `TEST_CASE` 只负责在 client executor 上 `sync_wait()` 测试协程。

测试不额外创建 `io_context`、阻塞 socket 或测试线程，避免测试本身引入与
rest_rpc 实际运行模型不同的并发方式。

### 10.2 Server seq 回传测试

- 普通 handler 响应回传请求 seq。
- `rpc_context` 延迟响应回传创建 context 时捕获的 seq。
- 跨端序模式下 seq 转换正确。

### 10.3 生命周期测试

- 一个请求超时，其他在途请求仍能完成。
- 请求在写队列中超时时，该请求帧不会继续发出。
- 请求已经发出后超时时，不取消连接上的其他读写操作。
- 超时请求的迟到响应不会交给后续请求。
- 响应和 timer 同时就绪时，请求只完成一次。
- `unknown` 模式超时后关闭连接，不把旧 server 的 seq 0 迟到响应交给新请求。
- `legacy` 模式强制单请求在途，并在超时后关闭连接。
- 合法迟到 seq 被丢弃；重复响应或从未分配的 seq 触发协议错误。
- socket 读错误会结束所有 pending 请求。
- socket 写错误会结束所有 pending 请求。
- partial header、partial body 和 stalled write 超时后关闭连接。
- 重连后 seq 分发不受旧读循环影响。
- client 销毁时没有悬空回调或未恢复协程。
- close、连接错误和 timeout 同时发生时，每个 pending completion 只调用一次。

### 10.4 协议异常测试

- `body_len == 0` 的 RPC 响应。
- `body_len` 超过配置上限。
- `body_len` 从 `uint64_t` 转换为 `size_t` 会溢出。
- `attach_length != 0`。
- magic、version、serialize type 或 message type 非法。
- publish/control header 声明非零 body 时，读循环不会发生下一帧错位。
- header 完整但 body 不完整，连接级 frame timer 生效。
- 非 `ok` RPC 错误正文不会按正常返回类型反序列化。
- 未知的远端 RPC 错误码只影响对应请求，不破坏帧流。

### 10.5 本地异常与资源测试

- 未连接、正在连接、closing 和已关闭状态调用 RPC。
- duration 为 0 或负值时不发送请求。
- 参数序列化抛异常后 pending map 和写队列为空。
- 返回值反序列化抛异常后读循环仍可完成下一个响应。
- pending 数、写队列字节数、abandoned seq 数达到上限。
- seq 接近回绕边界时触发重连，不复用旧 seq。
- `char`、`signed char`、`unsigned char` 单参数 codec 覆盖 0、边界值和负值；防止以后把 `std::to_string` 改为 `std::to_chars` 时引入编译或语义回归。

### 10.6 Server 与延迟响应测试

- handler 参数反序列化、执行和返回值序列化分别失败，响应均携带原 seq。
- 延迟响应在 client 已断开后执行，只返回连接错误且不崩溃。
- 同一 `rpc_context` 被复制或重复调用时只允许一次响应。
- handler 创建 `rpc_context` 后挂起，再由其他连接运行 handler，捕获的 connection 和 seq 不变化。
- route 抛异常后 thread-local request scope 被清理，下一请求不继承 delay/seq。
- server 空闲回收与长 handler 并发发生时，连接及 context 生命周期安全。
- 慢 client 造成 server 写队列达到上限时关闭该连接，不无限增长内存。

### 10.7 发布订阅测试

- 长时间没有 publish 时订阅保持等待，不按 RPC timeout 失败。
- RPC 响应与 publish 消息交错到达时分别进入 seq map 和 topic queue。
- 未知 topic、慢订阅者缓存超限以及连接关闭时的行为符合配置。
- 当前单 topic 限制有明确测试；如果后续支持多 topic，再替换该限制测试。

### 10.8 回归测试

- 单请求 `call()` 和 `call_for()`。
- void 及非 void 返回值。
- 发布订阅。
- 延迟响应。
- 跨端序。
- server 停止和 client 重连。

## 11. 验收标准

- 同一连接三个请求的 seq 为非零且互不相同。
- 响应按 3、2、1 到达时，每个 `co_await` 与自己的 seq 正确匹配。
- 任意时刻每个 socket 只有一个读操作和一个写操作。
- 连接级状态没有 mutex，且仅由所属 `io_context` 线程访问。
- 单请求超时不会中断其他在途请求。
- 半帧或卡住的写操作不会永久毒化连接。
- pending 请求在响应、超时、取消和关闭竞态下只完成一次。
- 网络长度字段在分配内存前经过上限与溢出检查。
- 旧 server 的 seq 0 迟到响应不会被错误匹配给新请求。
- 所有异步操作只持有共享连接状态，不依赖已经析构的 client/server 栈对象。
- 现有测试和新增复用测试全部通过。

## 12. 实现前需要确定的配置与 API

以下项目不应留到编码过程中临时决定：

- 默认 `max_body_size`。
- 默认最大 pending RPC 数和写队列字节数。
- 默认 `frame_read_timeout` 与 `frame_write_timeout`。
- abandoned seq 数达到上限时是否立即关闭连接。
- 订阅缓存超限采用丢弃旧消息、丢弃新消息还是断开连接。
- 是否新增 `too_many_requests`、`message_too_large`、`request_cancelled` 错误码。
- 是否用 `version` 字段明确标识 multiplex peer，还是暂时采用 `unknown/multiplex/legacy` 推断。
- 是否在本次范围内修复 `rpc_context` 的 thread-local 限制；若不修复，必须明确“第一次挂起前构造”的使用约束。

这些配置确定后，异常路径才可以写成稳定测试，而不是依赖实现细节。
