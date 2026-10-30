// cht-dump：离线解析 .seg 文件，输出每条的 id / 时间 / 发送者 / 文本，并校验 CRC。
#include <cstdio>
#include <string>

#include "chat/log.h"
#include "chat/time_util.h"
#include "storage.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cht-dump <file.seg> [more.seg ...]\n");
        return 2;
    }
    chat::log_init(false, "", chat::LogLevel::warn);

    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        uint64_t valid_end = 0;
        uint64_t count = 0;
        chat::Status st = chat::scan_segment_file(
            argv[i],
            [&count](const chat::StoredMessage& m) {
                ++count;
                std::printf("%llu\t%s\t%s\t%s\n", static_cast<unsigned long long>(m.msg_id),
                            chat::fmt_datetime(m.ts_ms).c_str(), m.from.c_str(), m.text.c_str());
            },
            valid_end);
        if (!st) {
            std::fprintf(stderr, "%s: %s\n", argv[i], chat::err_text(st.e, st.msg).c_str());
            rc = 1;
            continue;
        }
        std::fprintf(stderr, "%s: %llu records, valid bytes=%llu\n", argv[i],
                     static_cast<unsigned long long>(count), static_cast<unsigned long long>(valid_end));
    }
    chat::log_shutdown();
    return rc;
}
