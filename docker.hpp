#include <arpa/inet.h>
#include <fcntl.h>     // open
#include <net/if.h>
#include <sched.h>     // clone
#include <stdlib.h>
#include <sys/mount.h> // mount
#include <sys/stat.h>  // mkdir
#include <sys/wait.h>  // waitpid
#include <unistd.h>    // execv, sethostname, chroot, fchdir

#include <cstring>
#include <fstream>
#include <iostream>
#include <string> // std::string

#include "network/network.h"

#define STACK_SIZE (512 * 512) // 定义子进程空间大小

namespace docker {
const std::string CgroupMemoryHierarchyMount = "/sys/fs/cgroup/memory/";
const std::string CgroupCPUHierarchyMount = "/sys/fs/cgroup/cpu/";

typedef int proc_statu;
proc_statu proc_err = -1;
proc_statu proc_exit = 0;
proc_statu proc_wait = 1;
// docker 容器启动配置
typedef struct container_config {
    std::string host_name;   // 主机名
    std::string root_dir;    // 容器根目录
    std::string ip;          // 容器 IP
    std::string bridge_name; // 网桥名
    std::string bridge_ip;   // 网桥 IP
    std::string memory_size; // 限制内存大小
    std::string cpu_quota;   // 设置 CPU 限制
} container_config;

class container {
private:
    // 可读性增强
    typedef int process_pid;

    // 子进程栈
    char child_stack[STACK_SIZE];

    // 容器配置
    container_config config;

    // 命令
    std::string bash;

    // 保存容器网络设备, 用于删除
    char *veth1;
    char *veth2;

public:
    container(container_config &config) { this->config = config; }

    ~container() {
        // 退出时，记得删除创建的虚拟网络设备
        lxc_netdev_delete_by_name(veth1);
        lxc_netdev_delete_by_name(veth2);
    }

    void start(std::string bash) {
        this->bash = bash;

        char veth1buf[IFNAMSIZ] = "mydocker0X";
        char veth2buf[IFNAMSIZ] = "mydocker0X";

        /*创建一对网络设备, 一个用于加载到宿主机,
         * 另一个用于转移到子进程容器中lxc_mkifname 这个API
         * 在网络设备名字后面至少需要添加一个"X"
         * 来支持随机创建虚拟网络设备用于保证网络设备的正确创建, 详见 network.c
         * 中对 lxc_mkifname 的实现*/
        veth1 = lxc_mkifname(veth1buf);
        veth2 = lxc_mkifname(veth2buf);
        lxc_veth_create(veth1, veth2);

        // 设置 veth1 的 MAC 地址
        setup_private_host_hw_addr(veth1);

        // 将 veth1 添加到网桥中
        lxc_bridge_attach(config.bridge_name.c_str(), veth1);

        // 激活 veth1
        lxc_netdev_up(veth1);

        // system("ip addr add 192.168.48.1/24 dev mydocker0X");

        auto setup = [](void *args) -> int {
            auto _this = reinterpret_cast<container *>(args);

            // 对容器进行相关配置
            _this->set_hostname();
            _this->set_rootdir();
            _this->set_procsys();
            _this->set_network();
            // 启动 bash
            _this->start_bash();

            return proc_wait;
        };

        process_pid child_pid = clone(setup, child_stack + STACK_SIZE, // 移动到栈底
                                      CLONE_NEWUTS |                   // UTS   namespace
                                          CLONE_NEWNS |                // Mount namespace
                                          CLONE_NEWPID |               // PID   namespace
                                          CLONE_NEWNET |               // Net   namespace
                                          SIGCHLD,                     // 子进程退出时会发出信号给父进程
                                      this);
        // 将 veth2 转移到容器内部, 并命名为 eth0
        lxc_netdev_move_by_name(veth2, child_pid, "eth0");

        // 设置限制内存大小
        // set_memory(child_pid);

        // 设置限制CPU占用率
        set_cpu_quota(child_pid);

        waitpid(child_pid, nullptr, 0); // 等待子进程的退出
    }

private:
    void start_bash() {
        // 将 C++ std::string 安全的转换为 C 风格的字符串 char *
        // 从 C++14 开始, C++编译器将禁止这种写法 `char *str = "test";`
        char *c_bash = new char[bash.length() + 1]; // +1 用于存放 '\0'
        strcpy(c_bash, bash.c_str());

        char *const child_args[] = {c_bash, NULL};
        execv(child_args[0], child_args); // 在子进程中执行 /bin/bash
        delete[] c_bash;
    }

