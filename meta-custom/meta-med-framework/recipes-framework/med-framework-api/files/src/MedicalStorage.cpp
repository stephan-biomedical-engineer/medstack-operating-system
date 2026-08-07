// SPDX-License-Identifier: MIT

#include "MedicalStorage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace med {
namespace {

Status statusFromErrno(int error) {
    switch (error) {
        case ENOENT:  return Status::NotFound;
        case EACCES:
        case EPERM:
        case EROFS:   return Status::PermissionDenied;
        case EINVAL:
        case ENAMETOOLONG: return Status::InvalidArgument;
        case ENOSPC:
        case EDQUOT:  return Status::Unavailable;
        default:      return Status::IoError;
    }
}

/// Reject anything that could address storage outside the namespace root.
/// Checked lexically *before* any syscall, because the interesting attacks
/// (a symlink planted under /data) are races against the filesystem.
bool isSafeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 1024) {
        return false;
    }
    if (path.front() == '/') {
        return false;
    }
    if (path.find('\0') != std::string::npos) {
        return false;
    }

    std::string::size_type start = 0;
    while (start <= path.size()) {
        const std::string::size_type slash = path.find('/', start);
        const std::string component =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);

        if (component == ".." || component == ".") {
            return false;
        }
        // An empty component means "//" or a trailing slash: both name a
        // directory, and this API addresses records.
        if (component.empty()) {
            return false;
        }

        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool makeDirectories(const std::string& path, mode_t mode) {
    std::string partial;
    partial.reserve(path.size());

    for (std::string::size_type i = 0; i < path.size(); ++i) {
        partial.push_back(path[i]);
        const bool last = (i + 1 == path.size());
        if (path[i] != '/' && !last) {
            continue;
        }
        if (partial == "/") {
            continue;
        }
        const std::string component = (path[i] == '/') ? partial.substr(0, partial.size() - 1)
                                                       : partial;
        if (::mkdir(component.c_str(), mode) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

/// True when `path` is the root of a mount: its device differs from its
/// parent's. Cheaper and more portable than parsing /proc/self/mountinfo, and
/// it answers the only question that matters - is /data a volume of its own,
/// or did we silently fall back to writing into the rootfs?
bool isMountPoint(const std::string& path) {
    struct stat self = {};
    struct stat parent = {};
    if (::stat(path.c_str(), &self) != 0) {
        return false;
    }
    if (::stat((path + "/..").c_str(), &parent) != 0) {
        return false;
    }
    return self.st_dev != parent.st_dev;
}

/// True when the block device backing `path` is a device-mapper target whose
/// UUID marks it as dm-crypt. This is what refuses to write patient data to a
/// plaintext partition.
bool hasEncryptedBacking(const std::string& path) {
    struct stat info = {};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }

    char sysfsPath[128];
    std::snprintf(sysfsPath, sizeof(sysfsPath), "/sys/dev/block/%u:%u/dm/uuid",
                  major(info.st_dev), minor(info.st_dev));

    const int fd = ::open(sysfsPath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;  // not a device-mapper device at all
    }

    char uuid[64] = {0};
    const ssize_t n = ::read(fd, uuid, sizeof(uuid) - 1);
    ::close(fd);
    if (n <= 0) {
        return false;
    }

    return std::strncmp(uuid, "CRYPT-", 6) == 0;
}

Status writeAll(int fd, const void* data, std::size_t length) {
    const char* cursor = static_cast<const char*>(data);
    std::size_t remaining = length;
    while (remaining > 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return statusFromErrno(errno);
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return Status::Ok;
}

/// fsync the directory holding `path`, so the rename itself is durable and not
/// only the file contents.
void syncParentDirectory(const std::string& path) {
    const std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return;
    }
    const std::string directory = (slash == 0) ? "/" : path.substr(0, slash);
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

MedicalStorage::MedicalStorage(std::string basePath, StorageInfo info)
    : basePath_(std::move(basePath)), info_(std::move(info)) {}

MedicalStorage::~MedicalStorage() = default;

Result<std::unique_ptr<MedicalStorage>> MedicalStorage::open(const StorageConfig& config) {
    using StorageResult = Result<std::unique_ptr<MedicalStorage>>;

    if (config.root.empty() || config.root.front() != '/') {
        return StorageResult::fail(Status::InvalidArgument,
                                   "storage root must be an absolute path");
    }

    struct stat rootInfo = {};
    if (::stat(config.root.c_str(), &rootInfo) != 0) {
        return StorageResult::fail(Status::Unavailable,
                                   "storage root " + config.root + " does not exist: " +
                                       std::strerror(errno));
    }
    if (!S_ISDIR(rootInfo.st_mode)) {
        return StorageResult::fail(Status::InvalidArgument,
                                   "storage root " + config.root + " is not a directory");
    }

    StorageInfo info;
    info.root = config.root;
    info.mounted = isMountPoint(config.root);
    info.encrypted = hasEncryptedBacking(config.root);

    if (config.requireEncryptedBacking && !info.encrypted) {
        return StorageResult::fail(
            Status::PermissionDenied,
            "refusing to store medical records on the unencrypted backing of " +
                config.root +
                " (set StorageConfig::requireEncryptedBacking=false only on "
                "development images, and audit the decision)");
    }

    std::string basePath = config.root;
    if (!config.nameSpace.empty()) {
        if (!isSafeRelativePath(config.nameSpace)) {
            return StorageResult::fail(Status::InvalidArgument,
                                       "invalid namespace: " + config.nameSpace);
        }
        basePath += "/" + config.nameSpace;
    }

    if (!makeDirectories(basePath, 0700)) {
        return StorageResult::fail(statusFromErrno(errno),
                                   "cannot create " + basePath + ": " + std::strerror(errno));
    }

    struct statvfs vfs = {};
    if (::statvfs(basePath.c_str(), &vfs) == 0) {
        info.totalBytes = static_cast<std::uint64_t>(vfs.f_blocks) * vfs.f_frsize;
        info.availableBytes = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    }

    return StorageResult::ok(std::unique_ptr<MedicalStorage>(
        new MedicalStorage(std::move(basePath), std::move(info))));
}

Status MedicalStorage::write(const std::string& relativePath, const void* data,
                             std::size_t length) {
    if (!isSafeRelativePath(relativePath)) {
        return Status::InvalidArgument;
    }
    if (data == nullptr && length != 0) {
        return Status::InvalidArgument;
    }

    const std::string target = basePath_ + "/" + relativePath;

    const std::string::size_type slash = target.find_last_of('/');
    if (slash != std::string::npos && !makeDirectories(target.substr(0, slash), 0700)) {
        return statusFromErrno(errno);
    }

    // Atomic replace: a reader either sees the previous record or the new one.
    std::string temporary = target + ".tmpXXXXXX";
    const int fd = ::mkstemp(&temporary[0]);
    if (fd < 0) {
        return statusFromErrno(errno);
    }

    Status status = writeAll(fd, data, length);
    if (status == Status::Ok && ::fchmod(fd, 0600) != 0) {
        status = statusFromErrno(errno);
    }
    if (status == Status::Ok && ::fsync(fd) != 0) {
        status = statusFromErrno(errno);
    }
    ::close(fd);

    if (status != Status::Ok) {
        ::unlink(temporary.c_str());
        return status;
    }

    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        const int saved = errno;
        ::unlink(temporary.c_str());
        return statusFromErrno(saved);
    }

    syncParentDirectory(target);
    return Status::Ok;
}

Status MedicalStorage::writeString(const std::string& relativePath,
                                   const std::string& content) {
    return write(relativePath, content.data(), content.size());
}

Status MedicalStorage::append(const std::string& relativePath, const void* data,
                              std::size_t length) {
    if (!isSafeRelativePath(relativePath)) {
        return Status::InvalidArgument;
    }
    if (length == 0) {
        return Status::Ok;
    }
    if (data == nullptr) {
        return Status::InvalidArgument;
    }

    const std::string target = basePath_ + "/" + relativePath;

    const std::string::size_type slash = target.find_last_of('/');
    if (slash != std::string::npos && !makeDirectories(target.substr(0, slash), 0700)) {
        return statusFromErrno(errno);
    }

    // O_APPEND makes the offset update and the write one atomic operation, so
    // two writers cannot interleave inside a record.
    const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        return statusFromErrno(errno);
    }

    Status status = writeAll(fd, data, length);
    if (status == Status::Ok && ::fdatasync(fd) != 0) {
        status = statusFromErrno(errno);
    }
    ::close(fd);
    return status;
}

Result<std::vector<std::uint8_t>> MedicalStorage::read(const std::string& relativePath) const {
    using ReadResult = Result<std::vector<std::uint8_t>>;

    if (!isSafeRelativePath(relativePath)) {
        return ReadResult::fail(Status::InvalidArgument, "unsafe path: " + relativePath);
    }

    const std::string target = basePath_ + "/" + relativePath;
    const int fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return ReadResult::fail(statusFromErrno(errno),
                                "open " + relativePath + ": " + std::strerror(errno));
    }

    std::vector<std::uint8_t> content;
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved = errno;
            ::close(fd);
            return ReadResult::fail(statusFromErrno(saved), std::strerror(saved));
        }
        if (n == 0) {
            break;
        }
        content.insert(content.end(), buffer, buffer + n);
    }
    ::close(fd);

    return ReadResult::ok(std::move(content));
}

