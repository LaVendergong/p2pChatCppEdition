#include "chat/fs.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace chat::fs {

Status mkdirs(const std::string& path) {
    if (path.empty()) return Status::success();
    std::string cur;
    size_t i = 0;
    while (i <= path.size()) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && cur != "/") {
                if (::mkdir(cur.c_str(), 0755) < 0 && errno != EEXIST) return status_errno(("mkdir " + cur).c_str());
            }
        }
        if (i < path.size()) cur.push_back(path[i]);
        ++i;
    }
    return Status::success();
}

Status open_rw(const std::string& path, bool create, int& fd) {
    int flags = O_RDWR | O_CLOEXEC;
    if (create) flags |= O_CREAT;
    fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) return status_errno(("open " + path).c_str());
    return Status::success();
}

Status open_ro(const std::string& path, int& fd) {
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return status_errno(("open " + path).c_str());
    return Status::success();
}

Status pwrite_all(int fd, const void* data, size_t n, uint64_t off) {
    const char* p = static_cast<const char*>(data);
    size_t done = 0;
    while (done < n) {
        const ssize_t r = ::pwrite(fd, p + done, n - done, static_cast<off_t>(off + done));
        if (r > 0) {
            done += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        return status_errno("pwrite");
    }
    return Status::success();
}

Status pread_exact(int fd, void* data, size_t n, uint64_t off) {
    char* p = static_cast<char*>(data);
    size_t done = 0;
    while (done < n) {
        const ssize_t r = ::pread(fd, p + done, n - done, static_cast<off_t>(off + done));
        if (r > 0) {
            done += static_cast<size_t>(r);
            continue;
        }
        if (r == 0) return Status::fail(Err::eof, "unexpected end of file");
        if (errno == EINTR) continue;
        return status_errno("pread");
    }
    return Status::success();
}

Status pread_some(int fd, void* data, size_t n, uint64_t off, size_t& got) {
    got = 0;
    for (;;) {
        const ssize_t r = ::pread(fd, data, n, static_cast<off_t>(off));
        if (r >= 0) {
            got = static_cast<size_t>(r);
            return Status::success();
        }
        if (errno == EINTR) continue;
        return status_errno("pread");
    }
}

Status fdatasync(int fd) {
    if (::fdatasync(fd) < 0) return status_errno("fdatasync");
    return Status::success();
}

Status fsync(int fd) {
    if (::fsync(fd) < 0) return status_errno("fsync");
    return Status::success();
}

Status ftruncate_to(int fd, uint64_t size) {
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0) return status_errno("ftruncate");
    return Status::success();
}

Status file_size(int fd, uint64_t& size) {
    struct stat st{};
    if (::fstat(fd, &st) < 0) return status_errno("fstat");
    size = static_cast<uint64_t>(st.st_size);
    return Status::success();
}

Status atomic_write_file(const std::string& path, std::string_view data) {
    const std::string tmp = path + ".tmp." + std::to_string(::getpid());
    int fd = -1;
    Status st = open_rw(tmp, true, fd);
    if (!st) return st;
    st = pwrite_all(fd, data.data(), data.size(), 0);
    if (st) st = fsync(fd);
    ::close(fd);
    if (!st) {
        ::unlink(tmp.c_str());
        return st;
    }
    if (::rename(tmp.c_str(), path.c_str()) < 0) {
        st = status_errno("rename");
        ::unlink(tmp.c_str());
        return st;
    }
    // 目录 fsync：保证 rename 本身落盘
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return Status::success();
}

Status read_file(const std::string& path, std::string& out) {
    int fd = -1;
    Status st = open_ro(path, fd);
    if (!st) return st;
    out.clear();
    char buf[65536];
    for (;;) {
        const ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r > 0) {
            out.append(buf, static_cast<size_t>(r));
            continue;
        }
        if (r == 0) break;
        if (errno == EINTR) continue;
        st = status_errno("read");
        ::close(fd);
        return st;
    }
    ::close(fd);
    return Status::success();
}

Status remove_file(const std::string& path) {
    if (::unlink(path.c_str()) < 0 && errno != ENOENT) return status_errno("unlink");
    return Status::success();
}

Status list_dir(const std::string& path, std::vector<std::string>& names) {
    names.clear();
    DIR* d = ::opendir(path.c_str());
    if (!d) {
        if (errno == ENOENT) return Status::success();
        return status_errno(("opendir " + path).c_str());
    }
    struct dirent* e = nullptr;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        names.push_back(n);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    return Status::success();
}

bool exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

}  // namespace chat::fs
