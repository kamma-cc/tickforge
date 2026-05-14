# tickforge — Design Doc

> Status: draft v0.1 (2026-05-14)
> 目的:为 tickforge 的接口形态、运行时架构、关键 trade-off 提供一份初版设计参考。文档中标记为 "推理" 的部分均**未经实测**,仅基于第一性原理(syscall / cache coherence / 内核网络栈成本)推导,落地前需在目标硬件上 benchmark 验证。

---

## 1. 项目目标

tickforge 生成假的市场数据(orderbook、trade、ticker),用于压测其他交易相关系统的极限性能。

**核心约束**:tickforge 自己**绝对不能成为被测系统的瓶颈**。被测系统的吞吐 / 延迟天花板必须由它自己决定,不能被 tickforge 的生成或分发开销污染。

## 2. 场景拆解

"提供数据" 不是一种场景,而是三种,选型完全不同。把它们混在一起讨论会得错答案。

| 场景 | 描述 | 接口形态 |
|---|---|---|
| **A. 内部处理极限** | 测被测系统每秒能消化多少 tick、p99 处理延迟。tickforge 不能引入任何不必要开销。 | 进程内 callback / SHM ring |
| **B. 端到端真实表现** | 被测系统平时接 CEX WebSocket 或交易所 UDP multicast。要测它在"真实接入形态"下的表现。 | 模拟真实协议(WS+JSON / UDP) |
| **C. 跨语言 / 跨机器 fan-out** | 同时压测多个语言版本,或测网络部署形态。 | SHM(本机)/ UDP(跨机) |

被测目标决定 sink 选型,而不是反过来。

## 3. 接口候选与定位

| 接口 | 单条延迟量级(推理) | 适合场景 | 主要代价 |
|---|---|---|---|
| 进程内 callback (link as lib) | 函数调用 + cache 访问 | A | 必须 link C++,故障耦合,不跨语言 |
| SHM + lock-free SPSC ring | cache coherence 同步 | A、C(本机) | 被测端要 busy-poll,要 cache line padding 防 false sharing |
| UNIX domain socket | syscall + kernel buffer copy | B 的简化版 | 比 SHM 慢一个量级以上 |
| UDP multicast(类 ITCH/MoldUDP64) | 完整网络栈 | B,模拟传统交易所 | 真实但复杂,要做 seq / gap detection |
| WebSocket + JSON | 网络栈 + JSON 序列化 | B,模拟 CEX | tickforge 自己可能 CPU bound 在序列化 |
| io_uring / AF_XDP / DPDK | 减 syscall / 绕内核 | B 的极致版 | 复杂度爆炸,只在确认必要时才上 |

**注**:上表延迟量级排序基于第一性原理(系统调用成本 vs 内存访问 vs 网络栈处理)的推理,**没有 benchmark 引用**。落地后需在目标硬件实测排序是否成立。

ITCH / MoldUDP64 是 Nasdaq 公开发布的协议规范,可作为 UDP sink 的格式参考。

## 4. 核心架构:预生成 + 多 Sink + 时序控制

不论选哪个传输,有一个设计原则可以让 tickforge 几乎不可能成为瓶颈——**把"生成逻辑"和"分发"彻底解耦**。

```
[offline 生成阶段]                      [runtime 回放阶段]
随机游走价格 / 撮合伪造 / 状态机更新       mmap 顺序读 → 写 sink
        ↓                                     ↓
   events.bin (mmap-friendly)            ~ memory-bandwidth bound
```

### 4.1 为什么这样最快(推理)

- 生成 order book 状态机、价格随机游走、合理的 trade size 分布——这些 CPU 不便宜,但**只需做一次**,产物是紧凑 binary 文件。
- runtime 只做 `mmap` → 顺序读 → 写 sink,本质是内存带宽问题。
- 分支预测友好(顺序访问)、prefetcher 友好。
- 时序控制(满速 vs wall-clock 回放)在回放层做,与生成无关。

> 上面是基于内存访问模式与 CPU 流水线行为的推理,**未经实测**。具体能不能跑满内存带宽,要看 sink 的实现细节。

### 4.2 事件文件格式(初稿)

紧凑 binary,每条事件定长(变长字段如 order book level 用固定上限 + 长度字段)。设计目标:
- 单条事件 cache line 友好(理想 ≤ 64B,大事件不超过 2 个 cache line)
- 顺序读不需要 parse,直接 cast 到 struct
- 自带 origin timestamp 和 sequence number

具体 layout 待定,需要在 Sink 接口稳定后再敲。

## 5. Sink 接口

```cpp
class Sink {
public:
    virtual ~Sink() = default;
    // 返回 false 表示背压(由策略决定丢弃 / 阻塞 / 覆盖)
    virtual bool push(const Event& ev, uint64_t send_ts_ns) = 0;
    virtual void flush() = 0;
};
```

