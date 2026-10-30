#include "storage.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "chat/buffer.h"
#include "chat/crc32.h"
#include "chat/fs.h"
#include "chat/log.h"
#include "chat/protocol.h"
#include "chat/time_util.h"

namespace chat {
namespace {

constexpr uint16_t kRecMagic = 0x4352;  // "CR"
constexpr uint8_t kRecVersion = 1;
constexpr uint8_t kRecTypeMsg = 1;
constexpr size_t kRecHeaderSize = 12;
constexpr size_t kIdxEntrySize = 16;
constexpr size_t kScanWindow = 256 * 1024;

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

std::string segment_file_name(uint64_t first_id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%019llu.seg", static_cast<unsigned long long>(first_id));
    return buf;
}

// 记录布局：magic(2) ver(1) type(1) len(4) crc(4) body(len)；crc 覆盖 [version..len] + body
std::string encode_record(const StoredMessage& m) {
    MessageView v;
    v.msg_id = m.msg_id;
    v.ts_ms = m.ts_ms;
    v.from = m.from;
    v.text = m.text;
    v.kind = m.kind;
    TlvWriter tw;
    encode_message(tw, v);
    const std::string& body = tw.data();

    Writer h;
    h.u16(kRecMagic);
    h.u8(kRecVersion);
    h.u8(kRecTypeMsg);
    h.u32(static_cast<uint32_t>(body.size()));

    uint32_t c = crc32(h.data().data() + 2, 6);
    c = crc32(body.data(), body.size(), c);

    Writer cw;
    cw.u32(c);

    std::string out;
    out.reserve(kRecHeaderSize + body.size());
    out.append(h.data());
    out.append(cw.data());
    out.append(body);
    return out;
}

bool decode_record(std::string_view rec, StoredMessage& m) {
    if (rec.size() < kRecHeaderSize) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(rec.data());
    if (be16(p) != kRecMagic) return false;
    if (p[2] != kRecVersion) return false;
    if (p[3] != kRecTypeMsg) return false;
    const uint32_t len = be32(p + 4);
    if (rec.size() < kRecHeaderSize + len) return false;
    const uint32_t want = be32(p + 8);
    uint32_t c = crc32(p + 2, 6);
    c = crc32(p + kRecHeaderSize, len, c);
    if (want != c) return false;

    MessageView mv;
    if (!decode_message(rec.substr(kRecHeaderSize, len), mv)) return false;
    m.msg_id = mv.msg_id;
    m.ts_ms = mv.ts_ms;
    m.from = std::move(mv.from);
    m.text = std::move(mv.text);
    m.kind = mv.kind;
    return true;
}

// 扫描一个已打开的段文件：逐条校验并回调，返回有效末尾偏移
Status scan_fd(int fd, const std::function<void(const StoredMessage&, uint64_t offset)>& fn, uint64_t& valid_end,
               uint64_t& last_id, uint64_t& records) {
    uint64_t off = 0;
    last_id = 0;
    records = 0;
    std::vector<char> win(kScanWindow);

    for (;;) {
        size_t got = 0;
        Status st = fs::pread_some(fd, win.data(), win.size(), off, got);
        if (!st) return st;
        if (got < kRecHeaderSize) break;  // 文件尾或残片

        size_t pos = 0;
        while (pos + kRecHeaderSize <= got) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(win.data() + pos);
            if (be16(p) != kRecMagic || p[2] != kRecVersion || p[3] != kRecTypeMsg) {
                // 非预期的尾部数据：停止解析（后面全部丢弃）
                valid_end = off + pos;
                return Status::success();
            }
            const uint32_t body_len = be32(p + 4);
            const size_t need = kRecHeaderSize + body_len;
            if (need > win.size()) {
                // 单条记录超过扫描窗口，按损坏处理（协议上限远小于此）
                valid_end = off + pos;
                return Status::success();
            }
            if (pos + need > got) break;  // 跨窗口，外层次再读

            StoredMessage m;
            if (!decode_record(std::string_view(win.data() + pos, need), m)) {
                valid_end = off + pos;
                return Status::success();
            }
            if (fn) fn(m, off + pos);
            if (m.msg_id > last_id) last_id = m.msg_id;
            ++records;
            pos += need;
        }
        if (pos == 0) break;  // 无法前进，视为结束
        off += pos;
    }
    valid_end = off;
    return Status::success();
}

}  // namespace

