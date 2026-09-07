# rest_rpc protocol v2 多路复用实现方案

## 1. 结论

多路复用作为显式的 `protocol_v2` 能力实现，不修改 v1 的调用语义，也不通过 `seq_num == 0` 猜测对端能力。

- 现有 `call()`、`call_for()`、`subscribe()` 固定走 v1。
- 新增 `send_call()`、`send_call_for()`、`send_calls()`，固定走 v2。
- v1 的请求处理、同步读写和错误行为尽量保持原代码不动。
- v2 使用非零 `seq_num`、client 单读循环、client/server 单写队列和 pending map。
- server 的 v2 handler 独立执行，慢 handler 不阻塞连接继续读取后续请求。
- 一条 TCP 连接只能选择一种协议模式；首次请求确定 v1 或 v2，之后混用立即失败。重连后可重新选择。

该边界比运行时兼容猜测更清楚：旧 client 和旧 server 继续使用 v1；只有新 client 的新接口与支持 v2 的 server 才启用多路复用。

## 2. 目标与非目标

### 2.1 目标

- 同一条 v2 TCP 连接允许多个 RPC 同时在途。
- 请求和响应严格按 `seq_num` 关联，允许响应乱序。
- 调用方可以先启动多个请求，再单独 `co_await` 任意一个结果，不强制 collect-all。
- v2 server 读完一帧后立即继续读下一帧，不等待当前 handler 完成。
- 超时、取消、断连、写失败和重连都只完成每个请求一次。
- pending、请求写队列、服务端并发请求和响应队列都有上限。
- v2 的实现、状态和测试与 v1 隔离。

### 2.2 非目标

- 不在 v1 连接上提供多路复用。
- 不自动把 v1 调用升级为 v2。
- 不自动重试失败的 RPC；是否可重试取决于业务幂等性。
- 本次不设计取消远端 handler 的协议。取消只停止 client 本地等待；已经到达 server 的任务可以继续执行。
- v2 首版不与 v1 的发布订阅混用；需要 v2 发布订阅时单独定义协议和接口。

## 3. v1 与 v2 隔离

### 3.0 硬性代码边界

v2 是一组新接口及其独立实现，不是给现有 v1 `call()` 打开的一个内部开关。实现中禁止以下做法：

- 禁止把 `call()`、`call_for()` 改为 pending map + 单 read loop。
- 禁止在 `connect()` 成功后无条件启动 v2 read loop。
- 禁止让 v1 与 v2 共用 pending map、请求写队列、响应分发状态或超时状态。
- 禁止在 v1 主流程内部散布 `if (version == v2)` 后复用同一套读写状态机。
- 禁止用 `peer_mode`、seq 0 fallback 或响应特征把 v1 隐式升级成 v2。

公开接口和实现归属固定如下：

| 协议 | 公开接口 | 内部实现 |
|---|---|---|
| v1 | `connect/call/call_for/subscribe` | 现有 v1 代码路径 |
| v2 | `send_call/send_call_for/send_calls` | 新增 `multiplex_client_session` |

`rpc_client` 只负责保存连接和在接口入口选择 session；v1 函数除一次模式占用检查外，网络读写函数体应恢复为 `master` 的实现。v2 所需的 seq、pending、reader、writer、deadline 和队列限制全部位于新增组件中。

server 侧只允许在收到首个 header 后做一次协议入口分流；v2 委托给独立 session，v1 则继续进入现有循环：

```cpp
if (header.version == REST_RPC_PROTOCOL_V2) {
  co_return co_await start_multiplex(std::move(header));
}
// protocol_v1: continue the existing v1 loop
```

现有 v1 循环保留原来的串行 handler 和直接响应路径；v2 的完整帧复制、并发 handler 与 response queue 只存在于 `multiplex_server_session`。后续帧版本与连接首次选定的版本不一致时直接视为协议错误，不跨路径处理。

### 3.1 协议号

```cpp
inline constexpr uint8_t REST_RPC_PROTOCOL_V1 = 0;
inline constexpr uint8_t REST_RPC_PROTOCOL_V2 = 1;
inline constexpr uint8_t REST_RPC_SERIALIZE_TYPE = 0;
```

