#include "lime/uefi_shell.hpp"
#include <iostream>

namespace lime {

std::vector<UEFIShellLoader::AppInfo> UEFIShellLoader::active_apps_{};
std::mutex UEFIShellLoader::apps_mutex_{};

bool UEFIShellLoader::load_shell_application(std::shared_ptr<MemoryManager> mem,
                                              const std::string& app_name,
                                              uint64_t entry_gpa,
                                              std::function<void()> on_complete) {
    std::lock_guard<std::mutex> lock(apps_mutex_);






    
    AppInfo info;
    info.name = app_name;
    info.entry_gpa = entry_gpa;
    info.handle = active_apps_.size() + 1;
    info.active = true;
    
    active_apps_.push_back(info);

    std::cout << "[UEFI Shell] Loaded application: " << app_name 
              << " at GPA 0x" << std::hex << entry_gpa << std::dec << std::endl;
    
    return true;
}

bool UEFIShellLoader::execute_shell_command(std::shared_ptr<MemoryManager> mem,
                                             const std::string& command) {
    std::lock_guard<std::mutex> lock(apps_mutex_);


    std::cout << "[UEFI Shell] Executing command: " << command << std::endl;
    
    return true;
}

bool UEFIShellLoader::load_shell_script(std::shared_ptr<MemoryManager> mem,
                                         const std::vector<uint8_t>& script) {
    std::lock_guard<std::mutex> lock(apps_mutex_);


    std::string script_str(script.begin(), script.end());
    std::cout << "[UEFI Shell] Loading script (" << script.size() << " bytes): " 
              << script_str.substr(0, std::min<size_t>(script.size(), 64)) << std::endl;
    
    return true;
}

bool UEFIShellLoader::unload_application(uint64_t handle) {
    std::lock_guard<std::mutex> lock(apps_mutex_);
    
    for (auto& app : active_apps_) {
        if (app.handle == handle) {
            app.active = false;
            return true;
        }
    }
    
    return false;
}

};  