Status scan_segment_file(const std::string& path, const std::function<void(const StoredMessage&)>& fn,
                         uint64_t& valid_end) {
    int fd = -1;
    CHAT_TRY(fs::open_ro(path, fd));
    uint64_t last_id = 0, records = 0;
    std::function<void(const StoredMessage&, uint64_t)> cb;
    if (fn) {
        cb = [&fn](const StoredMessage& m, uint64_t) { fn(m); };
    }
    Status st = scan_fd(fd, cb, valid_end, last_id, records);
    ::close(fd);
    return st;
}

// ------------------------------------------------------------------ RoomStore
RoomStore::~RoomStore() { close(); }

void RoomStore::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (dirty_) {
        for (auto& seg : segments_) {
            if (seg.fd.valid()) (void)fs::fdatasync(seg.fd.get());
            if (seg.idx_fd.valid()) (void)fs::fdatasync(seg.idx_fd.get());
        }
        dirty_ = false;
    }
    for (auto& seg : segments_) {
        seg.fd.reset();
        seg.idx_fd.reset();
    }
    if (meta_dirty_) (void)save_meta_locked();
}

Status RoomStore::open(const std::string& name, const std::string& dir, uint32_t segment_bytes, bool repair,
                       uint64_t max_bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    name_ = name;
    dir_ = dir;
    segment_bytes_ = segment_bytes ? segment_bytes : (64u << 20);
    max_bytes_ = max_bytes;
    meta_path_ = fs::join(dir_, "room.meta");
    CHAT_TRY(fs::mkdirs(dir_));

    const bool meta_ok = load_meta().ok();
    if (repair || !meta_ok) {
        return scan_all_locked();
    }

    // 非最后一段：只信任 meta，但校验文件大小
    for (size_t i = 0; i + 1 < segments_.size(); ++i) {
        const std::string path = fs::join(dir_, segment_file_name(segments_[i].first_id));
        int fd = -1;
        Status st = fs::open_ro(path, fd);
        if (!st) return scan_all_locked();
        uint64_t sz = 0;
        st = fs::file_size(fd, sz);
        ::close(fd);
        if (!st || sz != segments_[i].bytes) {
            LOG_WARN("storage[{}]: segment {} size mismatch, rebuilding", name_, path);
            return scan_all_locked();
        }
        segments_[i].path = path;
        segments_[i].idx_path = path.substr(0, path.size() - 4) + ".idx";
    }

    if (!segments_.empty()) {
        // 最后一段：完整扫描一次，截掉崩溃残留的半条记录
        CHAT_TRY(scan_one_locked(segments_.back(), false));
        last_msg_id_ = segments_.back().last_id;
    }
    uint64_t total = 0;
    uint64_t bytes = 0;
    for (const auto& seg : segments_) {
        total += seg.records;
        bytes += seg.bytes;
    }
    msg_count_ = total;
    total_bytes_ = bytes;
    // 恢复后立刻把修正过的 last_id / 段表写回 meta，避免崩溃前滞后的 meta 一直留着
    meta_dirty_ = true;
    (void)save_meta_locked();
    LOG_INFO("storage[{}]: opened, {} segments, last_id={}, msgs={}", name_, segments_.size(), last_msg_id_,
             msg_count_);
    return Status::success();
}

