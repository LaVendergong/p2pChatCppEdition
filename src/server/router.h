// 房间成员关系表：谁在哪个房间。只存用户名，会话对象由 Server 持有。
#pragma once

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace chat {

class Router {
public:
    void join(const std::string& room, const std::string& user);
    void leave(const std::string& room, const std::string& user);
    void leave_all(const std::string& user);

    std::vector<std::string> members(const std::string& room) const;
    std::vector<std::string> rooms_of(const std::string& user) const;
    std::vector<std::string> all_rooms() const;
    size_t member_count(const std::string& room) const;
    size_t room_count() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::set<std::string>> rooms_;
    std::unordered_map<std::string, std::set<std::string>> user_rooms_;
};

}  // namespace chat
