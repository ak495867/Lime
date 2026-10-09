#ifndef LIME_UEFI_SHELL_HPP
#define LIME_UEFI_SHELL_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "lime/memory.hpp"

namespace lime {

class UEFIShellLoader {
public:
    static bool load_shell_application(std::shared_ptr<MemoryManager> mem, 
                                       const std::string& app_name,
                                       uint64_t entry_gpa,
                                       std::function<void()> on_complete);
    static bool execute_shell_command(std::shared_ptr<MemoryManager> mem,
                                      const std::string& command);
    static bool load_shell_script(std::shared_ptr<MemoryManager> mem,
                                  const std::vector<uint8_t>& script);
    static bool unload_application(uint64_t handle);

private:
    struct AppInfo {
        std::string name;
        uint64_t entry_gpa;
        uint64_t handle;
        bool active;
    };
    static std::vector<AppInfo> active_apps_;
    static std::mutex apps_mutex_;
};

}

#endif