Status RoomStore::load_meta() {
    segments_.clear();
    std::string data;
    Status st = fs::read_file(meta_path_, data);
    if (!st) return st;
    std::istringstream ss(data);
    std::string line;
    bool seen_version = false;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "v") {
            seen_version = true;
        } else if (key == "last") {
            ls >> last_msg_id_;
        } else if (key == "count") {
            ls >> msg_count_;
        } else if (key == "seg") {
            Segment seg;
            ls >> seg.first_id >> seg.last_id >> seg.bytes >> seg.records;
            seg.path = fs::join(dir_, segment_file_name(seg.first_id));
            seg.idx_path = seg.path.substr(0, seg.path.size() - 4) + ".idx";
            segments_.push_back(std::move(seg));
        }
    }
    if (!seen_version) return Status::fail(Err::storage_error, "bad meta");
    std::sort(segments_.begin(), segments_.end(),
              [](const Segment& a, const Segment& b) { return a.first_id < b.first_id; });
    return Status::success();
}

Status RoomStore::save_meta_locked() {
    std::string buf;
    buf.reserve(256 + segments_.size() * 64);
    buf.append("v 1\n");
    buf.append("last ").append(std::to_string(last_msg_id_)).push_back('\n');
    buf.append("count ").append(std::to_string(msg_count_)).push_back('\n');
    for (const auto& seg : segments_) {
        buf.append("seg ");
        buf.append(std::to_string(seg.first_id)).push_back(' ');
        buf.append(std::to_string(seg.last_id)).push_back(' ');
        buf.append(std::to_string(seg.bytes)).push_back(' ');
        buf.append(std::to_string(seg.records)).push_back('\n');
    }
    Status st = fs::atomic_write_file(meta_path_, buf);
    if (st) meta_dirty_ = false;
    else LOG_ERROR("storage[{}]: save meta failed: {}", name_, err_text(st.e, st.msg));
    return st;
}

Status RoomStore::open_segment_locked(Segment& seg, uint64_t first_id, bool create) {
    seg.first_id = first_id;
    seg.path = fs::join(dir_, segment_file_name(first_id));
    seg.idx_path = seg.path.substr(0, seg.path.size() - 4) + ".idx";

    int fd = -1;
    Status st = fs::open_rw(seg.path, create, fd);
    if (!st) return st;
    seg.fd.reset(fd);

    int ifd = -1;
    st = fs::open_rw(seg.idx_path, true, ifd);
    if (!st) return st;
    seg.idx_fd.reset(ifd);
    return Status::success();
}

Status RoomStore::scan_one_locked(Segment& seg, bool rebuild_idx) {
    if (!seg.fd.valid()) CHAT_TRY(open_segment_locked(seg, seg.first_id, true));
    if (!seg.idx_fd.valid()) {
        int ifd = -1;
        CHAT_TRY(fs::open_rw(seg.idx_path, true, ifd));
        seg.idx_fd.reset(ifd);
    }

    uint64_t valid_end = 0, last_id = 0, records = 0;
    Status st = scan_fd(seg.fd.get(), nullptr, valid_end, last_id, records);
    if (!st) return st;

    uint64_t size = 0;
    st = fs::file_size(seg.fd.get(), size);
    if (!st) return st;
    if (size > valid_end) {
        LOG_WARN("storage[{}]: truncating {} from {} to {} bytes (torn write)", name_, seg.path, size, valid_end);
        CHAT_TRY(fs::ftruncate_to(seg.fd.get(), valid_end));
        (void)fs::fdatasync(seg.fd.get());
    }

    uint64_t idx_size = 0;
    st = fs::file_size(seg.idx_fd.get(), idx_size);
    if (!st) return st;
    const uint64_t want_idx = records * kIdxEntrySize;

    seg.bytes = valid_end;
    seg.records = records;
    seg.last_id = last_id ? last_id : (seg.first_id ? seg.first_id - 1 : 0);

    if (rebuild_idx || idx_size != want_idx) {
        LOG_WARN("storage[{}]: rebuilding index for {} (have {} want {})", name_, seg.idx_path, idx_size, want_idx);
        return rebuild_idx_locked(seg);
    }
    return Status::success();
}

