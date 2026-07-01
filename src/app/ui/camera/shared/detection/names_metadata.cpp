#include "ui/camera/shared/detection/names_metadata.h"

namespace denso::ui {

std::vector<std::string> parse_names_metadata(const std::string& names) {
    // Extract each quoted substring in order; keys ascend, so appearance order
    // is class-id order. Handles both ' and " quotes.
    std::vector<std::string> out;
    size_t i = 0;
    while (i < names.size()) {
        const char c = names[i];
        if (c == '\'' || c == '"') {
            const char quote = c;
            const size_t start = i + 1;
            size_t end = names.find(quote, start);
            if (end == std::string::npos) {
                break;
            }
            out.push_back(names.substr(start, end - start));
            i = end + 1;
        } else {
            ++i;
        }
    }
    return out;
}

} // namespace denso::ui
