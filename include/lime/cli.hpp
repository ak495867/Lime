#ifndef LIME_CLI_HPP
#define LIME_CLI_HPP

#include <string>
#include <vector>

namespace lime {

class CLI {
public:
    static int run(int argc, char** argv);

private:
    static void print_usage();
    static int handle_run(const std::vector<std::string>& args);
    static int handle_build(const std::vector<std::string>& args);
    static int handle_inspect(const std::vector<std::string>& args);
    static int handle_stats(const std::vector<std::string>& args);
    static int handle_overlay(const std::vector<std::string>& args);
    static size_t parse_size(const std::string& str);
};

}

#endif