Status RoomStore::rebuild_idx_locked(Segment& seg) {
    // 收集 (id, offset) 后整体重写，保证与数据一致
    std::vector<std::pair<uint64_t, uint64_t>> items;
    items.reserve(static_cast<size_t>(seg.records));
    uint64_t valid_end = 0, last_id = 0, records = 0;
    auto collect = [&items](const StoredMessage& m, uint64_t off) { items.emplace_back(m.msg_id, off); };
    Status st = scan_fd(seg.fd.get(), collect, valid_end, last_id, records);
    if (!st) return st;

    std::string buf;
    buf.resize(items.size() * kIdxEntrySize);
    for (size_t i = 0; i < items.size(); ++i) {
        Writer w;
        w.u64(items[i].first);
        w.u64(items[i].second);
        std::memcpy(&buf[i * kIdxEntrySize], w.data().data(), kIdxEntrySize);
    }
    CHAT_TRY(fs::ftruncate_to(seg.idx_fd.get(), 0));
    if (!buf.empty()) CHAT_TRY(fs::pwrite_all(seg.idx_fd.get(), buf.data(), buf.size(), 0));
    CHAT_TRY(fs::fdatasync(seg.idx_fd.get()));
    seg.records = items.size();
    seg.bytes = valid_end;
    seg.last_id = items.empty() ? (seg.first_id ? seg.first_id - 1 : 0) : items.back().first;
    meta_dirty_ = true;
    return save_meta_locked();
}

Status RoomStore::scan_all_locked() {
    std::vector<std::string> names;
    CHAT_TRY(fs::list_dir(dir_, names));

    // 保留名称，重新按文件名构建段
    std::vector<uint64_t> first_ids;
    for (const auto& n : names) {
        if (n.size() < 5) continue;
        if (n.compare(n.size() - 4, 4, ".seg") != 0) continue;
        const uint64_t id = std::strtoull(n.substr(0, n.size() - 4).c_str(), nullptr, 10);
        if (id > 0) first_ids.push_back(id);
    }
    std::sort(first_ids.begin(), first_ids.end());

    segments_.clear();
    last_msg_id_ = 0;
    msg_count_ = 0;
    total_bytes_ = 0;
    for (uint64_t fid : first_ids) {
        Segment seg;
        Status st = open_segment_locked(seg, fid, false);
        if (!st) return st;
        st = scan_one_locked(seg, true);
        if (!st) return st;
        if (seg.last_id >= seg.first_id) {
            last_msg_id_ = std::max(last_msg_id_, seg.last_id);
            msg_count_ += seg.records;
            total_bytes_ += seg.bytes;
        }
        segments_.push_back(std::move(seg));
        // 只保留最后一段的 fd 打开
        segments_.back().fd.reset();
        segments_.back().idx_fd.reset();
    }
    if (!segments_.empty()) {
        CHAT_TRY(open_segment_locked(segments_.back(), segments_.back().first_id, true));
        // 重新拿到 fd 后不需要再扫描
    }
    meta_dirty_ = true;
    CHAT_TRY(save_meta_locked());
    LOG_INFO("storage[{}]: scanned {} segments, last_id={}, msgs={}", name_, segments_.size(), last_msg_id_,
             msg_count_);
    return Status::success();
}

Status RoomStore::roll_segment_locked() {
    if (!segments_.empty()) {
        Segment& last = segments_.back();
        if (dirty_) {
            if (last.fd.valid()) (void)fs::fdatasync(last.fd.get());
            if (last.idx_fd.valid()) (void)fs::fdatasync(last.idx_fd.get());
            dirty_ = false;
        }
        last.fd.reset();
        last.idx_fd.reset();
    }
    Segment seg;
    seg.first_id = last_msg_id_ + 1;
    CHAT_TRY(open_segment_locked(seg, seg.first_id, true));
    seg.last_id = seg.first_id - 1;
    segments_.push_back(std::move(seg));
    meta_dirty_ = true;
    (void)save_meta_locked();
    return Status::success();
}