- v1 请求保持 `version == 0`、`seq_num == 0`。
- v2 RPC 请求必须是 `version == 1`、`seq_num != 0`。
- v2 响应必须原样带回请求的 `version` 和 `seq_num`。
- v2 client 收到 v1 响应、零 seq 响应或错误版本时按 `protocol_error` 关闭连接。
- 不保留 `peer_mode`、`legacy_seq_zero_safe` 或“仅有一个 pending 时接受 seq 0”等推断逻辑。

### 3.2 连接模式

```cpp
enum class connection_mode {
  unset,
  v1,
  v2,
};
```

- `connect()` 成功后模式为 `unset`。
- 第一次调用 `call/call_for/subscribe` 时切换为 `v1`。
- 第一次调用 `send_call/send_call_for/send_calls` 时切换为 `v2` 并启动 v2 read loop。
- 模式确定后，另一组接口返回 `protocol_mode_conflict`，不读写 socket。
- 显式重连创建全新的连接状态并把模式恢复为 `unset`。

模式互斥可保证 v1 的“调用协程自己读取响应”和 v2 的“唯一 read loop 读取所有响应”绝不会同时操作同一个 socket。

### 3.3 代码隔离

v2 代码放进独立内部组件，公开类只增加薄转发接口：

```text
rpc_client
  ├─ v1 path: 现有 connect/call/call_for/subscribe
  └─ multiplex_client_session: 仅由 send_call/send_call_for/send_calls 使用

rpc_connection
  ├─ start_v1(): 现有 server 处理路径
  └─ multiplex_server_session: v2 read loop、并发 handler、response queue
```

实现时先从 `master` 恢复 v1 的 client/server 主路径，再以新函数或新头文件添加 v2。与协议无关的修复（例如 `rpc_context` 只响应一次的原子状态）可以保留，但必须有独立回归测试。

## 4. Client 公开接口

### 4.1 两阶段调用

```cpp
template <typename R>
class async_result; // 内部持有 asio::awaitable<call_result<R>>

template <auto func, typename... Args>
asio::awaitable<async_result<
    return_type_t<function_return_type_t<decltype(func)>>>>
send_call(Args&&... args);

template <auto func, typename Rep, typename Period, typename... Args>
asio::awaitable<async_result<
    return_type_t<function_return_type_t<decltype(func)>>>>
send_call_for(std::chrono::duration<Rep, Period> timeout, Args&&... args);
```

第一层 `co_await` 只负责启动请求：完成参数序列化、分配 seq、登记 pending、加入写队列，然后马上返回第二层 awaitable；它不等待网络响应。

第二层 `co_await` 等待该 seq 的最终结果。响应可以在第二层开始等待之前到达，结果必须缓存在 request state 中，不能丢失。

```cpp
auto slow = co_await client.send_call<slow_rpc>(1);
auto fast = co_await client.send_call<fast_rpc>(2);

// 两个请求都已启动，可以先等任意一个。
auto fast_result = co_await fast.wait();
auto slow_result = co_await slow.wait();
```

`async_result<R>` 封装 Asio 1.36 的 move-only `asio::awaitable`，通过 `wait()` 在内部完成所有权转移，因此调用方不需要显式 `std::move`。结果仍是 single-consumer，第二次 `wait()` 会抛出 `std::logic_error`。Asio 的 `awaitable` promise 只接受其原生 awaitable/异步操作，无法在不修改 Asio 的情况下让普通包装器直接支持命名左值 `co_await sender`，所以使用明确的 `co_await sender.wait()`。

这才是 v2 的主要用法。`awaitable_operators::operator&&` 或 collect-all 只能作为“确实需要等全部结果”时的组合工具，不能成为启动并发请求的必要条件。

### 4.2 `send_calls()`

`send_calls()` 是批量启动的便利接口，支持不同函数、参数和返回类型：

```cpp
auto [slow, fast] = client.send_calls(
    client.send_call<slow_rpc>(1),
    client.send_call<fast_rpc>(2));

auto fast_result = co_await fast.wait();
auto slow_result = co_await slow.wait();
```

它是立即启动函数，而不是 collect-all：

