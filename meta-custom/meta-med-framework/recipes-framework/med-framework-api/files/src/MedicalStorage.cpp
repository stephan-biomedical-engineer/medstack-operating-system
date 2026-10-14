// SPDX-License-Identifier: MIT

#include "MedicalStorage.h"

#include <grp.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

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

// ---------------------------------------------------------------------------
// RecordStream

struct RecordStream::Impl {
    int fd = -1;
    std::size_t capacity = 0;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::uint8_t> queued;  // whole records, in order
    std::uint64_t queuedRecords = 0;
    bool closing = false;
    bool closed = false;
    RecordStreamStats stats;

    off_t end = 0;  // where the next batch begins; the writer's alone
    std::thread writer;

    void run() {
        std::vector<std::uint8_t> batch;
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return closing || !queued.empty(); });
            if (queued.empty()) {
                return;  // closing, and nothing left to write
            }
            batch.clear();
            batch.swap(queued);
            const std::uint64_t records = queuedRecords;
            queuedRecords = 0;
            lock.unlock();

            Status status = writeAll(fd, batch.data(), batch.size());
            if (status == Status::Ok && ::fdatasync(fd) != 0) {
                status = statusFromErrno(errno);
            }
            if (status == Status::Ok) {
                end += static_cast<off_t>(batch.size());
            } else {
                // Whatever part of the batch reached the file goes: a record
                // must not be left half written in front of the next one.
                // If even this fails there is nothing further to do here, and
                // the count below is what says so.
                if (::ftruncate(fd, end) == 0) {
                    ::fdatasync(fd);
                }
            }

            lock.lock();
            if (status == Status::Ok) {
                stats.written += records;
                ++stats.syncs;
            } else {
                stats.failed += records;
                stats.lastError = status;
            }
        }
    }
};

RecordStream::RecordStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RecordStream::~RecordStream() {
    close();
}

Status RecordStream::append(const void* data, std::size_t length) {
    if (length == 0) {
        return Status::Ok;
    }
    if (data == nullptr) {
        return Status::InvalidArgument;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->closing) {
            return Status::Unavailable;
        }
        if (impl_->queued.size() + length > impl_->capacity) {
            ++impl_->stats.refused;
            return Status::WouldBlock;
        }
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        impl_->queued.insert(impl_->queued.end(), bytes, bytes + length);
        ++impl_->queuedRecords;
    }
    impl_->wake.notify_one();
    return Status::Ok;
}

RecordStreamStats RecordStream::close() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->closed) {
            return impl_->stats;
        }
        impl_->closing = true;
    }
    impl_->wake.notify_one();
    if (impl_->writer.joinable()) {
        impl_->writer.join();
    }
    ::close(impl_->fd);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closed = true;
    return impl_->stats;
}

RecordStreamStats RecordStream::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

Result<std::unique_ptr<RecordStream>> MedicalStorage::openStream(const std::string& relativePath,
                                                                 std::size_t queueBytes) {
    using StreamResult = Result<std::unique_ptr<RecordStream>>;

    if (!isSafeRelativePath(relativePath)) {
        return StreamResult::fail(Status::InvalidArgument, "unsafe path: " + relativePath);
    }
    if (queueBytes == 0) {
        return StreamResult::fail(Status::InvalidArgument, "a record stream needs a queue");
    }

    const std::string target = basePath_ + "/" + relativePath;
    const std::string::size_type slash = target.find_last_of('/');
    if (slash != std::string::npos && !makeDirectories(target.substr(0, slash), 0700)) {
        return StreamResult::fail(statusFromErrno(errno), "cannot create " + relativePath);
    }

    const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        return StreamResult::fail(statusFromErrno(errno), "cannot open " + relativePath);
    }
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0) {
        const int saved = errno;
        ::close(fd);
        return StreamResult::fail(statusFromErrno(saved), "cannot measure " + relativePath);
    }
    // The file's name, made durable once, here: the writer synchronises data
    // only.
    syncParentDirectory(target);

    std::unique_ptr<RecordStream::Impl> impl(new RecordStream::Impl);
    impl->fd = fd;
    impl->capacity = queueBytes;
    impl->end = end;
    RecordStream::Impl* raw = impl.get();
    try {
        impl->writer = std::thread([raw] { raw->run(); });
    } catch (const std::exception&) {
        ::close(fd);
        return StreamResult::fail(Status::Unavailable, "cannot start the record writer");
    }
    return StreamResult::ok(std::unique_ptr<RecordStream>(new RecordStream(std::move(impl))));
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

Result<std::vector<std::uint8_t>> MedicalStorage::read(const std::string& relativePath,
                                                       std::uint64_t offset,
                                                       std::size_t length) const {
    using ReadResult = Result<std::vector<std::uint8_t>>;

    if (!isSafeRelativePath(relativePath)) {
        return ReadResult::fail(Status::InvalidArgument, "unsafe path: " + relativePath);
    }
    if (offset > static_cast<std::uint64_t>(INT64_MAX)) {
        return ReadResult::fail(Status::OutOfRange, "offset beyond any file");
    }

    const std::string target = basePath_ + "/" + relativePath;
    const int fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return ReadResult::fail(statusFromErrno(errno),
                                "open " + relativePath + ": " + std::strerror(errno));
    }

    std::vector<std::uint8_t> content(length);
    std::size_t got = 0;
    while (got < length) {
        const ssize_t n = ::pread(fd, content.data() + got, length - got,
                                  static_cast<off_t>(offset + got));
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
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);

    content.resize(got);
    return ReadResult::ok(std::move(content));
}

