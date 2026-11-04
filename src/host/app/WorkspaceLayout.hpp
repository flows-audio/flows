#pragma once
#include <juce_core/juce_core.h>

#include <filesystem>
#include <map>

namespace flows::app {
inline juce::File workspaceFolder(const juce::File& root, const juce::String& path) {
    if (path.isEmpty() || juce::File::isAbsolutePath(path) || path.containsAnyOf("\\:"))
        throw std::runtime_error("Choose a folder inside the workspace");
    for (const auto& part : juce::StringArray::fromTokens(path, "/", {}))
        if (part.isEmpty() || part.startsWithChar('.'))
            throw std::runtime_error("Choose a visible workspace folder");
    std::error_code error;
    auto base = std::filesystem::weakly_canonical(root.getFullPathName().toStdString(), error);
    if (error) throw std::runtime_error("Cannot resolve workspace");
    auto target = std::filesystem::weakly_canonical(
        root.getChildFile(path).getFullPathName().toStdString(), error);
    auto relative = target.lexically_relative(base);
    if (error || relative.empty() || relative.is_absolute())
        throw std::runtime_error("Cannot resolve workspace folder");
    for (const auto& part : relative)
        if (part == "..") throw std::runtime_error("Folder is outside the workspace");
    return root.getChildFile(path);
}
inline juce::Array<juce::var> workspaceFiles(const juce::File& root) {
    juce::Array<juce::var> result;
    for (auto file : root.findChildFiles(juce::File::findFilesAndDirectories, true)) {
        const auto path = file.getRelativePathFrom(root);
        bool hidden = false;
        for (const auto& part : juce::StringArray::fromTokens(path, "/", {}))
            if (part.startsWithChar('.')) hidden = true;
        if (hidden || (!file.isDirectory() && !file.hasFileExtension("flows"))) continue;
        auto* value = new juce::DynamicObject();
        value->setProperty("path", path);
        value->setProperty("name", file.getFileName());
        value->setProperty("kind", file.isDirectory() ? "directory" : "instrument");
        result.add(juce::var(value));
        if (result.size() >= 4096) break;
    }
    return result;
}
// Run before restoring documents. Keep original files and metadata in a hidden
// backup, and never replace a different destination instrument.
inline void organizeWorkspace(const juce::File& root) {
    for (const auto* name : {"tutorial", "examples", "AI-gen"})
        if (!workspaceFolder(root, name).createDirectory())
            throw std::runtime_error("Could not create workspace folders");
    const auto marker = root.getChildFile(".workspace-layout-v1");
    if (marker.existsAsFile()) return;
    std::map<juce::String, juce::String> paths;
    auto files = root.findChildFiles(juce::File::findFiles, false, "*.flows");
    files.addArray(root.getChildFile("ADCxGather").findChildFiles(juce::File::findFiles, true));
    for (const auto& file : files) {
        if (file.isSymbolicLink()) continue;
        const auto old = file.getRelativePathFrom(root);
        const auto destination =
            old.startsWith("ADCxGather/") ? "tutorial/" + old.substring(11) : "examples/" + old;
        auto target = root.getChildFile(destination);
        if (target.existsAsFile() && !target.hasIdenticalContentTo(file))
            target = target.getNonexistentSibling();
        if (!target.getParentDirectory().createDirectory() ||
            (!target.exists() && !file.copyFileTo(target)))
            throw std::runtime_error("Could not organize instrument: " + old.toStdString());
        paths[old] = target.getRelativePathFrom(root);
    }
    const auto remap = [&](auto&& self, juce::var value) -> juce::var {
        if (value.isString()) {
            if (const auto found = paths.find(value.toString()); found != paths.end())
                return found->second;
        } else if (auto* array = value.getArray()) {
            for (auto& item : *array) item = self(self, item);
        } else if (auto* object = value.getDynamicObject()) {
            auto& properties = object->getProperties();
            for (int i = 0; i < properties.size(); ++i)
                properties.set(properties.getName(i), self(self, properties.getValueAt(i)));
        }
        return value;
    };
    auto metadata =
        root.getChildFile(".sequencers").findChildFiles(juce::File::findFiles, true, "*.json");
    if (root.getChildFile(".session.flowsinst").existsAsFile())
        metadata.add(root.getChildFile(".session.flowsinst"));
    if (root.getChildFile(".assistant-history.json").existsAsFile())
        metadata.add(root.getChildFile(".assistant-history.json"));
    const auto backup = root.getChildFile(".workspace-layout-backup");
    for (const auto& file : metadata) {
        const auto old = file.getRelativePathFrom(root);
        auto target = file;
        if (old.startsWith(".sequencers/"))
            if (const auto found = paths.find(old.substring(12).dropLastCharacters(5));
                found != paths.end())
                target = root.getChildFile(".sequencers/" + found->second + ".json");
        auto saved = backup.getChildFile(old);
        saved.getParentDirectory().createDirectory();
        if (!saved.exists() && !file.copyFileTo(saved))
            throw std::runtime_error("Could not back up workspace metadata");
        juce::var value;
        const auto parsed = juce::JSON::parse(file.loadFileAsString(), value);
        if (parsed.failed()) continue;
        target.getParentDirectory().createDirectory();
        if (!target.replaceWithText(juce::JSON::toString(remap(remap, value))))
            throw std::runtime_error("Could not update workspace metadata");
    }
    for (const auto& [old, next] : paths) {
        auto saved = backup.getChildFile(old);
        saved.getParentDirectory().createDirectory();
        auto file = root.getChildFile(old);
        if (!saved.exists() && !file.copyFileTo(saved))
            throw std::runtime_error("Could not back up instrument");
        if (!file.deleteFile()) throw std::runtime_error("Could not finish organizing instrument");
    }
    // Only remove empty directories; unrelated tutorial assets remain intact.
    auto folders =
        root.getChildFile("ADCxGather").findChildFiles(juce::File::findDirectories, true);
    for (int i = folders.size() - 1; i >= 0; --i) folders[i].deleteFile();
    root.getChildFile("ADCxGather").deleteFile();
    if (!marker.replaceWithText("1\n")) throw std::runtime_error("Could not save workspace layout");
}
}  // namespace flows::app