Status RoomStore::append(StoredMessage& m) {
    std::lock_guard<std::mutex> lk(mu_);
    if (segments_.empty() || !segments_.back().fd.valid()) {
        CHAT_TRY(roll_segment_locked());
    }

    m.msg_id = last_msg_id_ + 1;
    const std::string rec = encode_record(m);

    // 磁盘保护：超限拒绝写入，而不是删旧数据或静默失败
    if (max_bytes_ > 0 && total_bytes_ + rec.size() > max_bytes_) {
        return Status::fail(Err::storage_error,
                            fmt("room {} data limit reached ({} MB / {} MB)", name_, total_bytes_ >> 20,
                                max_bytes_ >> 20));
    }

    // 段满则滚动；滚动不改变 msg_id，编码结果可直接使用
    if (segments_.back().records > 0 && segments_.back().bytes + rec.size() > segment_bytes_) {
        CHAT_TRY(roll_segment_locked());
    }
    Segment& target = segments_.back();
    CHAT_TRY(fs::pwrite_all(target.fd.get(), rec.data(), rec.size(), target.bytes));

    Writer k;
    k.u64(m.msg_id);
    k.u64(target.bytes);
    CHAT_TRY(fs::pwrite_all(target.idx_fd.get(), k.data().data(), kIdxEntrySize, target.records * kIdxEntrySize));

    target.bytes += rec.size();
    target.records += 1;
    target.last_id = m.msg_id;
    total_bytes_ += rec.size();
    last_msg_id_ = m.msg_id;
    msg_count_ += 1;
    dirty_ = true;
    meta_dirty_ = true;
    return Status::success();
}

Status RoomStore::sync() {
    std::lock_guard<std::mutex> lk(mu_);
    if (dirty_) {
        for (auto& seg : segments_) {
            if (seg.fd.valid()) CHAT_TRY(fs::fdatasync(seg.fd.get()));
            if (seg.idx_fd.valid()) CHAT_TRY(fs::fdatasync(seg.idx_fd.get()));
        }
        dirty_ = false;
    }
    // meta 只需周期性刷新（崩溃后可由扫描修正）
    static thread_local uint64_t last_meta_ms = 0;
    const uint64_t now = steady_ms();
    if (meta_dirty_ && now - last_meta_ms > 1000) {
        last_meta_ms = now;
        (void)save_meta_locked();
    }
    return Status::success();
}

bool RoomStore::dirty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dirty_;
}

uint64_t RoomStore::last_msg_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_msg_id_;
}

uint64_t RoomStore::msg_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return msg_count_;
}

