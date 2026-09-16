// SPDX-License-Identifier: MIT
//
// A scratch directory that removes itself.
//
// The storage and configuration tests write real files, on purpose: both
// classes are mostly about what happens at the filesystem boundary (atomic
// rename, fsync, refusing a path that escapes the namespace), and a test that
// mocked the filesystem would exercise none of it.

#pragma once

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace medtest {

inline void removeTree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr) {
        for (struct dirent* entry = ::readdir(dir); entry != nullptr;
             entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            removeTree(path + "/" + name);
        }
        ::closedir(dir);
        ::rmdir(path.c_str());
        return;
    }
    ::unlink(path.c_str());
}

class TempDir {
public:
    TempDir() {
        std::string templated = "/tmp/medframework-test-XXXXXX";
        std::vector<char> buffer(templated.begin(), templated.end());
        buffer.push_back('\0');
        const char* made = ::mkdtemp(buffer.data());
        path_ = (made != nullptr) ? made : std::string();
    }

    ~TempDir() {
        if (!path_.empty()) {
            removeTree(path_);
        }
    }

    bool valid() const { return !path_.empty(); }
    const std::string& path() const { return path_; }
    std::string file(const std::string& name) const { return path_ + "/" + name; }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

private:
    std::string path_;
};

}  // namespace medtest