### 5.1 默认实现优先级

| 优先级 | Sink | 用途 |
|---|---|---|
| P0 | `InProcessSink` | A 场景的 ground truth,microbenchmark 基线 |
| P0 | `ShmRingSink` | A、C 本机场景的极限延迟 sink |
| P1 | `WsJsonSink` *or* `UdpSink` | B 场景。先实现哪个看被测系统形态 |
| P2 | io_uring / AF_XDP / DPDK 变体 | 仅在 P1 sink 被证明是瓶颈时考虑 |

## 6. 时序控制模式

两种模式必须都支持:

- **`max-throughput`**:push 到 sink 满为止,测被测系统吞吐天花板。
- **`wall-clock-replay`**:严格按事件 timestamp 节奏推,测被测系统在真实速率下的延迟分布。

`wall-clock-replay` 的实现细节:
- 不能用 `sleep` / `nanosleep`,精度不够、被调度器扰动
- 需要 spin-wait 到 ns 级,典型实现是 `while (rdtsc() < target) _mm_pause();`
- 回放线程必须 pin 到独占 core,且关掉该 core 的 scheduler tick(`isolcpus` / `nohz_full`)以减少 jitter
- 这部分本身性能要求高,实现时单独 benchmark

## 7. 背压策略

被测端跟不上时,sink 必须明确选一种行为——**这影响 sink 接口形状,得在编码前定**。

| 策略 | 行为 | 适用场景 |
|---|---|---|
| **Drop** | 丢弃新事件,递增 drop counter | 模拟 UDP 真实行为 |
| **Block** | 阻塞 producer 等慢 consumer | 测吞吐天花板时(被测端是瓶颈,我们想知道它实际跑多快) |
| **Overwrite** | ring 绕回覆盖未读数据 | SHM ring 的天然行为,被测端用 seq num 检测丢失 |

建议:每种 sink 默认一种策略,但允许通过 config 切换。

## 8. 序列号 / Gap detection

模拟真实交易所的 sequence number——每条事件单调递增。被测系统应能根据 seq 检测丢包 / 乱序。

这同时让 tickforge 可以验证自己:被测端报的 "received seq range" 应与 tickforge 发出的范围对齐(差集就是 drop 数)。

## 9. 时钟与时间戳

三个时间点要可对齐:

- **`origin_ts`**:事件在历史 / 模拟序列中的"应当发生"时刻。来自预生成阶段,落在 binary 文件里。
- **`send_ts`**:tickforge runtime 实际把事件交给 sink 的时刻。runtime 写入。
- **`recv_ts`**:被测系统收到事件的时刻。被测端记录。

延迟分析:
- `recv_ts - send_ts` = sink + 链路延迟(我们关心的)
- `send_ts - origin_ts` = tickforge 自己的 jitter(应趋近 0,如果显著说明 wall-clock-replay 实现有问题)

时钟源建议 `CLOCK_MONOTONIC_RAW` 或 `rdtscp`(后者更快但需要校准)。被测端如果在另一台机器,需要 PTP 对齐——跨机延迟测量不要靠 NTP。

## 10. CPU 亲和性 / 系统调优

tickforge runner 应内置(或文档化)以下配置:

- 生成 / 分发线程 pin 到独占 core
- 关 turbo boost / C-states / P-states 浮动(测延迟时)
- `isolcpus` + `nohz_full` 隔离测试 core
- 关 transparent hugepages 的 defrag,但 mmap 文件用 hugepage 加速
- IRQ affinity:把网卡中断 / 内核线程从测试 core 上挪开

**没有这些,后续 benchmark 数字基本没有意义**。这条不是优化建议,是测试有效性的前提。

## 11. 暂未决定 / 待讨论

- 事件 binary 格式细节(哪些字段、对齐方式、版本兼容)
- order book 状态机的真实度等级(只动 top-of-book? 全 depth?)
- 是否需要支持多 symbol / 多 venue 的并发回放
- WS sink 是模拟某个特定 CEX(如某交易所)还是设计中性接口
- 是否提供 Python / Rust binding(给非 C++ 被测端用)

这些等 P0 sink 落地后,根据实际使用反馈再定。

## 12. 不做什么(non-goals)

- **不做真实撮合引擎**:tickforge 的事件是"看起来合理的假数据",不保证 order book 状态严格自洽到撮合层面。需要真实撮合的场景请用别的工具。
- **不做行情录制 / 回放真实历史**:那是另一类工具(如 nanomsg-based 录制器)。tickforge 专注于"按需大量生成"。
- **不做被测系统的 driver / harness**:tickforge 只负责"喷数据",怎么测、测什么由调用方决定。
