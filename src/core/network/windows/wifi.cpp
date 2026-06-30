#include "network/windows/wifi.h"

#include "network/windows/parse.h"
#include "util/strutil.h"

namespace denso::network::wifi {

using denso::network::parse::value_after_colon;
using denso::strutil::iequals;
using denso::strutil::split_lines;
using denso::strutil::trim;

std::vector<WifiNetwork> parse_wifi_networks(const std::string& out) {
    std::vector<WifiNetwork> nets;
    std::optional<WifiNetwork> cur;
    const auto flush = [&] {
        if (cur) {
            if (!cur->ssid.empty()) nets.push_back(*cur);
            cur.reset();
        }
    };
    for (const auto& line : split_lines(out)) {
        const std::string l = trim(line);
        if (l.starts_with("SSID ") && !l.starts_with("BSSID")) {
            flush();
            cur = WifiNetwork{value_after_colon(line), "", false};
        } else if (cur) {
            if (l.starts_with("Authentication")) {
                cur->secured = !iequals(value_after_colon(line), "Open");
            } else if (l.starts_with("Signal") && cur->signal.empty()) {
                cur->signal = value_after_colon(line);
            }
        }
    }
    flush();
    return nets;
}

std::string xml_escape(const std::string& s) {
    using denso::strutil::replace_all;
    std::string out = s;
    out = replace_all(out, "&", "&amp;");
    out = replace_all(out, "<", "&lt;");
    out = replace_all(out, ">", "&gt;");
    out = replace_all(out, "\"", "&quot;");
    out = replace_all(out, "'", "&apos;");
    return out;
}

std::string build_profile_xml(const std::string& ssid, const std::optional<std::string>& password) {
    const std::string s = xml_escape(ssid);
    std::string auth;
    std::string enc;
    std::string shared_key;
    if (password) {
        auth = "WPA2PSK";
        enc = "AES";
        shared_key =
            "<sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
            "<keyMaterial>" +
            xml_escape(*password) + "</keyMaterial></sharedKey>";
    } else {
        auth = "open";
        enc = "none";
    }
    return std::string("<?xml version=\"1.0\"?>") +
           "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">" +
           "<name>" + s + "</name>" + "<SSIDConfig><SSID><name>" + s +
           "</name></SSID></SSIDConfig>" +
           "<connectionType>ESS</connectionType><connectionMode>auto</connectionMode>" +
           "<MSM><security>" + "<authEncryption><authentication>" + auth + "</authentication>" +
           "<encryption>" + enc + "</encryption><useOneX>false</useOneX></authEncryption>" +
           shared_key + "</security></MSM></WLANProfile>";
}

} // namespace denso::network::wifi
