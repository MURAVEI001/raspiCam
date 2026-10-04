#include "config.hpp"
#include "log.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mjpeg {

static std::string trim(std::string s) {
    auto notspace = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

bool load_config(const std::string& path, Config& out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_WARN("config '%s' not found, using defaults", path.c_str());
        return false;
    }

    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        // убрать комментарии
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) {
            LOG_WARN("config line %d: missing '=', skipping", lineno);
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        try {
            if      (key == "device")        out.device        = val;
            else if (key == "width")         out.width         = std::stoi(val);
            else if (key == "height")        out.height        = std::stoi(val);
            else if (key == "fps")           out.fps           = std::stoi(val);
            else if (key == "buffer_count")  out.buffer_count  = std::stoi(val);
            else if (key == "jpeg_quality")  out.jpeg_quality  = std::stoi(val);
            else if (key == "server_host")   out.server_host   = val;
            else if (key == "server_port")   out.server_port   = std::stoi(val);
            else if (key == "reconnect_ms")  out.reconnect_ms  = std::stoi(val);
            else if (key == "watchdog_ms")   out.watchdog_ms   = std::stoi(val);
            else if (key == "verbose")       out.verbose       = (val == "1" || val == "true");
            else LOG_WARN("config line %d: unknown key '%s'", lineno, key.c_str());
        } catch (const std::exception& e) {
            LOG_WARN("config line %d: bad value for '%s': %s", lineno, key.c_str(), e.what());
        }
    }
    return true;
}

} // namespace mjpeg