// Portable file helpers used by the app core in every host.
#include "app/Files.hpp"
#include "app/Text.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace flows;
namespace {
namespace fs = std::filesystem;
void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
struct Folder {
    fs::path path = fs::temp_directory_path() / ("flows-files-tests-" + text::randomId());
    Folder() { check(fs::create_directories(path), "Could not create a test folder"); }
    ~Folder() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void bounded_reads() {
    Folder folder;
    const auto file = folder.path / "data.bin";
    check(!files::tryRead(file, 16), "A missing file was read");
    files::writeAtomic(file, "");
    check(files::tryRead(file, 16) == std::string(), "An empty file did not read as empty");
    // Sizes around the read buffer's growth steps and the limit.
    for (const std::size_t size : {std::size_t{1}, std::size_t{4095}, std::size_t{65536}, std::size_t{65537}, std::size_t{200000}}) {
        std::string bytes(size, '\0');
        for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>('a' + i % 23);
        files::writeAtomic(file, bytes);
        check(files::tryRead(file, size) == bytes, "A file exactly at the limit was not read intact");
        check(files::tryRead(file, 256 * 1024 * 1024) == bytes, "A small file under a large limit was not read intact");
        check(!files::tryRead(file, size - 1), "A file over the limit was accepted");
    }
    check(!files::tryRead(folder.path, 16), "A folder was read as a file");
}

void numbered_siblings() {
    Folder folder;
    const auto touch = [](const fs::path& file) { files::writeAtomic(file, "x"); };
    const auto take = folder.path / "take.wav";
    check(files::nonexistentSibling(take) == take, "A free name was renamed");
    touch(take);
    check(files::nonexistentSibling(take).filename() == "take(2).wav", "A taken name did not get the next number");
    touch(folder.path / "take(2).wav");
    check(files::nonexistentSibling(folder.path / "take(2).wav").filename() == "take(3).wav",
          "A numbered name did not continue its sequence");
    // A timestamp in parentheses is part of the name, not a counter to increment.
    const auto stamped = folder.path / "take(20240101123045).wav";
    touch(stamped);
    check(files::nonexistentSibling(stamped).filename() == "take(20240101123045)(2).wav",
          "A long digit run was treated as a counter");
}
}  // namespace

int main() {
    try {
        bounded_reads();
        numbered_siblings();
        std::cout << "App files passed: bounded reads at, under and over their limit, and numbered sibling names.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "App files failed: " << error.what() << "\n";
        return 1;
    }
}
