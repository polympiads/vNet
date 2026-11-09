#include "common/sysutil.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

bool run_program(const std::vector<std::string>& argv, const char* tag,
                 bool quiet) {
    if (argv.empty()) return false;

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << tag << " fork: " << strerror(errno) << "\n";
        return false;
    }
    if (pid == 0) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp(args[0], args.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            std::cerr << tag << " waitpid: " << strerror(errno) << "\n";
            return false;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    if (quiet) return false;

    std::string line;
    for (const auto& a : argv) line += (line.empty() ? "" : " ") + a;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        std::cerr << tag << " Program not found: " << argv[0] << "\n";
    else
        std::cerr << tag << " Command failed (" << WEXITSTATUS(status)
                  << "): " << line << "\n";
    return false;
}

bool env_int(const char* name, long min, long max, long& value) {
    const char* text = std::getenv(name);
    if (!text || !*text) return true;

    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        std::cerr << "[Config] " << name << " must be an integer between "
                  << min << " and " << max << ", got '" << text << "'\n";
        return false;
    }
    value = parsed;
    return true;
}

std::string env_or(const char* name, const std::string& def) {
    const char* text = std::getenv(name);
    return (text && *text) ? std::string(text) : def;
}

bool valid_name(const std::string& name, size_t max_len) {
    if (name.empty() || name.size() > max_len) return false;
    for (unsigned char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool parse_cidr(const std::string& text, uint32_t& addr, int& prefix) {
    std::string ip = text;
    prefix = 32;
    size_t slash = text.find('/');
    if (slash != std::string::npos) {
        ip = text.substr(0, slash);
        std::string rest = text.substr(slash + 1);
        if (rest.empty() || rest.size() > 2) return false;
        for (char c : rest) if (c < '0' || c > '9') return false;
        prefix = std::atoi(rest.c_str());
        if (prefix < 0 || prefix > 32) return false;
    }
    uint32_t parsed = 0;
    if (inet_pton(AF_INET, ip.c_str(), &parsed) != 1) return false;
    addr = parsed;
    return true;
}

std::string load_auth_key(const char* argv_key, const char* tag) {
    const char* env = std::getenv("VNET_AUTH_KEY");
    if (env && *env) return env;

    const char* path = std::getenv("VNET_AUTH_KEY_FILE");
    if (path && *path) {
        std::ifstream in(path);
        std::string line;
        if (!in || !std::getline(in, line)) {
            std::cerr << tag << " Cannot read " << path << "\n";
            return "";
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        return line;
    }

    if (argv_key && *argv_key) {
        std::cerr << tag << " The auth key on the command line is visible "
                  << "to every local user; prefer VNET_AUTH_KEY_FILE\n";
        return argv_key;
    }
    return "";
}
