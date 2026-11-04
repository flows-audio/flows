#pragma once
#include "Json.hpp"

#include <filesystem>
#include <string>

namespace flows::app {
// Validates a visible folder inside the workspace and returns its location.
// Throws for absolute paths, hidden components or escapes through links.
std::filesystem::path workspaceFolder(const std::filesystem::path& root, const std::string& path);
// Visible instruments and folders, sorted by path (at most 4096 entries).
json::Array workspaceFiles(const std::filesystem::path& root);
// Run before restoring documents. Keeps original files and metadata in a
// hidden backup, and never replaces a different destination instrument.
void organizeWorkspace(const std::filesystem::path& root);
}  // namespace flows::app