Status RoomStore::idx_lower_bound_locked(Segment& seg, uint64_t target, uint64_t& pos) {
    // 返回第一条 msg_id >= target 的序号
    uint64_t lo = 0, hi = seg.records;
    uint8_t buf[kIdxEntrySize];
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        CHAT_TRY(fs::pread_exact(seg.idx_fd.get(), buf, kIdxEntrySize, mid * kIdxEntrySize));
        const uint64_t id = be64(buf);
        if (id < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    pos = lo;
    return Status::success();
}

Status RoomStore::read_record_at_locked(Segment& seg, uint64_t offset, StoredMessage& m, uint64_t& next_off) {
    uint8_t hdr[kRecHeaderSize];
    Status st = fs::pread_exact(seg.fd.get(), hdr, kRecHeaderSize, offset);
    if (!st) return st;
    if (be16(hdr) != kRecMagic || hdr[2] != kRecVersion || hdr[3] != kRecTypeMsg) {
        return Status::fail(Err::storage_error, "bad record header");
    }
    const uint32_t body_len = be32(hdr + 4);
    if (body_len > kMaxPayload) return Status::fail(Err::storage_error, "record too large");

    std::string rec;
    rec.resize(kRecHeaderSize + body_len);
    std::memcpy(&rec[0], hdr, kRecHeaderSize);
    st = fs::pread_exact(seg.fd.get(), &rec[kRecHeaderSize], body_len, offset + kRecHeaderSize);
    if (!st) return st;
    if (!decode_record(rec, m)) return Status::fail(Err::storage_error, "record crc mismatch");
    next_off = offset + kRecHeaderSize + body_len;
    return Status::success();
}

Status RoomStore::read_before(uint64_t before_id, uint32_t limit, std::vector<StoredMessage>& out) {
    out.clear();
    if (limit == 0) return Status::success();
    std::lock_guard<std::mutex> lk(mu_);
    if (segments_.empty()) return Status::success();

    const bool latest = (before_id == 0);
    uint64_t target = latest ? UINT64_MAX : before_id;

    // 定位到可能包含 < target 的最后一段；latest 时直接从最后一段开始
    size_t seg_idx = segments_.size();
    if (latest) {
        seg_idx = segments_.size() - 1;
    } else {
        for (size_t i = 0; i < segments_.size(); ++i) {
            const Segment& s = segments_[i];
            if (s.records == 0) continue;
            if (s.last_id + 1 >= target) {
                seg_idx = i;
                break;
            }
        }
    }
    if (seg_idx == segments_.size()) return Status::success();

    std::vector<StoredMessage> rev;
    rev.reserve(limit);
    for (int64_t i = static_cast<int64_t>(seg_idx); i >= 0 && rev.size() < limit; --i) {
        Segment& seg = segments_[static_cast<size_t>(i)];
        if (seg.records == 0) continue;
        if (!seg.fd.valid()) {
            Status st = open_segment_locked(seg, seg.first_id, false);
            if (!st) return st;
        }
        if (!seg.idx_fd.valid()) {
            int ifd = -1;
            Status st = fs::open_rw(seg.idx_path, true, ifd);
            if (!st) return st;
            seg.idx_fd.reset(ifd);
        }
        uint64_t pos = 0;
        Status st = idx_lower_bound_locked(seg, target, pos);
        if (!st) return st;
        while (pos > 0 && rev.size() < limit) {
            --pos;
            uint8_t entry[kIdxEntrySize];
            st = fs::pread_exact(seg.idx_fd.get(), entry, kIdxEntrySize, pos * kIdxEntrySize);
            if (!st) return st;
            const uint64_t offset = be64(entry + 8);
            StoredMessage m;
            uint64_t next = 0;
            st = read_record_at_locked(seg, offset, m, next);
            if (!st) return st;
            rev.push_back(std::move(m));
        }
        target = seg.first_id;
    }

    out.assign(std::make_move_iterator(rev.rbegin()), std::make_move_iterator(rev.rend()));
    return Status::success();
}

Status RoomStore::read_latest(uint32_t limit, std::vector<StoredMessage>& out) {
    uint64_t last = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        last = last_msg_id_;
    }
    return read_before(last + 1, limit, out);
}

// ------------------------------------------------------------------ StorageManager
StorageManager::~StorageManager() { stop(); }

Status StorageManager::init(const std::string& data_dir, uint32_t segment_bytes, const std::string& sync_mode,
                            bool repair, uint64_t max_room_bytes) {
    data_dir_ = data_dir;
    segment_bytes_ = segment_bytes;
    sync_mode_ = sync_mode;
    repair_ = repair;
    max_room_bytes_ = max_room_bytes;
    const std::string rooms_dir = fs::join(data_dir_, "rooms");
    CHAT_TRY(fs::mkdirs(rooms_dir));

    // 只记名字，不打开 fd：房间仍然懒加载，但 /rooms 能列出磁盘上已存在的房间
    std::vector<std::string> names;
    CHAT_TRY(fs::list_dir(rooms_dir, names));
    for (const auto& n : names) {
        if (fs::exists(fs::join(rooms_dir, n))) known_rooms_.push_back(n);
    }

    LOG_INFO("storage: data dir {}, {} existing room(s), sync={}, segment={}MB", data_dir_, known_rooms_.size(),
             sync_mode_, segment_bytes_ / (1024 * 1024));
    return Status::success();
}

