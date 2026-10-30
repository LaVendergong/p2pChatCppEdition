#include "auth.h"

#include <sstream>

#include "chat/fs.h"
#include "chat/protocol.h"
#include "chat/sha256.h"
#include "chat/log.h"

namespace chat {

Status Auth::load(const std::string& path, bool create_if_missing) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    std::string data;
    Status st = fs::read_file(path, data);
    if (!st) {
        if (st.e == Err::sys_errno && !fs::exists(path)) {
            if (!create_if_missing) return Status::fail(Err::storage_error, "account db missing: " + path);
            const size_t slash = path.find_last_of('/');
            if (slash != std::string::npos) {
                CHAT_TRY(fs::mkdirs(path.substr(0, slash)));
            }
            return Status::success();  // 空库
        }
        return st;
    }
    accounts_.clear();
    std::istringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string id_s, name, salt, digest;
        if (!std::getline(ls, id_s, '\t')) continue;
        if (!std::getline(ls, name, '\t')) continue;
        if (!std::getline(ls, salt, '\t')) continue;
        if (!std::getline(ls, digest)) continue;
        Account a;
        a.id = std::strtoull(id_s.c_str(), nullptr, 10);
        a.name = name;
        a.salt = salt;
        a.digest = digest;
        if (a.id >= next_id_) next_id_ = a.id + 1;
        accounts_[name] = std::move(a);
    }
    LOG_INFO("auth: loaded {} accounts from {}", accounts_.size(), path);
    return Status::success();
}

Status Auth::save_locked() {
    std::string buf;
    buf.reserve(accounts_.size() * 100);
    buf.append("# id\tname\tsalt\tdigest\n");
    for (const auto& kv : accounts_) {
        const Account& a = kv.second;
        buf.append(std::to_string(a.id));
        buf.push_back('\t');
        buf.append(a.name);
        buf.push_back('\t');
        buf.append(a.salt);
        buf.push_back('\t');
        buf.append(a.digest);
        buf.push_back('\n');
    }
    return fs::atomic_write_file(path_, buf);
}

LoginResult Auth::login(std::string_view user, std::string_view digest, bool allow_create) {
    std::lock_guard<std::mutex> lk(mu_);
    LoginResult r;
    const std::string name(user);

    auto it = accounts_.find(name);
    if (digest.empty()) {
        // 第一次往返：下发盐
        if (it != accounts_.end()) {
            r.kind = LoginKind::need_digest;
            r.salt = it->second.salt;
            r.user_id = it->second.id;
            return r;
        }
        if (!allow_create) {
            r.kind = LoginKind::bad_credentials;
            return r;
        }
        auto pit = pending_salt_.find(name);
        if (pit == pending_salt_.end()) {
            pit = pending_salt_.emplace(name, random_salt_hex()).first;
        }
        r.kind = LoginKind::need_digest;
        r.salt = pit->second;
        return r;
    }

    if (it != accounts_.end()) {
        if (it->second.digest == digest) {
            r.kind = LoginKind::ok;
            r.user_id = it->second.id;
            r.salt = it->second.salt;
        } else {
            r.kind = LoginKind::bad_credentials;
        }
        return r;
    }

    if (!allow_create) {
        r.kind = LoginKind::bad_credentials;
        return r;
    }
    // 自动注册：必须使用第一步下发的盐
    auto pit = pending_salt_.find(name);
    if (pit == pending_salt_.end()) {
        r.kind = LoginKind::bad_credentials;
        return r;
    }
    Account a;
    a.id = next_id_++;
    a.name = name;
    a.salt = pit->second;
    a.digest = std::string(digest);
    accounts_[name] = a;
    pending_salt_.erase(pit);
    Status st = save_locked();
    if (!st) LOG_ERROR("auth: save failed: {}", err_text(st.e, st.msg));
    LOG_INFO("auth: created account {} (id={})", name, a.id);
    r.kind = LoginKind::ok;
    r.user_id = a.id;
    r.salt = a.salt;
    r.created = true;
    return r;
}

bool Auth::exists(std::string_view user) const {
    std::lock_guard<std::mutex> lk(mu_);
    return accounts_.find(std::string(user)) != accounts_.end();
}

size_t Auth::account_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return accounts_.size();
}

}  // namespace chat
