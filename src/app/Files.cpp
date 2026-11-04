#include "Files.hpp"

#include "Text.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace flows::files {
namespace {
std::string pathString(const fs::path& path) { return path.generic_string(); }
}  // namespace

std::optional<std::string> tryRead(const fs::path& file, std::size_t limit) {
    std::error_code error;
    if (!fs::is_regular_file(file, error) || error) return std::nullopt;
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes;
    bytes.resize(limit + 1);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad()) return std::nullopt;
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > limit) return std::nullopt;
    return bytes;
}
std::string read(const fs::path& file, std::size_t limit) {
    std::error_code error;
    if (!fs::is_regular_file(file, error)) throw std::runtime_error("Could not read " + fileName(pathString(file)));
    const auto bytes = tryRead(file, limit);
    if (!bytes) {
        const auto size = fs::file_size(file, error);
        if (!error && size > limit)
            throw std::runtime_error(fileName(pathString(file)) + " exceeds its size limit");
        throw std::runtime_error("Could not read " + fileName(pathString(file)));
    }
    return *bytes;
}
fs::path temporarySibling(const fs::path& target) {
    return target.parent_path() / ("." + target.filename().string() + ".tmp-" + text::randomId().substr(0, 12));
}
namespace {
void writeFile(const fs::path& file, std::initializer_list<std::string_view> parts) {
    std::FILE* output = std::fopen(file.string().c_str(), "wb");
    if (!output) throw std::runtime_error("Could not write " + file.filename().string());
    bool written = true;
    for (const auto bytes : parts)
        written = written && (bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size());
    const bool flushed = std::fflush(output) == 0;
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // Data must reach the disk before rename publishes the new name.
    ::fsync(fileno(output));
#endif
    const bool closed = std::fclose(output) == 0;
    if (!written || !flushed || !closed) {
        std::error_code ignored;
        fs::remove(file, ignored);
        throw std::runtime_error("Could not finish writing " + file.filename().string());
    }
}
}  // namespace
void writeAtomic(const fs::path& target, std::string_view bytes) { writeAtomic(target, {bytes}); }
void writeAtomic(const fs::path& target, std::initializer_list<std::string_view> parts) {
    std::error_code error;
    if (!target.parent_path().empty()) fs::create_directories(target.parent_path(), error);
    if (fs::is_directory(target, error)) throw std::runtime_error("Could not replace a folder with a file");
    const auto temporary = temporarySibling(target);
    writeFile(temporary, parts);
    fs::rename(temporary, target, error);
    if (error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("Could not replace " + target.filename().string());
    }
}
bool writeNew(const fs::path& target, std::string_view bytes) {
    std::error_code error;
    if (!target.parent_path().empty()) fs::create_directories(target.parent_path(), error);
    const auto temporary = temporarySibling(target);
    writeFile(temporary, {bytes});
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    } cleanup{temporary};
    // A hard link publishes atomically and fails if the name already exists.
    fs::create_hard_link(temporary, target, error);
    if (!error) return true;
    if (fs::exists(target)) return false;
    // Browser storage has no hard links. The web host is a single process and
    // the workspace is held by one tab, so a checked rename is sufficient.
    error.clear();
    if (fs::exists(target, error)) return false;
    fs::rename(temporary, target, error);
    if (error) throw std::runtime_error("Could not publish " + target.filename().string());
    return true;
}

std::string stem(std::string_view name) {
    const auto dot = name.rfind('.');
    return std::string(dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot));
}
std::string fileName(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}
std::string withExtension(std::string_view name, std::string_view extension) {
    auto base = stem(name);
    if (extension.empty()) return base;
    return base + (extension.front() == '.' ? "" : ".") + std::string(extension);
}
bool hasExtension(std::string_view path, std::string_view extensions) {
    const auto name = fileName(path);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    const auto extension = std::string_view(name).substr(dot + 1);
    for (const auto& candidate : text::split(extensions, ";"))
        if (text::equalsIgnoreCase(candidate, extension)) return true;
    return false;
}
fs::path nonexistentSibling(const fs::path& file) {
    std::error_code error;
    if (!fs::exists(file, error)) return file;
    const auto name = file.filename().string();
    const auto dot = name.rfind('.');
    const bool hasDot = dot != std::string::npos && dot != 0;
    std::string prefix = hasDot ? name.substr(0, dot) : name;
    const std::string suffix = hasDot ? name.substr(dot) : std::string();
    int number = 1;
    const auto trimmed = text::trim(prefix);
    if (!trimmed.empty() && trimmed.back() == ')') {
        const auto open = prefix.rfind('('), close = prefix.rfind(')');
        if (open != std::string::npos && open > 0 && close > open) {
            const auto digits = prefix.substr(open + 1, close - open - 1);
            if (text::containsOnly(digits, "0123456789")) {
                number = digits.empty() ? 0 : std::stoi(digits);
                prefix = prefix.substr(0, open);
            }
        }
    }
    fs::path candidate;
    do candidate = file.parent_path() / (prefix + "(" + std::to_string(++number) + ")" + suffix);
    while (fs::exists(candidate, error));
    return candidate;
}