RoomStore* StorageManager::room(const std::string& name) {
    std::lock_guard<std::mutex> lk(rooms_mu_);
    auto it = rooms_.find(name);
    return it == rooms_.end() ? nullptr : it->second.get();
}

Status StorageManager::open_room(const std::string& name, RoomStore** out) {
    {
        std::lock_guard<std::mutex> lk(rooms_mu_);
        auto it = rooms_.find(name);
        if (it != rooms_.end()) {
            if (out) *out = it->second.get();
            return Status::success();
        }
    }
    auto ptr = std::make_unique<RoomStore>();
    Status st =
        ptr->open(name, fs::join(fs::join(data_dir_, "rooms"), name), segment_bytes_, repair_, max_room_bytes_);
    if (!st) return st;
    RoomStore* raw = ptr.get();
    {
        std::lock_guard<std::mutex> lk(rooms_mu_);
        rooms_[name] = std::move(ptr);
        if (std::find(known_rooms_.begin(), known_rooms_.end(), name) == known_rooms_.end()) {
            known_rooms_.push_back(name);
        }
    }
    if (out) *out = raw;
    return Status::success();
}

bool StorageManager::has_room(const std::string& name) const {
    std::lock_guard<std::mutex> lk(rooms_mu_);
    if (rooms_.find(name) != rooms_.end()) return true;
    return std::find(known_rooms_.begin(), known_rooms_.end(), name) != known_rooms_.end();
}

std::vector<std::string> StorageManager::list_rooms() const {
    std::lock_guard<std::mutex> lk(rooms_mu_);
    std::vector<std::string> out = known_rooms_;
    for (const auto& kv : rooms_) {
        if (std::find(out.begin(), out.end(), kv.first) == out.end()) out.push_back(kv.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Status StorageManager::append(const std::string& name, StoredMessage& m, uint64_t tag) {
    RoomStore* rs = room(name);
    if (!rs) return Status::fail(Err::room_not_found, name);
    CHAT_TRY(rs->append(m));  // 分配 msg_id 并写 page cache（微秒级）
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push_back(PendingItem{name, m, tag});
    }
    cv_.notify_one();
    return Status::success();
}

void StorageManager::set_commit_callback(CommitCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    cb_ = std::move(cb);
}

void StorageManager::start_flusher() {
    stop_ = false;
    flusher_ = std::thread([this] { flush_loop(); });
}

void StorageManager::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (flusher_.joinable()) flusher_.join();
}

void StorageManager::flush_loop() {
    const int wait_ms = sync_mode_ == "always" ? 1 : 2;
    for (;;) {
        std::deque<PendingItem> batch;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(wait_ms), [this] { return stop_ || !pending_.empty(); });
            if (pending_.empty()) {
                if (stop_) break;
                continue;
            }
            batch.swap(pending_);
        }

        // 1) 批量 fsync（group commit：一批共享一次刷盘）
        if (sync_mode_ != "never") {
            std::vector<std::string> rooms;
            for (const auto& it : batch) {
                if (std::find(rooms.begin(), rooms.end(), it.room) == rooms.end()) rooms.push_back(it.room);
            }
            for (const auto& r : rooms) {
                RoomStore* rs = room(r);
                if (!rs) continue;
                Status st = rs->sync();
                if (!st) LOG_ERROR("storage: sync {} failed: {}", r, err_text(st.e, st.msg));
            }
        }

        // 2) 回调：此刻已落盘，可以安全地回 ACK 与广播（批内顺序 = msg_id 递增）
        CommitCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = cb_;
        }
        if (cb) {
            for (const auto& it : batch) cb(it.room, it.msg, it.tag);
        }
    }
}

}  // namespace chat
