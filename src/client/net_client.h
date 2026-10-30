// 客户端网络：非阻塞连接、待发缓冲、帧解析。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chat/buffer.h"
#include "chat/error.h"
#include "chat/net.h"
#include "chat/protocol.h"

namespace chat {

class NetClient {
public:
    Status connect_to(const std::string& host, uint16_t port, int timeout_ms = 5000);
    // 接管一个已经建立的 fd（accept 出来的连接），只用于 P2P 入站
    void adopt(UniqueFd fd);
    void close();
    bool connected() const { return fd_.valid(); }
    int fd() const { return fd_.get(); }

    void send(MsgType type, uint64_t seq, const std::string& payload, bool push = false);
    Status flush();
    bool has_pending() const { return out_off_ < out_.size(); }

    // 读出所有完整帧；closed=true 表示连接不可用（对端关闭或读错误）
    Status read_frames(std::vector<Frame>& out, bool& closed);

private:
    UniqueFd fd_;
    Buffer in_;
    std::string out_;
    size_t out_off_ = 0;
};

}  // namespace chat