1. 把每个外层 `send_call()` awaitable 立即 `co_spawn` 到 client executor。
2. 为每个启动操作创建独立的 start state。
3. 返回一组扁平化的 `async_result<R>`；每个结果只等待自己的启动过程和 RPC 响应。
4. 一个请求失败、超时或被取消，不取消其他请求。
5. 某个启动阶段抛出的异常保存到对应 start state，并在 `co_await` 该结果时重新抛出。

`send_calls()` 不等待所有响应，也不要求按参数顺序等待结果。

### 4.3 sender 的所有权

- `async_result<R>` 是 move-only、single-consumer；公开的 `wait()` 隐藏底层 awaitable 的 move。
- 每个 sender 只能调用并 `co_await wait()` 一次。
- sender 暂时不被等待时，请求仍继续执行，响应会被缓存。
- sender 被丢弃时，请求仍由 deadline 管理；响应、超时或连接关闭后状态会自动释放。
- 需要主动取消时，通过等待协程的 Asio cancellation slot 取消；首版不额外增加远端 cancel 消息。

## 5. v2 Client 内部设计

### 5.1 连接状态

```cpp
struct multiplex_client_session {
  shared_ptr<socket_t> transport;
  strand<any_io_executor> executor;
  uint64_t next_seq_num;
  unordered_map<uint64_t, shared_ptr<pending_request>> pending;
  unordered_set<uint64_t> abandoned_seqs;
  deque<outgoing_frame> write_queue;
  size_t queued_bytes;
  bool writing;
  bool reading;
};
```

每次重连都会创建全新的 `socket_t` 和 `multiplex_client_session`；session 对象本身就是连接世代边界，不需要让旧回调读取 `rpc_client` 上的可变 generation 数字。

所有字段只能在 session 自己的 executor 上访问，不使用 mutex。公开接口从其他 executor 调用时，必须通过 `co_spawn(session_executor, ..., use_awaitable)` 进入 session executor；完成后调用协程回到原 executor。

异步读写、deadline 和 completion 回调只捕获 `shared_ptr<multiplex_client_session>`，不能捕获裸 `rpc_client*`。

### 5.2 pending state

```cpp
enum class request_state {
  queued,
  writing,
  waiting_response,
  completed,
};

struct pending_request {
  uint64_t seq_num;
  uint64_t generation;
  request_state state;
  steady_timer deadline;
  steady_timer completion_notify;
  optional<raw_response> response;
  rpc_errc completion_error;
  bool completed;
};
```

deadline 与 completion notification 必须分开：即使调用方晚些时候才 `co_await` sender，请求也应按原始 deadline 超时并从 pending 中删除。

### 5.3 启动顺序

一次 `send_call_for()` 的外层 awaitable 按以下顺序执行：

1. 校验 timeout；`timeout <= 0` 返回一个已完成、结果为 `request_timeout` 的 sender，不序列化也不发送。
2. 把参数按值保存在外层 coroutine frame，避免调用返回后引用悬空。
3. 完成参数序列化和 request body 构造。
4. 进入 client executor，确认连接打开且处于 v2/unset 模式。
5. 检查 pending 数量、写队列条目数和写队列字节数上限。
6. 分配递增的非零 seq；回绕前关闭并重建连接，不在同一代连接复用旧 seq。
7. 设置 `version=v2`、`seq_num=seq`，构造完整且独立拥有内存的 frame。
8. 创建 pending state 和 deadline operation。
9. 先执行 `pending.emplace(seq, state)`。
10. 再把 frame 加入唯一 write queue。
11. 启动独立 deadline watcher。
12. 返回只等待该 state 的第二层 `async_result<R>`。

必须先登记 pending 再把帧暴露给 writer。frame 构造、内存分配等所有可能抛异常的操作应尽量在 pending 登记前完成；登记后的任何异常必须通过 scope guard 把 pending 提取并完成，不能遗留孤儿项。

### 5.4 唯一 writer

- 每条 v2 连接最多一个 `async_write`。
- 新请求只追加完整 frame，不直接写 socket。
- writer 取出队首前再次检查 pending 是否仍存在；排队期间已超时或取消的请求直接跳过。
- 写成功后把 pending 状态从 `writing` 改为 `waiting_response`。
- 当前帧写失败时关闭连接，并以 `write_error` 完成全部 pending；队列中剩余 frame 一并清空。

