// 客户端应用：连接状态机、消息窗口、按键处理、渲染循环。
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "chat/protocol.h"
#include "commands.h"
#include "editor.h"
#include "net_client.h"
#include "render.h"
#include "term.h"

namespace chat {

struct ClientOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 7700;
    std::string user;
    std::string password;
    std::string room = "public";
    bool plain = false;
    bool no_color = false;
};

class App {
public:
    explicit App(ClientOptions opt);
    int run();

private:
    enum class State { disconnected, hello_wait, salt_wait, login_wait, join_wait, ready };

    struct Msg {
        uint64_t id = 0;
        std::string from;
        std::string room;      // 发送时所在房间，重发时用
        uint64_t ts = 0;
        std::string text;
        bool system = false;
        bool pending = false;  // 本地已发出，尚未收到 MSG_SEND_ACK
        bool failed = false;   // 断线等原因未能确认
        uint64_t cmid = 0;     // 本地消息号，用于匹配 ACK
    };

    // 连接/登录完成前提交的消息，登录加入房间后补发
    struct Outgoing {
        std::string room;
        std::string text;
    };

    // 连接与协议
    bool connect_once();
    void send_hello();
    void send_login();
    void send_join(const std::string& room);
    void send_message(const std::string& text);
    void flush_outbox();
    void retry_failed();  // Ctrl-R：重发最近一条失败消息
    void send_history(uint64_t before_id, uint32_t limit);
    void request_older();          // 上翻到顶时自动加载更早历史
    uint64_t oldest_msg_id() const;
    void append_local_pending(const std::string& room, const std::string& text, uint64_t cmid);
    void on_socket();
    void handle_frame(const Frame& f);
    void handle_disconnect(const std::string& why);
    void tick();

    // 输入
    void on_stdin();
    void handle_plain_line();
    void handle_bytes();
    bool key_sequence(const std::string& s, size_t pos, size_t& consumed, std::string& action);
    void submit();
    void exec_command(const ParsedCommand& pc);

    // 显示
    void add_system(const std::string& text);
    void add_message(const MessageView& v);
    // 把一批历史消息去重后插到本地最前面；keep_viewport 时保持当前滚动位置
    void merge_older(const std::vector<std::string_view>& items, bool keep_viewport);
    void rebuild_cache(int cols);
    void render();
    void plain_print(const std::string& line);

    ClientOptions opt_;
    NetClient net_;
    TerminalGuard term_;
    Renderer renderer_;
    Editor editor_;

    State state_ = State::disconnected;
    bool online_ = false;
    bool quit_ = false;
    bool dirty_ = true;
    int ctrl_c_ = 0;

    std::string user_;
    std::string salt_;
    std::string digest_;
    std::string room_ = "public";
    std::string peer_info_;
    size_t member_count_ = 0;

    std::deque<Msg> msgs_;
    std::vector<std::string> cache_;   // 已折行、带颜色的屏幕行
    bool cache_dirty_ = true;          // 消息或宽度变化后需要重建
    size_t cache_cols_ = 0;
    size_t scroll_ = 0;          // 从底部向上偏移的显示行数
    size_t printed_plain_ = 0;   // plain 模式已打印的消息数
    uint64_t last_id_ = 0;
    bool no_more_history_ = false;
    bool history_pending_ = false;  // 已发出 HISTORY_REQ，等待应答
    size_t new_below_ = 0;          // 上翻期间到达的新消息数

    uint64_t seq_ = 0;
    uint64_t next_cmid_ = 0;
    uint64_t last_ping_ms_ = 0;
    uint64_t next_reconnect_ms_ = 0;
    int backoff_ms_ = 500;

    std::string inbuf_;  // stdin 原始字节
    std::vector<Outgoing> outbox_;
};

}  // namespace chat