std::optional<fs::path> child(const fs::path& root, std::string_view relativePath) {
    const fs::path relativeValue{std::string(relativePath)};
    if (relativeValue.is_absolute() || relativePath.empty()) return std::nullopt;
    auto result = (root / relativeValue).lexically_normal();
    if (!isInside(root, result)) return std::nullopt;
    return result;
}
bool isInside(const fs::path& root, const fs::path& file) {
    const auto base = root.lexically_normal(), target = file.lexically_normal();
    const auto relativePath = target.lexically_relative(base);
    if (relativePath.empty() || relativePath.is_absolute() || relativePath == ".") return false;
    for (const auto& part : relativePath)
        if (part == "..") return false;
    return true;
}
bool isInsideResolved(const fs::path& root, const fs::path& file) {
    std::error_code error;
    const auto base = fs::weakly_canonical(root, error);
    if (error) return false;
    const auto target = fs::weakly_canonical(file, error);
    if (error) return false;
    return isInside(base, target);
}
std::string relative(const fs::path& root, const fs::path& file) {
    return file.lexically_normal().lexically_relative(root.lexically_normal()).generic_string();
}

bool isFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}
bool isDirectory(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}
bool exists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error) || fs::is_symlink(path, error);
}
bool isSymlink(const fs::path& path) {
    std::error_code error;
    return fs::is_symlink(path, error);
}
std::int64_t size(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return -1;
    const auto bytes = fs::file_size(path, error);
    return error ? -1 : static_cast<std::int64_t>(bytes);
}
std::int64_t modifiedStamp(const fs::path& path) {
    std::error_code error;
    const auto stamp = fs::last_write_time(path, error);
    return error ? 0 : static_cast<std::int64_t>(stamp.time_since_epoch().count());
}
std::int64_t modifiedMillis(const fs::path& path) {
    std::error_code error;
    const auto stamp = fs::last_write_time(path, error);
    if (error) return 0;
    // file_time_type's epoch differs per library; measure the age instead.
    const auto age = fs::file_time_type::clock::now() - stamp;
    return text::currentTimeMillis() - std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
}
bool createDirectories(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    return fs::is_directory(path, error);
}
bool remove(const fs::path& path) {
    std::error_code error;
    return fs::remove(path, error) && !error;
}
void removeAll(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
}
bool sameContent(const fs::path& a, const fs::path& b) {
    const auto first = size(a), second = size(b);
    if (first < 0 || first != second) return false;
    const auto x = tryRead(a, static_cast<std::size_t>(first)), y = tryRead(b, static_cast<std::size_t>(second));
    return x && y && *x == *y;
}
bool copyFile(const fs::path& from, const fs::path& to) {
    std::error_code error;
    if (!to.parent_path().empty()) fs::create_directories(to.parent_path(), error);
    error.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
    return !error;
}
bool move(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::rename(from, to, error);
    return !error;
}

struct ProcessLock::State {
    std::string name;
    std::timed_mutex* local = nullptr;
    bool held = false;
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    int handle = -1;
#endif
};
namespace {
std::timed_mutex& localMutex(const std::string& name) {
    static std::mutex registry;
    static std::map<std::string, std::unique_ptr<std::timed_mutex>> mutexes;
    std::lock_guard lock(registry);
    auto& slot = mutexes[name];
    if (!slot) slot = std::make_unique<std::timed_mutex>();
    return *slot;
}
}  // namespace
ProcessLock::ProcessLock(std::string name) : state_(std::make_unique<State>()) {
    state_->name = std::move(name);
    state_->local = &localMutex(state_->name);
}
ProcessLock::~ProcessLock() { exit(); }
bool ProcessLock::enter(int timeoutMs) {
    if (state_->held) return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    if (!state_->local->try_lock_until(deadline)) return false;
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // Other processes (a DAW plugin and the standalone app) share a lock file.
    const char* folder = access("/var/tmp", W_OK) == 0 ? "/var/tmp" : "/tmp";
    const auto path = std::string(folder) + "/" + state_->name + ".lock";
    state_->handle = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (state_->handle < 0) {
        state_->local->unlock();
        return false;
    }
    for (;;) {
        struct flock request{};
        request.l_type = F_WRLCK;
        request.l_whence = SEEK_SET;
        if (::fcntl(state_->handle, F_SETLK, &request) == 0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::close(state_->handle);
            state_->handle = -1;
            state_->local->unlock();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    state_->held = true;
    return true;
}
void ProcessLock::exit() {
    if (!state_ || !state_->held) return;
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (state_->handle >= 0) {
        struct flock request{};
        request.l_type = F_UNLCK;
        request.l_whence = SEEK_SET;
        ::fcntl(state_->handle, F_SETLK, &request);
        ::close(state_->handle);
        state_->handle = -1;
    }
#endif
    state_->held = false;
    state_->local->unlock();
}
}  // namespace flows::files
