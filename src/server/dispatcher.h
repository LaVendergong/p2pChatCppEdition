// 协议消息分发：鉴权检查 + 按类型路由到具体 handler。
#pragma once

#include "chat/protocol.h"

namespace chat {

class Server;
class Session;

class Dispatcher {
public:
    explicit Dispatcher(Server& server) : server_(server) {}
    void on_frame(Session& s, const Frame& f);

private:
    void on_hello(Session& s, const Frame& f);
    void on_login(Session& s, const Frame& f);
    void on_register(Session& s, const Frame& f);
    void on_join(Session& s, const Frame& f);
    void on_leave(Session& s, const Frame& f);
    void on_msg_send(Session& s, const Frame& f);
    void on_history(Session& s, const Frame& f);
    void on_user_list(Session& s, const Frame& f);
    void on_room_list(Session& s, const Frame& f);
    void on_ping(Session& s, const Frame& f);
    void on_bye(Session& s, const Frame& f);

    Server& server_;
};

}  // namespace chat
