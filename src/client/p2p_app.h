// P2P 客户端：同时监听（被连）与发起（连人），通过 chatsig 自动交换地址。
// 消息存本地，双方各一份；聊天内容不经过服务器。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "chat/crypto.h"
#include "chat/error.h"
#include "chat/protocol.h"
#include "editor.h"
#include "net_client.h"
#include "render.h"
#include "term.h"

namespace chat {

class RoomStore;  // 本地存储（复用分段日志）

struct P2POptions {
    std::string user;
    std::string password;
    std::string sig_host = "127.0.0.1";
    uint16_t sig_port = 7900;
    uint16_t listen_port = 7800;
    std::string data_dir = "./p2pdata";
    bool no_color = false;
    bool force_relay = false;  // 调试用：跳过直连，强制走中继
};

class P2PApp {
public:
    explicit P2PApp(P2POptions opt);
    ~P2PApp();
    int run();

private:
    struct Candidate {
        uint8_t kind = 0;
        std::string ip;
        uint16_t port = 0;
    };

    // 端到端加密状态：握手完成后 ready=true，之后所有聊天内容都用 keys 加解密
    struct Secure {
        bool started = false;  // 已发出自己的 kex
        bool ready = false;    // 双方数据齐全，密钥已派生
        crypto::KeyPair eph{};       // 本次连接的一次性密钥，派生后立刻擦除私钥
        uint8_t peer_pub[32] = {0};
        uint8_t peer_eph[32] = {0};
        crypto::SessionKeys keys{};
        uint64_t send_ctr = 0;  // 本方向消息序号（也用作 AEAD nonce）
        uint64_t recv_ctr = 0;  // 收到的最新序号，用于拒绝重放
        std::string peer_fp;    // 对端指纹（人眼比对用）
        int trust = 0;          // 0=未知 1=首次记录 2=与记录一致 3=与记录不符
    };

    struct PeerConn {
        std::string name;
        NetClient net;          // 直连通道
        bool relay = false;     // true 表示这条会话走 chatsig 转发，net 不可用
        bool ready = false;
        bool inbound = false;
        uint64_t last_active = 0;
        uint64_t last_ping = 0;
        Secure sec;
    };

    struct Msg {
        std::string from;
        uint64_t ts = 0;
        std::string text;
        bool system = false;
        bool failed = false;
    };

    struct Chat {
        std::string peer;
        std::vector<Msg> msgs;
        std::unique_ptr<RoomStore> store;
        bool store_dirty = false;
    };

    // ---- 信令 ----
    bool sig_connect();
    void sig_close();
    void sig_send(MsgType t, const std::string& payload);
    void sig_on_read();
    void sig_dispatch(const Frame& f);
    void sig_send_hello();
    void sig_send_login();
    void sig_send_register();

    // ---- 对端 ----
    void start_chat(const std::string& peer);
    void connect_peer(const std::string& peer, const std::vector<Candidate>& cands);
    void accept_inbound();
    void on_peer_read(PeerConn& c);
    void on_peer_frame(PeerConn& c, const Frame& f);
    void peer_send(PeerConn& c, MsgType t, uint64_t seq, const std::string& payload);
    void peer_close(size_t index, const char* why);
    PeerConn* find_peer(const std::string& name);

    // ---- 端到端加密 ----
    void load_identity();  // 读/生成身份密钥，并加载已知指纹
    void kex_send(PeerConn& c);
    void kex_on(PeerConn& c, const TlvReader& r);
    bool send_text(PeerConn& c, const std::string& text);  // false = 未加密，没发出去
    void recv_text(PeerConn& c, const TlvReader& r);
    void save_known(const std::string& peer, const std::string& fp);
    std::string e2e_state(const PeerConn* c) const;

    Chat& chat_of(const std::string& peer);
    void add_msg(Chat& ch, Msg m);
    void add_system(const std::string& text);
    void persist(Chat& ch, const Msg& m);

    // ---- UI ----
    void on_stdin();
    void submit();
    void exec_command(const std::string& line);
    void render();
    void tick();
    std::string listen_lan_ip() const;

    P2POptions opt_;
    TerminalGuard term_;
    Renderer renderer_;
    Editor editor_;

    NetClient sig_;
    bool sig_online_ = false;
    bool sig_logged_in_ = false;
    bool sig_registered_ = false;
    std::string sig_salt_;
    std::string sig_digest_;
    uint64_t sig_seq_ = 0;
    uint64_t next_retry_ms_ = 0;
    int backoff_ms_ = 500;

    UniqueFd listen_fd_;
    uint16_t listen_port_ = 0;

    std::vector<std::unique_ptr<PeerConn>> peers_;
    std::vector<std::unique_ptr<Chat>> chats_;
    std::string current_;
    std::string pending_lookup_;
    std::string relay_peer_;  // 当前中继会话的对端（阶段 4 只支持一条中继）

    std::vector<Candidate> incoming_cands_;  // 有人要连我们时记下（暂未使用）

    crypto::KeyPair identity_{};               // 本机长期身份密钥
    bool identity_ok_ = false;
    std::map<std::string, std::string> known_;  // 对端用户名 → 指纹（TOFU，类似 known_hosts）

    std::string inbuf_;
    bool quit_ = false;
    bool dirty_ = true;
    size_t scroll_ = 0;
    uint64_t last_tick_ms_ = 0;
    uint64_t last_sync_ms_ = 0;
    size_t cache_cols_ = 0;
    std::vector<std::string> cache_;
    bool cache_dirty_ = true;
};

}  // namespace chat
