#include "session.h"

#include <algorithm>

namespace chat {

void Session::enqueue(std::string data) {
    if (data.empty()) return;
    out_bytes += data.size();
    OutChunk c;
    c.own = std::move(data);
    out.push_back(std::move(c));
}

void Session::enqueue_shared(std::shared_ptr<const std::string> data) {
    if (!data || data->empty()) return;
    out_bytes += data->size();
    OutChunk c;
    c.shared = std::move(data);
    out.push_back(std::move(c));
}

bool Session::has_room(const std::string& room) const {
    return std::find(rooms.begin(), rooms.end(), room) != rooms.end();
}

void Session::add_room(const std::string& room) {
    if (!has_room(room)) rooms.push_back(room);
}

void Session::remove_room(const std::string& room) {
    rooms.erase(std::remove(rooms.begin(), rooms.end(), room), rooms.end());
}

void Session::touch(uint64_t now) { last_active_ms = now; }

void Session::remember_ack(uint64_t client_msg_id, uint64_t msg_id) {
    if (idem_.size() >= kIdemCapacity && !idem_order_.empty()) {
        idem_.erase(idem_order_.front());
        idem_order_.pop_front();
    }
    idem_[client_msg_id] = msg_id;
    idem_order_.push_back(client_msg_id);
}

bool Session::lookup_ack(uint64_t client_msg_id, uint64_t& msg_id) const {
    auto it = idem_.find(client_msg_id);
    if (it == idem_.end()) return false;
    msg_id = it->second;
    return true;
}

}  // namespace chat
