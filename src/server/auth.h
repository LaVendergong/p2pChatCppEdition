// 账号表：meta.db 文本行存储（id \t name \t salt \t digest），登录走"取盐→提交摘要"两次往返。
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "chat/error.h"

namespace chat {

enum class LoginKind { ok, need_digest, bad_credentials };

struct LoginResult {
    LoginKind kind = LoginKind::bad_credentials;
    uint64_t user_id = 0;
    std::string salt;
    bool created = false;
};

class Auth {
public:
    Status load(const std::string& path, bool create_if_missing);

    // digest 为空 → need_digest（返回盐）；否则校验，用户不存在且 allow_create 时注册
    LoginResult login(std::string_view user, std::string_view digest, bool allow_create);

    bool exists(std::string_view user) const;
    size_t account_count() const;

private:
    struct Account {
        uint64_t id = 0;
        std::string name;
        std::string salt;
        std::string digest;
    };

    Status save_locked();

    std::string path_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Account> accounts_;
    std::unordered_map<std::string, std::string> pending_salt_;  // 首次取盐但未注册
    uint64_t next_id_ = 1;
};

}  // namespace chat