Result<std::string> MedicalStorage::readString(const std::string& relativePath) const {
    Result<std::vector<std::uint8_t>> raw = read(relativePath);
    if (!raw) {
        return Result<std::string>::fail(raw.status(), raw.message());
    }
    const std::vector<std::uint8_t>& bytes = raw.value();
    return Result<std::string>::ok(
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool MedicalStorage::exists(const std::string& relativePath) const {
    if (!isSafeRelativePath(relativePath)) {
        return false;
    }
    struct stat info = {};
    return ::stat((basePath_ + "/" + relativePath).c_str(), &info) == 0;
}

Status MedicalStorage::remove(const std::string& relativePath) {
    if (!isSafeRelativePath(relativePath)) {
        return Status::InvalidArgument;
    }
    const std::string target = basePath_ + "/" + relativePath;
    if (::unlink(target.c_str()) != 0) {
        return statusFromErrno(errno);
    }
    syncParentDirectory(target);
    return Status::Ok;
}

Result<std::vector<std::string>> MedicalStorage::list(const std::string& relativeDir) const {
    using ListResult = Result<std::vector<std::string>>;

    std::string target = basePath_;
    if (!relativeDir.empty()) {
        if (!isSafeRelativePath(relativeDir)) {
            return ListResult::fail(Status::InvalidArgument, "unsafe path: " + relativeDir);
        }
        target += "/" + relativeDir;
    }

    DIR* directory = ::opendir(target.c_str());
    if (directory == nullptr) {
        return ListResult::fail(statusFromErrno(errno),
                                "opendir " + target + ": " + std::strerror(errno));
    }

    std::vector<std::string> entries;
    while (const struct dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        entries.push_back(name);
    }
    ::closedir(directory);

    // Sorted output: callers iterate sessions in a deterministic order, and a
    // deterministic order is a precondition for reproducible verification.
    std::sort(entries.begin(), entries.end());
    return ListResult::ok(std::move(entries));
}

Result<StorageInfo> MedicalStorage::info() const {
    StorageInfo current = info_;

    struct statvfs vfs = {};
    if (::statvfs(basePath_.c_str(), &vfs) != 0) {
        return Result<StorageInfo>::fail(statusFromErrno(errno), std::strerror(errno));
    }
    current.totalBytes = static_cast<std::uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    current.availableBytes = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;

    return Result<StorageInfo>::ok(std::move(current));
}

const std::string& MedicalStorage::basePath() const noexcept {
    return basePath_;
}

}  // namespace med