    // 设置容器主机名
    void set_hostname() { sethostname(this->config.host_name.c_str(), this->config.host_name.length()); }

    // 设置根目录
    void set_rootdir() {
        // chdir 系统调用, 切换到某个目录下
        chdir(this->config.root_dir.c_str());

        // chrrot 系统调用, 设置根目录, 因为刚才已经切换过当前目录
        // 故直接使用当前目录作为根目录即可
        chroot(".");
    }

    // 设置独立的进程空间
    void set_procsys() {
        // 挂载 proc 文件系统
        mount("none", "/proc", "proc", 0, nullptr);
        mount("none", "/sys", "sysfs", 0, nullptr);
    }

    void set_network() {
        int ifindex = if_nametoindex("eth0");
        struct in_addr ipv4;
        struct in_addr bcast;
        struct in_addr gateway;

        int prefix = 16;
        // IP 地址转换函数，将 IP 地址在点分十进制和二进制之间进行转换
        inet_pton(AF_INET, this->config.ip.c_str(), &ipv4);
        inet_pton(AF_INET, "255.255.255.0", &bcast);
        inet_pton(AF_INET, this->config.bridge_ip.c_str(), &gateway);

        // 激活 lo
        lxc_netdev_up("lo");

        // 激活 eth0
        lxc_netdev_up("eth0");

        // 配置 eth0 IP 地址
        lxc_ipv4_addr_add(ifindex, &ipv4, &bcast, prefix);

        system("ip addr add 172.17.0.100/24 dev eth0"); // 配置IP地址

        // 设置网关
        lxc_ipv4_gateway_add(ifindex, &gateway);

        // 设置 eth0 的 MAC 地址
        char mac[18];
        new_hwaddr(mac);
        setup_hw_addr(mac, "eth0");
    }

    void set_memory(process_pid pid) {
        std::cout << "child pid = " << pid << std::endl;
        // 创建一个子cgroup，在默认的memory cgroup目录下
        mkdir((CgroupMemoryHierarchyMount + "memorylimit").c_str(), 0755);

        // 将容器加入到 cgroup 中，即将进程 PID 加入到 cgroup 中的 cgroup.procs 文件中
        std::ofstream file(CgroupMemoryHierarchyMount + "memorylimit/cgroup.procs", std::ios::out | std::ios::app);
        if (file.is_open()) {
            file << std::to_string(pid) << std::endl;
            file.close();
        } else {
            std::cout << "Failed to open the file." << std::endl;
        }

        // 限制进程的内存使用，即向 memory.limit_in_bytes 文件中写入数据
        std::ofstream file2(CgroupMemoryHierarchyMount + "memorylimit/memory.limit_in_bytes", std::ios::out);
        if (file2.is_open()) {
            file2 << config.memory_size << std::endl;
        } else {
            std::cout << "Failed to open the file." << std::endl;
        }
    }

    void set_cpu_quota(process_pid pid) {
        std::cout << "child pid = " << pid << std::endl;
        // 创建一个子cgroup，在默认的 cgroup目录下
        mkdir((CgroupCPUHierarchyMount + "cpulimit").c_str(), 0755);

        // 将容器加入到 cgroup 中，即将进程 PID 加入到 cgroup 中的 cgroup.procs 文件中
        std::ofstream file(CgroupCPUHierarchyMount + "cpulimit/cgroup.procs", std::ios::out | std::ios::app);
        if (file.is_open()) {
            file << std::to_string(pid) << std::endl;
            file.close();
        } else {
            std::cout << "Failed to open the file." << std::endl;
        }

        // 限制进程的内存使用，即向 memory.limit_in_bytes 文件中写入数据
        std::ofstream file2(CgroupCPUHierarchyMount + "cpulimit/cpu.cfs_quota_us", std::ios::out);
        if (file2.is_open()) {
            file2 << std::to_string((int)(std::stof(config.cpu_quota) * 100000)) << std::endl;
            std::cout << std::to_string((int)(std::stof(config.cpu_quota) * 100000)) << std::endl;
        } else {
            std::cout << "Failed to open the file." << std::endl;
        }
    }
};
} // namespace docker
