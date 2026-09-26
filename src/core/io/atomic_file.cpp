// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/atomic_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "core/io/io_error.hpp"

namespace rl::io {

namespace {

std::string errstr(int e) { return std::strerror(e); }

std::string dir_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string base_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool write_all(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        const ssize_t k = ::write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

}  // namespace

void atomic_write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    if (path.empty()) throw IoError("save: empty file name");
    const std::string dir = dir_of(path);
    std::string tmpl = dir + "/." + base_of(path) + ".rl-save-XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) throw IoError("cannot save '" + path + "': cannot create a temporary file in '" + dir + "': " +
                              errstr(errno));
    const std::string tmp(name.data());
    auto fail = [&](const std::string& why) {
        const int e = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        throw IoError("cannot save '" + path + "': " + why + (e ? ": " + errstr(e) : std::string()));
    };

    // Permissions: those of the file being replaced, else 0666 & ~umask (what open(2) would give).
    mode_t mode;
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            errno = 0;
            fail("the target exists and is not a regular file");
        }
        mode = st.st_mode & 07777;
    } else {
        const mode_t um = ::umask(0);
        ::umask(um);
        mode = 0666 & ~um;
    }
    if (::fchmod(fd, mode) != 0) fail("cannot set permissions");
    if (!write_all(fd, bytes.data(), bytes.size())) fail("write failed");
    if (::fsync(fd) != 0) fail("fsync failed");
    if (::close(fd) != 0) {
        const int e = errno;
        ::unlink(tmp.c_str());
        throw IoError("cannot save '" + path + "': close failed: " + errstr(e));
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int e = errno;
        ::unlink(tmp.c_str());
        throw IoError("cannot save '" + path + "': rename failed: " + errstr(e));
    }
    // Make the rename itself durable. A failure here cannot be undone and the new file is in place,
    // so it is not reported as a failed save.
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
}

std::vector<uint8_t> read_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw IoError("cannot open '" + path + "': " + errstr(errno));
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        throw IoError("cannot open '" + path + "': not a regular file");
    }
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(st.st_size));
    uint8_t buf[1 << 16];
    for (;;) {
        const ssize_t k = ::read(fd, buf, sizeof buf);
        if (k < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            throw IoError("cannot read '" + path + "': " + errstr(e));
        }
        if (k == 0) break;
        out.insert(out.end(), buf, buf + k);
    }
    ::close(fd);
    return out;
}

}  // namespace rl::io
