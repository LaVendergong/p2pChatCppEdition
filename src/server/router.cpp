#include "router.h"

namespace chat {

void Router::join(const std::string& room, const std::string& user) {
    std::lock_guard<std::mutex> lk(mu_);
    rooms_[room].insert(user);
    user_rooms_[user].insert(room);
}

void Router::leave(const std::string& room, const std::string& user) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = rooms_.find(room);
    if (it != rooms_.end()) {
        it->second.erase(user);
        if (it->second.empty()) rooms_.erase(it);
    }
    auto uit = user_rooms_.find(user);
    if (uit != user_rooms_.end()) {
        uit->second.erase(room);
        if (uit->second.empty()) user_rooms_.erase(uit);
    }
}

void Router::leave_all(const std::string& user) {
    std::lock_guard<std::mutex> lk(mu_);
    auto uit = user_rooms_.find(user);
    if (uit == user_rooms_.end()) return;
    for (const auto& room : uit->second) {
        auto it = rooms_.find(room);
        if (it == rooms_.end()) continue;
        it->second.erase(user);
        if (it->second.empty()) rooms_.erase(it);
    }
    user_rooms_.erase(uit);
}

std::vector<std::string> Router::members(const std::string& room) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    auto it = rooms_.find(room);
    if (it == rooms_.end()) return out;
    out.assign(it->second.begin(), it->second.end());
    return out;
}

std::vector<std::string> Router::rooms_of(const std::string& user) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    auto it = user_rooms_.find(user);
    if (it == user_rooms_.end()) return out;
    out.assign(it->second.begin(), it->second.end());
    return out;
}

std::vector<std::string> Router::all_rooms() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(rooms_.size());
    for (const auto& kv : rooms_) out.push_back(kv.first);
    return out;
}

size_t Router::member_count(const std::string& room) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = rooms_.find(room);
    return it == rooms_.end() ? 0 : it->second.size();
}

size_t Router::room_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return rooms_.size();
}

}  // namespace chat
