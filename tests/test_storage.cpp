#include <cstdlib>
#include <unistd.h>

#include "chat/fs.h"
#include "storage.h"
#include "test_framework.h"

using namespace chat;

namespace {

std::string temp_dir(const char* tag) {
    return std::string("/tmp/chat_test_") + tag + "_" + std::to_string(::getpid());
}

void rmrf(const std::string& dir) {
    const int rc = std::system(("rm -rf '" + dir + "'").c_str());
    (void)rc;
}

std::string find_seg(const std::string& dir, size_t index) {
    std::vector<std::string> names;
    (void)fs::list_dir(dir, names);
    std::vector<std::string> segs;
    for (const auto& n : names) {
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".seg") == 0) segs.push_back(n);
    }
    if (index >= segs.size()) return {};
    return fs::join(dir, segs[index]);
}

}  // namespace

TEST_CASE(storage_append_read_restart) {
    const std::string dir = temp_dir("append");
    rmrf(dir);
    {
        RoomStore rs;
        CHECK(rs.open("room", dir, 1u << 20, false).ok());
        for (int i = 0; i < 500; ++i) {
            StoredMessage m;
            m.from = "alice";
            m.ts_ms = 1000 + static_cast<uint64_t>(i);
            m.text = "msg " + std::to_string(i);
            CHECK(rs.append(m).ok());
            CHECK_EQ(m.msg_id, static_cast<uint64_t>(i + 1));
        }
        CHECK_EQ(rs.last_msg_id(), 500u);
        CHECK_EQ(rs.msg_count(), 500u);

        std::vector<StoredMessage> out;
        CHECK(rs.read_before(0, 10, out).ok());
        CHECK_EQ(out.size(), 10u);
        CHECK_EQ(out.front().msg_id, 491u);
        CHECK_EQ(out.back().msg_id, 500u);
        CHECK_EQ(out.back().text, std::string("msg 499"));
        CHECK(rs.sync().ok());
    }
    {
        RoomStore rs;
        CHECK(rs.open("room", dir, 1u << 20, false).ok());
        CHECK_EQ(rs.last_msg_id(), 500u);
        std::vector<StoredMessage> out;
        CHECK(rs.read_before(100, 5, out).ok());
        CHECK_EQ(out.size(), 5u);
        CHECK_EQ(out.back().msg_id, 99u);
        CHECK_EQ(out.front().msg_id, 95u);
    }
    rmrf(dir);
}

TEST_CASE(storage_segment_rotation) {
    const std::string dir = temp_dir("rotate");
    rmrf(dir);
    {
        RoomStore rs;
        CHECK(rs.open("room", dir, 400, false).ok());  // 很小的段，强制滚动
        for (int i = 0; i < 100; ++i) {
            StoredMessage m;
            m.from = "bob";
            m.ts_ms = static_cast<uint64_t>(i);
            m.text = "padding padding padding " + std::to_string(i);
            CHECK(rs.append(m).ok());
        }
        CHECK_EQ(rs.last_msg_id(), 100u);

        std::vector<std::string> names;
        (void)fs::list_dir(dir, names);
        int segs = 0;
        for (const auto& n : names) {
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".seg") == 0) ++segs;
        }
        CHECK(segs >= 3);
    }
    {
        RoomStore rs;
        CHECK(rs.open("room", dir, 400, false).ok());
        CHECK_EQ(rs.last_msg_id(), 100u);
        CHECK_EQ(rs.msg_count(), 100u);
        // 跨段读取
        std::vector<StoredMessage> out;
        CHECK(rs.read_before(51, 10, out).ok());
        CHECK_EQ(out.size(), 10u);
        CHECK_EQ(out.front().msg_id, 41u);
        CHECK_EQ(out.back().msg_id, 50u);
    }
    rmrf(dir);
}

TEST_CASE(storage_tail_truncation_recovery) {
    const std::string dir = temp_dir("torn");
    rmrf(dir);
    {
        RoomStore rs;
        CHECK(rs.open("room", dir, 1u << 20, false).ok());
        for (int i = 0; i < 10; ++i) {
            StoredMessage m;
            m.from = "c";
            m.text = "line " + std::to_string(i);
            CHECK(rs.append(m).ok());
        }
        CHECK(rs.sync().ok());
    }
    // 模拟掉电：截掉最后 15 字节（最后一条记录不完整）
    const std::string seg = find_seg(dir, 0);
    CHECK(!seg.empty());
    uint64_t size = 0;
    {
        int fd = -1;
        CHECK(fs::open_ro(seg, fd).ok());
        CHECK(fs::file_size(fd, size).ok());
        ::close(fd);
    }
    CHECK(size > 20);
    {
        int fd = -1;
        CHECK(fs::open_rw(seg, false, fd).ok());
        CHECK(fs::ftruncate_to(fd, size - 15).ok());
        ::close(fd);
    }
    {
        RoomStore rs;
        CHECK(rs.open("room", dir, 1u << 20, false).ok());
        CHECK_EQ(rs.last_msg_id(), 9u);  // 只丢最后一条
        std::vector<StoredMessage> out;
        CHECK(rs.read_before(0, 100, out).ok());
        CHECK_EQ(out.size(), 9u);
        CHECK_EQ(out.back().text, std::string("line 8"));
    }
    rmrf(dir);
}

TEST_CASE(storage_index_rebuild) {
    const std::string dir = temp_dir("idx");
    rmrf(dir);
    {
        RoomStore rs;
        CHECK(rs.open("room", dir, 1u << 20, false).ok());
        for (int i = 0; i < 50; ++i) {
            StoredMessage m;
            m.from = "d";
            m.text = "x" + std::to_string(i);
            CHECK(rs.append(m).ok());
        }
    }
    // 删掉索引文件，启动时应自动重建
    const std::string seg = find_seg(dir, 0);
    const std::string idx = seg.substr(0, seg.size() - 4) + ".idx";
    CHECK(fs::remove_file(idx).ok());
    {
        RoomStore rs;
        CHECK(rs.open("room", dir, 1u << 20, false).ok());
        std::vector<StoredMessage> out;
        CHECK(rs.read_before(11, 10, out).ok());
        CHECK_EQ(out.size(), 10u);
        CHECK_EQ(out.back().msg_id, 10u);
    }
    rmrf(dir);
}

TEST_CASE(storage_concurrent_append) {
    const std::string dir = temp_dir("conc");
    rmrf(dir);
    RoomStore rs;
    CHECK(rs.open("room", dir, 1u << 20, false).ok());

    constexpr int kThreads = 8;
    constexpr int kPer = 200;
    std::vector<std::thread> ths;
    std::atomic<int> ok{0};
    for (int t = 0; t < kThreads; ++t) {
        ths.emplace_back([&rs, &ok, t] {
            for (int i = 0; i < kPer; ++i) {
                StoredMessage m;
                m.from = "t" + std::to_string(t);
                m.text = "m" + std::to_string(i);
                if (rs.append(m)) ok.fetch_add(1);
            }
        });
    }
    for (auto& t : ths) t.join();
    CHECK_EQ(ok.load(), kThreads * kPer);
    CHECK_EQ(rs.last_msg_id(), static_cast<uint64_t>(kThreads * kPer));
    CHECK_EQ(rs.msg_count(), static_cast<uint64_t>(kThreads * kPer));
    rmrf(dir);
}
