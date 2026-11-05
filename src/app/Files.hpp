#pragma once
// Filesystem helpers for the portable core. Every host gives the core real
// directories: macOS uses the user's Application Support folder; the web host
// mounts browser storage at a virtual path. Workspace-relative paths always
// use '/' separators, matching what the editor shows.
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace flows::files {
namespace fs = std::filesystem;

// Reads at most `limit` bytes. Throws if the file is missing, unreadable or
// larger than the limit.
std::string read(const fs::path& file, std::size_t limit);
// Like read, but returns nullopt instead of throwing.
std::optional<std::string> tryRead(const fs::path& file, std::size_t limit);
// Writes through a hidden sibling temporary file and renames it over the
// target, so readers never observe a partial file.
void writeAtomic(const fs::path& target, std::string_view bytes);
// Same, concatenating parts without building one large string first.
void writeAtomic(const fs::path& target, std::initializer_list<std::string_view> parts);
// Writes bytes to a new file and publishes it only if `target` does not exist.
// Returns false if another writer created the target first.
bool writeNew(const fs::path& target, std::string_view bytes);
// Hidden sibling temporary path for staging a write next to `target`.
fs::path temporarySibling(const fs::path& target);

// `name(2).ext`, `name(3).ext`... matching the names the app always produced.
fs::path nonexistentSibling(const fs::path& file);
// Replaces the extension after the last dot (a leading dot is part of the name).
std::string withExtension(std::string_view name, std::string_view extension);
std::string stem(std::string_view name);
std::string fileName(std::string_view path);
// Case-insensitive match against a ';'-separated list such as "wav;aif".
bool hasExtension(std::string_view path, std::string_view extensions);

// Lexical child path of root, or nullopt if the result is not strictly inside
// root (absolute inputs, '..' escapes, or the root itself).
std::optional<fs::path> child(const fs::path& root, std::string_view relative);
bool isInside(const fs::path& root, const fs::path& file);
// Same containment check after resolving symbolic links that exist.
bool isInsideResolved(const fs::path& root, const fs::path& file);
std::string relative(const fs::path& root, const fs::path& file);

bool isFile(const fs::path& path);
bool isDirectory(const fs::path& path);
bool exists(const fs::path& path);
bool isSymlink(const fs::path& path);
std::int64_t size(const fs::path& path);  // -1 if missing
std::int64_t modifiedStamp(const fs::path& path);
std::int64_t modifiedMillis(const fs::path& path);
bool createDirectories(const fs::path& path);
bool remove(const fs::path& path);
void removeAll(const fs::path& path);
bool sameContent(const fs::path& a, const fs::path& b);
bool copyFile(const fs::path& from, const fs::path& to);
bool move(const fs::path& from, const fs::path& to);

// Exclusive lock shared with other processes that open the same lock name.
// On platforms without processes (the browser) it only excludes threads.
class ProcessLock {
public:
    explicit ProcessLock(std::string name);
    ~ProcessLock();
    ProcessLock(const ProcessLock&) = delete;
    ProcessLock& operator=(const ProcessLock&) = delete;
    // Waits up to `timeoutMs` for the lock.
    bool enter(int timeoutMs);
    void exit();

private:
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace flows::files
