#pragma once
// Built editor files embedded by tools/embed_ui.py.
#include <cstddef>

namespace FlowsUi {
struct File {
    const char* path;
    const char* mime;
    const unsigned char* data;
    std::size_t size;
};
extern const File files[];
extern const std::size_t count;
}  // namespace FlowsUi
