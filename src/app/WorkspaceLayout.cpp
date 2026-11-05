#include "WorkspaceLayout.hpp"

#include "Files.hpp"
#include "Text.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <vector>

namespace flows::app {
namespace fs = std::filesystem;

fs::path workspaceFolder(const fs::path& root, const std::string& path) {
    if (path.empty() || path.front() == '/' || text::containsAnyOf(path, "\\:"))
        throw std::runtime_error("Choose a folder inside the workspace");
    for (const auto& part : text::split(path, "/", true))
        if (part.empty() || part.front() == '.') throw std::runtime_error("Choose a visible workspace folder");
    std::error_code error;
    const auto base = fs::weakly_canonical(root, error);
    if (error) throw std::runtime_error("Cannot resolve workspace");
    const auto target = fs::weakly_canonical(root / path, error);
    const auto relativePath = target.lexically_relative(base);
    if (error || relativePath.empty() || relativePath.is_absolute())
        throw std::runtime_error("Cannot resolve workspace folder");
    for (const auto& part : relativePath)
        if (part == "..") throw std::runtime_error("Folder is outside the workspace");
    return root / path;
}

json::Array workspaceFiles(const fs::path& root) {
    struct Entry {
        std::string path, name;
        bool directory;
    };
    std::vector<Entry> entries;
    std::error_code error;
    fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error), end;
    for (; !error && iterator != end && entries.size() < 4096; iterator.increment(error)) {
        const auto name = iterator->path().filename().string();
        std::error_code status;
        const bool directory = iterator->is_directory(status);
        if (!name.empty() && name.front() == '.') {
            if (directory) iterator.disable_recursion_pending();
            continue;
        }
        if (!directory && !files::hasExtension(name, "flows")) continue;
        entries.push_back({files::relative(root, iterator->path()), name, directory});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    json::Array result;
    result.reserve(entries.size());
    for (auto& entry : entries)
        result.push_back(json::object(
            {{"path", entry.path}, {"name", entry.name}, {"kind", entry.directory ? "directory" : "instrument"}}));
    return result;
}

namespace {
json::Value remap(json::Value value, const std::map<std::string, std::string>& paths) {
    if (value.isString()) {
        if (const auto found = paths.find(value.string()); found != paths.end()) return json::Value(found->second);
    } else if (auto* array = value.getArray()) {
        for (auto& item : *array) item = remap(std::move(item), paths);
    } else if (auto* object = value.getObject()) {
        for (auto& [key, item] : *object) item = remap(std::move(item), paths);
    }
    return value;
}
std::vector<fs::path> listFiles(const fs::path& folder, bool recursive, std::string_view extension) {
    std::vector<fs::path> result;
    std::error_code error;
    if (!fs::is_directory(folder, error)) return result;
    const auto accept = [&](const fs::directory_entry& entry) {
        std::error_code status;
        return entry.is_regular_file(status) && (extension.empty() || files::hasExtension(entry.path().string(), extension));
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            if (accept(*it)) result.push_back(it->path());
    } else {
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            if (accept(*it)) result.push_back(it->path());
    }
    std::sort(result.begin(), result.end());
    return result;
}
}  // namespace

void organizeWorkspace(const fs::path& root) {
    for (const auto* name : {"tutorial", "examples", "AI-gen"})
        if (!files::createDirectories(workspaceFolder(root, name)))
            throw std::runtime_error("Could not create workspace folders");
    const auto marker = root / ".workspace-layout-v1";
    if (files::isFile(marker)) return;
    std::map<std::string, std::string> paths;
    auto sources = listFiles(root, false, "flows");
    for (auto& file : listFiles(root / "ADCxGather", true, {})) sources.push_back(std::move(file));
    for (const auto& file : sources) {
        if (files::isSymlink(file)) continue;
        const auto old = files::relative(root, file);
        const auto destination = old.starts_with("ADCxGather/") ? "tutorial/" + old.substr(11) : "examples/" + old;
        auto target = root / destination;
        if (files::isFile(target) && !files::sameContent(target, file)) target = files::nonexistentSibling(target);
        files::createDirectories(target.parent_path());
        if (!files::exists(target) && !files::copyFile(file, target))
            throw std::runtime_error("Could not organize instrument: " + old);
        paths[old] = files::relative(root, target);
    }
    auto metadata = listFiles(root / ".sequencers", true, "json");
    if (files::isFile(root / ".session.flowsinst")) metadata.push_back(root / ".session.flowsinst");
    if (files::isFile(root / ".assistant-history.json")) metadata.push_back(root / ".assistant-history.json");
    const auto backup = root / ".workspace-layout-backup";
    // Metadata files were written by the app itself; recovery may be large.
    json::ParseLimits limits;
    limits.maxBytes = 512u * 1024 * 1024;
    limits.maxSeparators = limits.maxSeparatorsPerContainer = 64u * 1024 * 1024;
    for (const auto& file : metadata) {
        const auto old = files::relative(root, file);
        auto target = file;
        if (old.starts_with(".sequencers/") && old.size() > 17) {
            const auto key = old.substr(12, old.size() - 12 - 5);
            if (const auto found = paths.find(key); found != paths.end())
                target = root / (".sequencers/" + found->second + ".json");
        }
        const auto saved = backup / old;
        files::createDirectories(saved.parent_path());
        if (!files::exists(saved) && !files::copyFile(file, saved))
            throw std::runtime_error("Could not back up workspace metadata");
        json::Value value;
        try {
            const auto bytes = files::tryRead(file, limits.maxBytes);
            if (!bytes) continue;
            value = json::parse(*bytes, limits);
        } catch (const std::exception&) {
            continue;
        }
        try {
            files::writeAtomic(target, json::stringifyPretty(remap(std::move(value), paths)));
        } catch (const std::exception&) {
            throw std::runtime_error("Could not update workspace metadata");
        }
    }
    for (const auto& [old, next] : paths) {
        const auto saved = backup / old;
        files::createDirectories(saved.parent_path());
        if (!files::exists(saved) && !files::copyFile(root / old, saved))
            throw std::runtime_error("Could not back up instrument");
        if (!files::remove(root / old)) throw std::runtime_error("Could not finish organizing instrument");
    }
    // Only remove empty directories; unrelated tutorial assets remain intact.
    std::vector<fs::path> folders;
    std::error_code error;
    if (fs::is_directory(root / "ADCxGather", error))
        for (fs::recursive_directory_iterator it(root / "ADCxGather", error), end; !error && it != end; it.increment(error))
            if (it->is_directory(error)) folders.push_back(it->path());
    std::sort(folders.begin(), folders.end());
    for (auto it = folders.rbegin(); it != folders.rend(); ++it) fs::remove(*it, error);
    fs::remove(root / "ADCxGather", error);
    try {
        files::writeAtomic(marker, "1\n");
    } catch (const std::exception&) {
        throw std::runtime_error("Could not save workspace layout");
    }
}
}  // namespace flows::app
