#include "models/class_map.h"
#include <set>
namespace denso::models {
namespace { ClassMapResult fail(const std::string& why) { return {std::nullopt, why}; } }
ClassMapResult resolve_class_map(const std::vector<std::string>& old_names,
                                 const std::vector<std::string>& new_names,
                                 const std::map<std::string, std::string>& explicit_remap) {
    // Class ids are int and a class-name list is always tiny (models carry tens of
    // classes), so size()->int can never overflow here.
    std::map<std::string, int> new_index;
    for (int i = 0; i < static_cast<int>(new_names.size()); ++i)
        if (!new_index.emplace(new_names[i], i).second)
            return fail("duplicate name in new_names: " + new_names[i]);
    std::set<std::string> old_set;
    for (const auto& n : old_names)
        if (!old_set.insert(n).second)
            return fail("duplicate name in old_names: " + n);
    for (const auto& [k, v] : explicit_remap) {
        if (old_set.find(k) == old_set.end())
            return fail("explicit_remap key not in old_names: " + k);
        if (new_index.find(v) == new_index.end())
            return fail("explicit_remap target not in new_names: " + v);
    }
    std::map<int, int> forward;
    std::set<int> used_new;
    for (int i = 0; i < static_cast<int>(old_names.size()); ++i) {
        const auto it = explicit_remap.find(old_names[i]);
        const std::string& target = it == explicit_remap.end() ? old_names[i] : it->second;
        const auto ni = new_index.find(target);
        if (ni == new_index.end()) continue;   // omitted; selected-id check happens at migrate time
        if (!used_new.insert(ni->second).second)
            return fail("non-injective remap: two old ids map to new id " + std::to_string(ni->second));
        forward.emplace(i, ni->second);
    }
    return {forward, {}};
}
}
