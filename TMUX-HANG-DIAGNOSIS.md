# btop 在 tmux 断开/重连后卡死 —— 诊断记录

> 记录时间：2026-09-27
> 适用版本：btop 1.3.0（Ubuntu 24.04 发行版）→ 已换成基于上游 `main` 的本地构建（1.4.7）
> 本文件用途：**下次再遇到"btop 卡死"先读这里**，不要重复排查。
> 本文件已脱敏（不含主机名/用户名/绝对路径）；机器相关的安装位置见文末"本机环境"。

---

## 1. 症状

- SSH 登录后 `tmux attach` 恢复会话，btop 画面**卡住不动**（时钟不跳、曲线不动、按键无反应）
- 有时表现为**停在 `Terminal size too small` 界面**
- 只能重启 btop 恢复
- 关键：进程通常还活着（`ps` 可见、CPU 占用接近 0），**不是崩溃**

## 2. 环境快照（与问题相关）

| 项 | 值 |
|---|---|
| 发行版 | Ubuntu 24.04.4 LTS，g++ 13.3.0 |
| btop | 1.3.0（apt `/usr/bin/btop`）→ 1.4.7（本地构建 `~/.local/bin/btop`） |
| tmux | 3.4，`TERM=tmux-256color` |
| tmux 配置 | `window-size latest` / **`default-size 80x24`** / `aggressive-resize off` / `destroy-unattached off` |
| btop 配置 | `shown_boxes = "cpu mem net proc"` / `update_ms = 2000` / `background_update = true` |
| 硬件特征 | 12 核工作站 + 大显存 NVIDIA GPU；**内存带宽受限，/proc 采集偏慢**（这是关键前提） |

## 3. 现场证据：btop 自己的日志

`~/.config/btop/btop.log`（1.3.0 的日志路径），**5 次启动 5 次报错**：

```
2026/09/23 (23:11:35) | ===> btop++ v.1.3.0
2026/09/23 (23:11:35) | ERROR: Stall in Runner thread, restarting!
2026/09/24 (14:29:47) | ===> btop++ v.1.3.0
2026/09/24 (14:29:47) | ERROR: Stall in Runner thread, restarting!
2026/09/24 (23:09:41) | ===> btop++ v.1.3.0
2026/09/24 (23:09:41) | ERROR: Stall in Runner thread, restarting!
2026/09/25 (23:41:43) | ===> btop++ v.1.3.0
2026/09/25 (23:41:43) | ERROR: Stall in Runner thread, restarting!
2026/09/26 (21:42:56) | ===> btop++ v.1.3.0
2026/09/26 (21:42:56) | ERROR: Stall in Runner thread, restarting!
```

读日志的坑：banner 是**本次运行第一条日志**时才写的（`Logger::log_write()` 里的 `if (first)`），
所以 banner 与 ERROR 同秒**不代表同时刻**，不能据此判断问题发生在启动瞬间。

## 4. 根因

### 4.1 主因：1.3.0 把整个 resize 流程塞进了 SIGWINCH 信号处理函数

```cpp
// v1.3.0  src/btop.cpp:361
case SIGWINCH:
    term_resize();      // ← 在信号处理函数里干重活
    break;
```

`term_resize()`（`src/btop.cpp:193-268`）在信号上下文里做了这些事：

- `atomic_lock lck(resizing, true)` —— **无上限自旋**等锁
- `Runner::stop()` —— 等渲染线程最多 5 秒，超时后直接 `clean_quit(1)` **杀掉 btop**（`src/btop.cpp:802`）
- `cout << ... << flush`、`sleep_ms(100)`、`Input::poll(10)`
- 一个「等终端变大」的 `while` 循环

而 tmux 重连**必然可能发 SIGWINCH**——实测 attach 时 tmux 会把窗口 resize 成新 client 的尺寸
（复现过 140x39 / 100x29 / 78x19 / 150x44）。处理信号期间 SIGWINCH 被屏蔽，一旦中途失败无法自恢复。

**上游 `main` 已修**（⚠️ 不在任何 release tag 里，`v1.4.7` tag 仍是有问题的代码）：

```cpp
// origin/main
case SIGWINCH:
    Global::resized = true;
    Input::interrupt();
    break;
```

### 4.2 帮凶：5 秒 stall 超时 → `pthread_cancel` 渲染线程

`src/btop.cpp:760` `Runner::run()`：等渲染线程释放 `active` 超 5 秒 →
打 `Stall in Runner thread, restarting!` → `pthread_cancel()` + 重建线程。

上游后来把这条路径**整条删除**（PR #1647 → #1662），改为 10 秒警告 / 30 秒才退出。
删除原因：cancel 恢复路径本身会死锁——线程停在不可取消点上导致 `pthread_join` 永久挂起、
被取消线程留下未解锁的 mutex、取消恰好落在 `sync_start`/`sync_end` 之间会留下陈旧画面。

本机采集慢，**首轮 collect 容易超过 5 秒** → 每次启动触发一次 →
进程从此处于「渲染线程被 cancel 过」的脆弱状态。这正是重连时出问题的前置条件。

### 4.3 tmux 侧陷阱：default-size 恰好比 btop 最小尺寸少一行

- tmux `default-size 80x24`，无 client 的窗口实际只有 **80x23**（状态栏吃掉一行）
- btop 在 `shown_boxes = "cpu mem net proc"` 下的最小需求**正好是 80x24**
  （Cpu 60x8 / Mem 36x10 / Net 36x6 / Proc 44x16，见 `src/btop_draw.cpp:511,1094,1360,1459`）
