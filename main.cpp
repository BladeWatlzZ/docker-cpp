
#include <iostream>

#include "docker.hpp"

int main(int argc, char** argv) {
    std::cout << "...start container" << std::endl;
    docker::container_config config;
    config.host_name = "mydocker";
    config.root_dir = "./rootfs";
    // 配置网络参数
    config.ip = "172.17.0.100";      // 容器 IP
    config.bridge_name = "docker0";  // 宿主机网桥
    config.bridge_ip = "172.17.0.1"; // 宿主机网桥 IP
    // config.memory_size = "100m";     // 限制内存大小
    config.cpu_quota = "0.5"; // 限制 CPU

    // 配置容器
    docker::container container(config); // 根据 config 构造容器
    // 判断命令
    if (strcmp(argv[1], "run") == 0) {
        if (argc > 3) {
            std::string str = argv[2];
            // config.memory_size = str.substr(str.find('=') + 1);
            config.cpu_quota = str.substr(str.find('=') + 1);
            std::cout << config.cpu_quota << std::endl;
        }
        std::string cmd = argv[argc - 1];
        container.start(cmd); // 启动容器
    } else {
        std::cout << "have not defined!" << std::endl;
    }
    std::cout << "stop container..." << std::endl;
    return 0;
}
