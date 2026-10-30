# chatTerminal

纯终端环境下的 C++ 聊天工具（C/S 架构），仅依赖 POSIX 系统调用与 C++20 标准库，
在 WSL / Linux 上构建运行。设计目标：**大数据量（千万级消息）**、**强一致持久化**、
**长时间运行不崩**。

```
┌──────────────┐   TCP (自定义二进制帧协议)   ┌──────────────┐
│   chatt      │ ───────────────────────────► │   chatd      │
│ 终端客户端   │ ◄─────────────────────────── │  服务端      │
└──────────────┘                              └──────┬───────┘
                                                     │ append-only WAL
                                                     ▼
                                            data/rooms/<room>/seg-*.log
```

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/00-需求与目标](docs/00-需求与目标.md) | 功能/非功能需求、边界、验收标准 |
| [docs/01-总体架构](docs/01-总体架构.md) | 进程模型、线程模型、数据流、技术选型 |
| [docs/02-模块清单](docs/02-模块清单.md) | 所有待实现模块、文件、职责、依赖顺序 |
| [docs/03-通信协议](docs/03-通信协议.md) | 帧格式、TLV 编码、消息类型字典、时序 |
| [docs/04-存储引擎](docs/04-存储引擎.md) | 分段日志、索引、崩溃恢复、查询 |
| [docs/05-终端界面](docs/05-终端界面.md) | raw mode、渲染、输入编辑、快捷键 |
| [docs/06-可靠性与错误处理](docs/06-可靠性与错误处理.md) | 错误码、超时、重连、优雅退出 |
| [docs/07-开发计划](docs/07-开发计划.md) | 里程碑 M0–M6 与验收 |
| [docs/08-编码规范](docs/08-编码规范.md) | 命名、分层、禁止事项 |
| [docs/09-构建与运行](docs/09-构建与运行.md) | 环境准备、编译、调试、部署 |
| [docs/10-测试与性能](docs/10-测试与性能.md) | 单测、集成、压测方法与指标 |
| [docs/PROGRESS.md](docs/PROGRESS.md) | 过程记录（每阶段实际进展与决策） |

## 快速开始

```bash
# WSL 内
sudo apt-get update && sudo apt-get install -y build-essential cmake gdb
git clone https://github.com/LaVendergong/p2pChatCppEdition.git
cd p2pChatCppEdition
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

# 终端 1：服务端
./build/bin/chatd --port 7700 --data ./data

# 终端 2、3：客户端
./build/bin/chatt --host 127.0.0.1 --port 7700 --user alice
./build/bin/chatt --host 127.0.0.1 --port 7700 --user bob
```

## 目录结构

```
chatTerminal/
├── CMakeLists.txt
├── docs/            # 设计与过程文档（12 篇设计 + PROGRESS）
├── include/chat/    # 公共头文件
├── src/common/      # 公共库：日志/缓冲区/协议/网络/哈希/密码学/UTF-8
├── src/server/      # 服务端（epoll + 分段日志存储）
├── src/sig/         # chatsig 信令服务（P2P 地址交换与中继）
├── src/client/      # 终端客户端（raw mode + diff 渲染，含 P2P 模式）
├── tests/           # 单元测试（自研断言，无第三方）
├── tools/           # cht-load 压测、cht-dump 离线转储
└── scripts/         # 构建、单测、端到端、崩溃、压测脚本
```

## P2P 模式（不经过中心服务器聊天）

信令服务器只交换"地址"，**聊天内容不经过它**；双方消息各存本地。

```bash
# VPS 上（需要公网 IP）
./build/bin/chatsig --port 7900 --data /var/lib/chatsig

# 电脑 A
./build/bin/chatt --p2p --user alice --sig your-vps:7900 --listen 7800 --data ./p2pdata

# 电脑 B（换用户名和监听端口）
./build/bin/chatt --p2p --user bob --sig your-vps:7900 --listen 7801 --data ./p2pdata
```

然后在任意一端输入 `/chat bob`，会自动向信令查询地址并直连（局域网零配置、优先走内网地址）。
**直连失败会自动走中继**（由 chatsig 转发，覆盖双方都在 NAT 后的情况）。

### 端到端加密

聊天内容用 X25519 + HKDF-SHA256 派生的会话密钥做 ChaCha20-Poly1305 加密，
**直连和中继都一样，服务器只看到密文**；每次连接换临时密钥（连接级前向保密）。

首次连接会记录对方的公钥指纹（TOFU），之后指纹变化会告警：

```
/fingerprint     显示自己和各对端的指纹
/trust <name>    指纹变了之后确认接受
```

密码学原语（X25519 / ChaCha20-Poly1305 / HKDF-SHA256）都是手写的，不依赖 OpenSSL，
正确性由 RFC 7748 / 8439 / 5869 官方向量保证，并有独立 Python 实现的互操作测试。

调试时可用 `--force-relay` 强制走中继。详情见 [docs/11-P2P架构.md](docs/11-P2P架构.md)。

## 脚本

| 脚本 | 作用 |
| --- | --- |
| `scripts/check.sh build` | Release 构建（0 警告） |
| `scripts/check.sh asan` | ASan/UBSan 构建 + 单测 |
| `scripts/dev.sh test` | 跑单元测试 |
| `scripts/e2e.sh` | 端到端：双客户端互聊 + 重启保历史 |
| `scripts/crash.sh` | `kill -9` 崩溃一致性（已 ACK 消息零丢失） |
| `scripts/load.sh` | 压测：QPS + p50/p99 |
| `scripts/soak.sh` | 浸泡测试：持续发消息 + 采样 RSS，输出 CSV |
| `scripts/srv_start.sh` / `scripts/cli.sh` | 手工起服务端 / 跑一个 plain 客户端 |
| `scripts/pty_test.py` | 用伪终端驱动 raw mode，自动验证按键回显、发送、渲染 |
| `scripts/p2p_test.py [relay]` | P2P 端到端：chatsig + 两个客户端直连/中继互发，含指纹互验 |
| `scripts/e2e_sec_test.py` | 端到端加密互操作：纯 Python 独立实现握手与 AEAD，验证重放/篡改/明文拒绝 |
| `scripts/sig_test.py` | 信令服务联调（注册/查询/通知/离线） |

存储层单测工具：`./build/bin/cht-bench --dir /home/bench --count 10000000 --reads 100000`

## 许可

内部项目，未定。