- 实测：新建 detached 会话 → 80x23 → btop 直接停在 `Terminal size too small`

## 5. 已排除的假设（别再查一遍）

| 假设 | 结论 |
|---|---|
| 未闭合的同步输出块（DECSET 2026）冻结终端 | ❌ 实测 tmux 3.4 直接吃掉 `2026` 序列、**不透传**给外层终端；btop 的 `sync_start/end` 在 tmux 下等于空操作 |
| 单纯 resize 会卡死 | ❌ 1.3.0 上 117x69→100x40→60x20→150x45 全部正常恢复 |
| attach/detach 本身会卡死 | ❌ 1.3.0 上 12 轮随机尺寸 attach / SIGKILL 断线 / 重连压力测试全程存活 |
| pty 缓冲写满 / tmux 停止读取 | ❌ 无 client 时 tmux 仍持续读取；btop 0.3% CPU 停在 `do_select`，画面正常更新 |

**结论：单纯 resize 打不死它，必须叠加「渲染线程已因 stall 被 cancel 过」这个前置状态。**
这也解释了为什么日志里每次启动都有一条 stall。

## 6. 已做的修复

### 6.1 运行逻辑 —— 取上游 `main`

- SIGWINCH 只设标志 + `Input::interrupt()`，信号处理函数里不再做任何重活
- 删除 `pthread_cancel` 整条恢复路径；超时改为 **10 秒警告 / 30 秒才干净退出**
- 新增 `terminal_sync` 配置开关

### 6.2 编译修复 —— 本 fork 独有（commit `0fa2f85`）

上游 `main` 用了 `std::ranges::to`，而 libstdc++ **GCC 14 才提供**
（`__cpp_lib_ranges_to_container`），Ubuntu 24.04 只有 GCC 13.3 → 编译失败：

```
src/btop_tools.hpp:296:67: error: expected primary-expression before '>' token
    std::ranges::to<std::vector<std::string>>();
```

修复：新增 `Tools::to_vector<T>()` 替代，共 4 处调用点
（`btop_tools.hpp` 的 `ssplit()` / `main.cpp` 的 argv / `linux/btop_collect.cpp` 的 `detect_active_cpus()` ×2），语义不变。

## 7. 安装与验证结果

| 项 | 值 |
|---|---|
| 二进制 | `~/.local/bin/btop` —— `1.4.7`，g++ 13.3.0，`GPU_SUPPORT=true` |
| 源码 | `~/src/btop`（fork：`github.com/qiaoxu123/btop`，分支 `fix/tmux-reattach-hang`） |
| 旧版本 | `/usr/bin/btop` 1.3.0 仍在（删除需 sudo；PATH 中 `~/.local/bin` 在前，已被遮蔽） |
| 配置备份 | `~/.config/btop/btop.conf.v1.3.0.bak` |
| **日志路径已变** | 1.4.x 写 `~/.local/state/btop/btop.log`，**不再是** `~/.config/btop/btop.log` |

验证项（全部 PASS）：

- 渲染正常、GPU 面板识别到显卡
- 缩到 60x20（低于最小）→ 显示 `Terminal size too small` → 放大回 150x45 → **正常恢复**
- 12 轮 attach/detach 压力 + 真实 resize（140x39 / 100x29 / 78x19 / 150x44）→ 全程存活、持续重绘
- **全程零日志**：`~/.local/state/btop/btop.log` 根本没被创建 = 没有任何 ERROR/WARNING

## 8. 尚未验证 / 残留风险

- **重载场景未验证**：真正的卡死需要「collect 超过 5 秒」的重载条件。
  验证时机器上正跑着训练任务，没有额外加负载去复现，所以**新版本在重载下的表现没有直接验证**。
  下次训练重载时留意 `~/.local/state/btop/btop.log`。
- **墙钟超时风险**：`Tools::time_ms()` 用 `system_clock`（墙钟），
  `atomic_wait_for()` 的超时判断在时钟跳变时可能瞬间"超时"。
  如果日志出现 `Runner thread stalled for 30s, exiting` 但实际并没有卡 30 秒，就是这个原因。

## 9. 下次卡死时：30 秒现场采集

```bash
# 1. 看窗口尺寸是否低于最小 80x24
tmux list-sessions -F '#{session_name} #{window_width}x#{window_height}'

# 2. 进程状态：S=活着 / D=IO 卡死；nlwp=线程数
ps -o pid,stat,wchan:20,nlwp,pcpu -p $(pgrep -x btop)

# 3. 日志（注意 1.4.x 的新路径）
tail -20 ~/.local/state/btop/btop.log

# 4. 是否响应按键（有反应=卡在等待屏；无反应=线程真卡死）
tmux send-keys -t <session> q
```

判定：

- 停在 `Terminal size too small` → 把窗口拉到 ≥80x24，或按 `1`~`4` 切换面板即可恢复
- 日志出现 `Runner thread stalled for 30s, exiting` → 真的卡死了，把日志发出来
- 日志为空但画面不动 → 新问题，需要重新排查（回到第 5 节，先确认不是已排除的假设）

## 10. 更新方法

```bash
cd ~/src/btop
git fetch upstream && git merge upstream/main
# 若上游新增了 std::ranges::to 用法，同样替换成 Tools::to_vector
make -j8 && make install PREFIX=$HOME/.local
```