### 5.5 唯一 reader

- 每条 v2 连接只有一个常驻 read loop。
- header 和 body 都是当前循环迭代的局部变量。
- 分配 body 前校验 magic、version、serialize type、msg type、seq、body length 和 attachment length。
- 收到响应后按 seq 从 pending map 中提取节点；只有提取成功者可以完成 state。
- pending 命中：取消 deadline，缓存 raw response，唤醒该 sender。
- seq 位于 `abandoned_seqs`：这是超时或取消后的迟到响应，删除记录并丢弃。
- seq 两处都不存在：属于重复响应或对端发送的未知响应，直接丢弃，绝不误配给其他 pending。

返回值反序列化在 sender 恢复后执行。反序列化异常只由当前 sender 抛出，不能终止 read loop，也不能关闭连接。

## 6. v2 Server 内部设计

### 6.1 读循环与请求对象

v2 server 读完一整个请求帧后，构造独立请求对象：

```cpp
struct v2_request {
  protocol_version version;
  uint64_t seq_num;
  uint32_t function_id;
  std::string body;
  shared_ptr<multiplex_server_session> session;
};
```

`body` 必须由该对象独占，不能传递指向连接复用 buffer 的 `string_view`。

```cpp
while (!closed) {
  auto request = co_await read_complete_v2_frame();
  asio::co_spawn(executor,
                 handle_v2_request(std::move(request)),
                 guarded_completion_handler);
}
```

read loop 不等待 `handle_v2_request()`。普通协程 handler 即使在内部等待 timer、数据库或其他 RPC，也不会阻止连接继续收包。

### 6.2 handler context

现有 `thread_local tls_data` 不能作为 v2 coroutine-local context：多个 handler 在同一线程交错执行时会互相覆盖 version/seq/connection。

v2 为每个请求显式创建 `rpc_context(session, version, seq)`：

- 普通“返回结果”的 handler 保持原签名。
- 需要延迟或手动响应的 v2 handler 通过第一个参数显式接收 `rpc_context`。
- `route_multiplex()` 识别 context-aware handler，并把请求自己的 context 注入；context 不从请求 body 反序列化。
- `rpc_context` 保存 version、seq、弱/强 session 引用以及共享的 response-once 原子状态。
- v1 的默认构造 `rpc_context` 和 TLS 行为保留在 v1 路径，不拿来承载 v2 并发上下文。

示例：

```cpp
asio::awaitable<int> normal_v2_handler(int value) {
  co_return value;
}

void delayed_v2_handler(rpc_context ctx, int value) {
  asio::co_spawn(ctx.get_executor(), delayed_response(std::move(ctx), value),
                 asio::detached);
}
```

### 6.3 响应队列

所有 v2 响应都先构造成完整 frame，再进入每连接唯一的 `response_queue`：

- response header 原样使用 request 的 `version` 和 `seq_num`。
- 任意时刻只有一个 server `async_write`。
- `response_queue` 默认最多 1024 项，并同时限制总字节数。
- 队列已满时 `enqueue_response()` 立即返回 `rpc_errc::queue_full`，不等待 socket 可写。
- 显式 `rpc_context::response()` 把 `queue_full` 返回给 handler。
- 自动响应无法入队时记录错误并关闭该 v2 连接，避免 client 对一个已被丢弃的响应永久等待。
- server socket 写失败时清空队列并关闭连接；对端 client 会由自己的 read loop 以 `read_error` 唤醒所有 pending。

并发 handler 数量也设置上限。超过上限时不启动 handler，而是尝试给该 seq 返回 `queue_full`；如果连拒绝响应都无法入队，则关闭连接。

## 7. 超时、取消与一次完成

响应、deadline、取消和连接关闭都可能竞争结束同一个请求。统一规则是：谁先在 client executor 上从 pending map 成功提取该 seq，谁负责完成；其他路径什么也不做。