Result<std::uint64_t> MedicalStorage::size(const std::string& relativePath) const {
    if (!isSafeRelativePath(relativePath)) {
        return Result<std::uint64_t>::fail(Status::InvalidArgument, "unsafe path: " + relativePath);
    }
    struct stat info = {};
    if (::lstat((basePath_ + "/" + relativePath).c_str(), &info) != 0) {
        return Result<std::uint64_t>::fail(statusFromErrno(errno), std::strerror(errno));
    }
    if (!S_ISREG(info.st_mode)) {
        return Result<std::uint64_t>::fail(Status::InvalidArgument,
                                           relativePath + " is not a regular file");
    }
    return Result<std::uint64_t>::ok(static_cast<std::uint64_t>(info.st_size));
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

namespace {

/// A published name is one path component: it is joined to a directory this
/// class chose, and a separator in it would let a caller choose another.
bool isSafeName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}

/// `<root>/review/<namespace>`, or empty when the handle was opened on a root
/// with no namespace of its own.
std::string reviewDirectory(const std::string& root, const std::string& basePath) {
    if (basePath.size() <= root.size() + 1 || basePath.compare(0, root.size(), root) != 0 ||
        basePath[root.size()] != '/') {
        return {};
    }
    return root + "/review/" + basePath.substr(root.size() + 1);
}

bool lookupGroup(const std::string& name, gid_t& gid) {
    if (name.empty()) {
        return false;
    }
    std::vector<char> buffer(4096);
    struct group entry = {};
    struct group* found = nullptr;
    for (;;) {
        const int rc = ::getgrnam_r(name.c_str(), &entry, buffer.data(), buffer.size(), &found);
        if (rc == ERANGE && buffer.size() < (1U << 20)) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (rc != 0 || found == nullptr) {
            return false;
        }
        gid = found->gr_gid;
        return true;
    }
}

/// Creates one directory of the review tree and makes it what it must be even
/// if it already existed as something looser: group-traversable, and no more.
bool ensureReviewDirectory(const std::string& path, gid_t gid) {
    if (::mkdir(path.c_str(), 0750) != 0 && errno != EEXIST) {
        return false;
    }
    struct stat info = {};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return false;
    }
    if (info.st_gid != gid && ::chown(path.c_str(), static_cast<uid_t>(-1), gid) != 0) {
        return false;
    }
    return ::chmod(path.c_str(), 0750) == 0;
}

}  // namespace

Status MedicalStorage::publish(const std::string& relativePath, const std::string& publishedName,
                               const std::string& group) {
    if (!isSafeRelativePath(relativePath) || !isSafeName(publishedName)) {
        return Status::InvalidArgument;
    }
    const std::string directory = reviewDirectory(info_.root, basePath_);
    if (directory.empty()) {
        return Status::NotSupported;
    }

    const std::string source = basePath_ + "/" + relativePath;
    struct stat sourceInfo = {};
    if (::lstat(source.c_str(), &sourceInfo) != 0) {
        return statusFromErrno(errno);
    }
    // lstat, and a regular file only: a symbolic link here would publish
    // whatever it points at, under the authority of whoever holds this handle.
    if (!S_ISREG(sourceInfo.st_mode)) {
        return Status::InvalidArgument;
    }

    gid_t gid = 0;
    if (!lookupGroup(group, gid)) {
        return Status::Unavailable;
    }

    if (!ensureReviewDirectory(info_.root + "/review", gid) ||
        !ensureReviewDirectory(directory, gid)) {
        return statusFromErrno(errno);
    }

    // The inode first, the name second: a name that appears before the file is
    // readable is a name that fails for the reader it was made for.
    if (sourceInfo.st_gid != gid && ::chown(source.c_str(), static_cast<uid_t>(-1), gid) != 0) {
        return statusFromErrno(errno);
    }
    if (::chmod(source.c_str(), 0640) != 0) {
        return statusFromErrno(errno);
    }

    const std::string target = directory + "/" + publishedName;
    if (::link(source.c_str(), target.c_str()) != 0) {
        if (errno != EEXIST) {
            return statusFromErrno(errno);
        }
        struct stat targetInfo = {};
        if (::lstat(target.c_str(), &targetInfo) == 0 && targetInfo.st_dev == sourceInfo.st_dev &&
            targetInfo.st_ino == sourceInfo.st_ino) {
            return Status::Ok;
        }
        // The name belongs to an earlier file - one that was derived again.
        if (::unlink(target.c_str()) != 0 || ::link(source.c_str(), target.c_str()) != 0) {
            return statusFromErrno(errno);
        }
    }
    syncParentDirectory(target);
    return Status::Ok;
}

Status MedicalStorage::unpublish(const std::string& publishedName) {
    if (!isSafeName(publishedName)) {
        return Status::InvalidArgument;
    }
    const std::string directory = reviewDirectory(info_.root, basePath_);
    if (directory.empty()) {
        return Status::NotSupported;
    }
    const std::string target = directory + "/" + publishedName;
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
