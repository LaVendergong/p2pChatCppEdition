// 文件系统原语：pread/pwrite 全量、fdatasync、原子替换写、目录遍历。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chat/error.h"

namespace chat::fs {

Status mkdirs(const std::string& path);

// 打开（不存在则创建），O_CLOEXEC 总是开启
Status open_rw(const std::string& path, bool create, int& fd);
Status open_ro(const std::string& path, int& fd);

// 短读短写重试，保证 n 字节
Status pwrite_all(int fd, const void* data, size_t n, uint64_t off);
Status pread_exact(int fd, void* data, size_t n, uint64_t off);
// 读最多 n 字节，got 返回实际字节数（0 表示到文件尾）
Status pread_some(int fd, void* data, size_t n, uint64_t off, size_t& got);

Status fdatasync(int fd);
Status fsync(int fd);
Status ftruncate_to(int fd, uint64_t size);
Status file_size(int fd, uint64_t& size);

// 临时文件 + fsync + rename，保证读者要么看到旧内容要么看到新内容
Status atomic_write_file(const std::string& path, std::string_view data);
Status read_file(const std::string& path, std::string& out);
Status remove_file(const std::string& path);

// 只返回文件名（不含路径），已排序
Status list_dir(const std::string& path, std::vector<std::string>& names);

bool exists(const std::string& path);
std::string join(const std::string& a, const std::string& b);

}  // namespace chat::fs