```cpp
auto node = pending.extract(seq);
if (node.empty()) {
  return;
}

node.mapped()->deadline.cancel();
node.mapped()->complete(result); // complete 内部仍做一次性保护
```

### 7.1 超时

- deadline 从外层 `send_call_for()` 启动请求时开始，包含写队列等待、socket 写入和服务端处理时间。
- 超时只删除当前 seq，返回 `request_timeout`，不关闭健康的 v2 连接。
- 未开始写的 frame 由 writer 跳过。
- 已经写出的请求无法撤回；seq 放入有界 `abandoned_seqs`，迟到响应到达后丢弃。

### 7.2 取消

- sender 的等待协程收到 Asio cancellation 后，在 client executor 上尝试提取对应 pending。
- 成功提取时返回 `request_cancelled`，并按超时相同方式处理 queued frame 或迟到响应。
- 如果响应已经先完成，取消不覆盖已缓存的响应。
- 首版不发送远端 cancel frame，因此 server handler可以继续运行。

### 7.3 连接错误和主动关闭

- read error、write error、协议错误和主动 `close()` 都先移动整个 pending map，再逐个取消 deadline 和完成 sender。
- `fail_all()` 不在遍历原 map 时恢复用户协程，避免重入修改容器。
- 每个 state 的 completion 最多调用一次。

## 8. 重连和连接世代

重连不能只替换旧状态里的 socket 成员。应创建全新的 shared session：

1. 从 `rpc_client` 取下旧 session。
2. 关闭旧 socket，以 `socket_closed` 完成旧 session 的全部 pending。
3. 创建新的 socket 和新的 v1/v2 状态，generation 递增。
4. 新连接从 `connection_mode::unset` 开始。
5. 旧 reader、writer、deadline 和 handler 只捕获旧 session；即使迟到回调执行，也无法访问新 session 的 pending 或 socket。

旧请求不自动迁移或重发到新连接。

## 9. 资源上限与错误码

v2 首版提供以下每连接默认值，并允许在连接开始前配置：

| 资源 | 默认上限 | 达到上限的行为 |
|---|---:|---|
| client pending RPC | 1024 | 当前 sender 返回 `queue_full`，不发送 |
| client request queue | 1024 项 | 当前 sender 返回 `queue_full`，不发送 |
| client queued bytes | 64 MiB | 当前 sender 返回 `queue_full`，不发送 |
| client abandoned seq | 4096 | 关闭连接，全部 pending 返回 `protocol_error` |
| server active v2 handlers | 1024 | 给当前 seq 返回 `queue_full` |
| server response queue | 1024 项 | enqueue 立即返回 `queue_full` |
| server queued bytes | 64 MiB | enqueue 立即返回 `queue_full` |
| v2 frame body | 16 MiB | `message_too_large` 或关闭恶意对端 |

新增错误码必须追加到 `rpc_errc` 末尾，不能改变现有数值：

```cpp
queue_full,
request_cancelled,
protocol_mode_conflict,
message_too_large,
```

本地序列化异常、返回值反序列化异常和 `std::bad_alloc` 继续以 C++ 异常传播；协议和传输状态使用 `call_result.ec`。

## 10. 异常处理矩阵

| 场景 | 当前请求 | 连接 | 其他 pending |
|---|---|---|---|
| timeout <= 0 | `request_timeout` | 不变 | 不受影响 |
| 参数序列化失败 | 外层 await 抛异常 | 不变 | 不受影响 |
| pending/发送队列满 | `queue_full` | 不变 | 不受影响 |
| 单请求超时 | `request_timeout` | 保持 | 不受影响 |
| 单请求取消 | `request_cancelled` | 保持 | 不受影响 |
| 返回值反序列化失败 | sender 抛异常 | 保持 | 不受影响 |
| 已超时/取消请求的迟到或重复 seq | 丢弃响应 | 保持 | 不受影响 |
| v2 收到 v1/seq 0 响应 | `protocol_error` | 关闭 | 全部 `protocol_error` |
| socket 读失败 | `read_error` | 关闭 | 全部 `read_error` |
| socket 写失败 | `write_error` | 关闭 | 全部 `write_error` |
| 主动 close/重连 | `socket_closed` | 关闭旧连接 | 全部 `socket_closed` |
| server handler 业务异常 | 现有 function error 响应 | 保持 | 不受影响 |
| server response queue 满 | 显式 `rpc_context::response()` 返回 `queue_full`；自动响应路径关闭连接 | 见左 | client pending 由断连唤醒 |
| v1/v2 API 混用 | `protocol_mode_conflict` | 保持原模式 | 不受影响 |

