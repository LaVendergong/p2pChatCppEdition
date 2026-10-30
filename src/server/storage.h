// 分段 append-only 日志存储（见 docs/04-存储引擎.md）
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "chat/error.h"
#include "chat/net.h"

namespace chat {

struct StoredMessage {
    uint64_t msg_id = 0;
    uint64_t ts_ms = 0;
    std::string from;
    std::string text;
    uint8_t kind = 0;
};

// 一个房间的数据文件集合。所有公开方法线程安全。
class RoomStore {
public:
    RoomStore() = default;
    ~RoomStore();
    RoomStore(const RoomStore&) = delete;
    RoomStore& operator=(const RoomStore&) = delete;

    // max_bytes > 0 时限制房间总数据量，超限拒绝写入（不静默删旧数据）
    Status open(const std::string& name, const std::string& dir, uint32_t segment_bytes, bool repair,
                uint64_t max_bytes = 0);
    void close();

    // 分配 msg_id 并写入 page cache（是否落盘由 StorageManager 的 flush 线程决定）
    Status append(StoredMessage& m);

    // fdatasync 当前段与索引；顺带定期刷新 room.meta
    Status sync();
    bool dirty() const;

    // 读取 msg_id < before_id 的最近 limit 条，按 id 升序返回
    Status read_before(uint64_t before_id, uint32_t limit, std::vector<StoredMessage>& out);
    Status read_latest(uint32_t limit, std::vector<StoredMessage>& out);

    uint64_t last_msg_id() const;
    uint64_t msg_count() const;
    const std::string& name() const { return name_; }

private:
    struct Segment {
        uint64_t first_id = 0;
        uint64_t last_id = 0;
        uint64_t bytes = 0;
        uint64_t records = 0;
        UniqueFd fd;
        UniqueFd idx_fd;
        std::string path;
        std::string idx_path;
    };

    Status load_meta();
    Status save_meta_locked();
    Status scan_all_locked();
    Status scan_one_locked(Segment& seg, bool rebuild_idx);
    Status roll_segment_locked();
    Status open_segment_locked(Segment& seg, uint64_t first_id, bool create);
    Status rebuild_idx_locked(Segment& seg);
    // 段内按 id 二分，返回 idx 序号（0..records），表示第一条 > target 的位置
    Status idx_lower_bound_locked(Segment& seg, uint64_t target, uint64_t& pos);
    Status read_record_at_locked(Segment& seg, uint64_t offset, StoredMessage& m, uint64_t& next_off);

    mutable std::mutex mu_;
    std::string name_;
    std::string dir_;
    std::string meta_path_;
    uint32_t segment_bytes_ = 64u << 20;
    uint64_t max_bytes_ = 0;    // 0 = 不限
    uint64_t total_bytes_ = 0;  // 所有段文件字节数之和
    uint64_t last_msg_id_ = 0;
    uint64_t msg_count_ = 0;
    bool dirty_ = false;
    bool meta_dirty_ = false;
    std::vector<Segment> segments_;
};

// ---------------------------------------------------------------------------
class StorageManager {
public:
    // tag 由调用方提供，落盘完成后原样回传，用于关联发送者与 client_msg_id
    using CommitCallback = std::function<void(const std::string& room, const StoredMessage& msg, uint64_t tag)>;

    ~StorageManager();
    Status init(const std::string& data_dir, uint32_t segment_bytes, const std::string& sync_mode, bool repair,
                uint64_t max_room_bytes = 0);

    Status open_room(const std::string& name, RoomStore** out);
    bool has_room(const std::string& name) const;
    std::vector<std::string> list_rooms() const;
    RoomStore* room(const std::string& name);

    // 追加消息：分配 msg_id 后进入待提交队列，落盘后由回调交付
    Status append(const std::string& name, StoredMessage& m, uint64_t tag = 0);

    void set_commit_callback(CommitCallback cb);
    void start_flusher();
    void stop();

private:
    void flush_loop();

    std::string data_dir_;
    uint32_t segment_bytes_ = 64u << 20;
    std::string sync_mode_ = "batch";
    bool repair_ = false;
    uint64_t max_room_bytes_ = 0;
    std::vector<std::string> known_rooms_;  // 磁盘上存在的房间（可能尚未打开）

    mutable std::mutex rooms_mu_;
    std::unordered_map<std::string, std::unique_ptr<RoomStore>> rooms_;

    // 待刷盘队列：append 只把记录写进 page cache 并入队，fdatasync 与回调由 flusher 批量完成。
    // （试过把 pwrite 也搬到 flusher，实测 p50 从 4ms 恶化到 9ms：多一次线程交接与攒批等待，
    //   而 pwrite 到 page cache 本身只有微秒级，不值得。）
    struct PendingItem {
        std::string room;
        StoredMessage msg;
        uint64_t tag = 0;
    };
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<PendingItem> pending_;
    std::thread flusher_;
    bool stop_ = false;
    CommitCallback cb_;
};

// 供 cht-dump 使用：顺序扫描段文件，返回有效末尾
Status scan_segment_file(const std::string& path, const std::function<void(const StoredMessage&)>& fn,
                         uint64_t& valid_end);

}  // namespace chat