登记 pending 之后的 frame 入队、timer 启动或其他操作如果抛异常，必须用 scope guard 回滚 pending 和队列计数，然后重新抛出。任何 detached v2 handler 都必须带 completion callback，禁止异常逃出 detached coroutine。

## 11. 测试验收

### 11.1 v1 回归与隔离

- v1 client 对旧 server 的请求/响应字节和行为不变。
- 旧 client 可以调用新 server 的 v1 路径。
- v1 `call/call_for/subscribe` 的现有测试全部不改语义地通过。
- 同一连接先 v1 后 v2、先 v2 后 v1 都返回 `protocol_mode_conflict`，且不产生并发读。
- v2 client 对不支持 v2 的 server 不降级，不把 seq 0 响应误配给 pending。

### 11.2 API 灵活性

- 先启动慢请求，再启动快请求，先 `co_await fast` 时必须在 slow 之前返回。
- 上述测试不能使用 `operator&&`、collect-all 或 handler 内部 detached response 来制造假并发。
- `send_calls()` 支持不同返回类型，并允许按任意顺序等待。
- 响应先于第二层 `co_await` 到达时结果仍可正确取得。

### 11.3 v2 协议与并发

- 同连接大量请求的 seq 唯一且递增。
- pending 一定先于请求帧进入 writer 可见状态。
- server 使用真正会 `co_await timer` 后再返回的普通 coroutine handler，证明慢 handler 不阻塞快 handler。
- 不同 body 长度和内容并发时，每个 handler 只读取自己的 body。
- server 乱序响应仍准确回到对应 sender。
- response header 精确回传 request version 和 seq。

### 11.4 异常与资源

- 请求在 queued、writing、waiting_response 三种状态分别超时。
- 请求在三种状态分别取消；迟到响应被丢弃且不污染下一调用。
- 多个 pending 存在时注入 read error、write error 和主动 close，全部 sender 被唤醒且只完成一次。
- 写队列中有多个 frame 时注入首帧/中间帧写失败，队列和 pending 最终为空。
- 重连期间旧请求返回 `socket_closed`，旧回调和迟到响应不能完成新请求。
- pending、请求队列、响应队列、字节数和 handler 数量达到边界时执行明确拒绝策略。
- 参数序列化、frame 构造和返回值反序列化抛异常后，pending 与队列计数正确。
- 重复/未知 seq 被丢弃；错误 version、seq 0、非法 body length 触发预期协议错误。

新增的 v2 网络测试使用系统分配的临时端口，不修改 v1 既有测试的端口和语义。需要精确制造半写、写失败和迟到帧的场景时使用可注入 transport/fake socket，不依赖不稳定的时间竞争。

## 12. 实施顺序

1. 恢复并冻结 v1 client/server 路径；删除当前从 `connect/call/call_for` 进入的多路复用状态，并补齐 v1 回归测试。
2. 增加显式 v1/v2 常量、连接模式和追加式错误码。
3. 实现独立 `multiplex_client_session`、pending state、单 writer 和单 reader。
4. 实现 `send_call()`/`send_call_for()` 两阶段接口及结果缓存。
5. 实现热启动但非 collect-all 的 `send_calls()`。
6. 实现独立 `multiplex_server_session`、完整请求对象和并发 handler。
7. 为 v2 增加显式 `rpc_context` 注入，移除对 TLS 的并发依赖。
8. 实现有界 response queue、client 资源上限、超时和取消。
9. 实现整 session 重连隔离与 fail-all。
10. 按第 11 节完成故障注入、边界和协议兼容测试。

只有以下条件全部满足后才算多路复用完成：调用方无需 collect-all 即可独立等待；普通慢 coroutine handler 不阻塞快请求；所有队列有界；所有失败路径不会留下 pending；v1 现有行为不变。
