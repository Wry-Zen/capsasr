// CapsASR fcitx5 文本提交模块。
//
// fcitx5 没有对外公开的"提交文本"接口（D-Bus Controller1 无此方法），
// 本 addon 在 fcitx5 进程内监听 Unix socket，收到文本后经
// InputContext::commitString 提交给当前焦点输入框——与 fcitx5 自带
// clipboard 模块选词同一条路径，兼容 wayland-im/XIM/GTK/Qt 全部前端。
//
// 协议（SOCK_STREAM，一行一请求）:
//   请求: 8 位十六进制长度 + '\n' + UTF-8 字节（长度 0 = 探测）
//   应答: "OK\n" 已提交 | "NOIC\n" 无焦点上下文 | "BAD\n" 格式错 | "TIMEOUT\n"
// socket: $XDG_RUNTIME_DIR/capsasr-commit.sock，权限 0600，仅同用户可写。

#include <fcitx-utils/log.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/instance.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <thread>

namespace {

constexpr char kSocketName[] = "capsasr-commit.sock";
constexpr size_t kMaxText = 64 * 1024;
constexpr auto kCommitTimeout = std::chrono::milliseconds(500);

bool readAll(int fd, void *buf, size_t n) {
    auto *p = static_cast<char *>(buf);
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

} // namespace

class CapsasrCommitModule : public fcitx::AddonInstance {
public:
    explicit CapsasrCommitModule(fcitx::Instance *instance)
        : instance_(instance), dispatcher_(instance->eventDispatcher()) {
        const char *runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            FCITX_INFO() << "capsasrcommit: XDG_RUNTIME_DIR 未设置，模块停用";
            return;
        }
        socketPath_ = std::string(runtime) + "/" + kSocketName;
        running_ = true;
        if (acquireSocket()) {
            listenThread_ = std::thread(&CapsasrCommitModule::listenLoop, this);
            FCITX_INFO() << "capsasrcommit: 监听 " << socketPath_;
        } else {
            // 让位给已在监听的实例（登录时 KWin 托管与 autostart 可能同时拉起
            // 两个 fcitx5）；看门狗等持有者退出后接管，覆盖 fcitx5 -rd 替换时序
            watchdogThread_ = std::thread(&CapsasrCommitModule::watchdogLoop, this);
            FCITX_INFO() << "capsasrcommit: socket 由其他实例持有，看门狗待命";
        }
    }

    ~CapsasrCommitModule() override {
        running_ = false;
        if (fd_ >= 0) {
            shutdown(fd_, SHUT_RDWR);
            close(fd_);
            fd_ = -1;
        }
        if (listenThread_.joinable()) {
            listenThread_.join();
        }
        if (watchdogThread_.joinable()) {
            watchdogThread_.join();
        }
        if (!socketPath_.empty() && socketIno_ != 0) {
            // 只删自己绑定的 socket（inode 匹配）；被后来实例接管时不误删
            struct stat st{};
            if (stat(socketPath_.c_str(), &st) == 0 &&
                static_cast<ino_t>(st.st_ino) == socketIno_) {
                unlink(socketPath_.c_str());
            }
        }
    }

private:
    bool pathHasLiveListener() {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return true; // 保守：无法判定时当作有人持有
        }
        timeval tv{0, 200 * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            close(fd);
            return false; // ECONNREFUSED 等：残留文件，无监听者
        }
        static constexpr char kPing[] = "00000000\n";
        send(fd, kPing, sizeof(kPing) - 1, MSG_NOSIGNAL);
        char buf[8] = {};
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        close(fd);
        return n > 0; // 有响应即视为活监听者（不苛求 PONG，宁可让位也不抢）
    }

    bool acquireSocket() {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);
        if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            // 路径被占：活实例在监听则让位；仅残留文件（崩溃/被杀）才清理重试
            if (pathHasLiveListener()) {
                close(fd);
                return false;
            }
            unlink(socketPath_.c_str());
            if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                close(fd);
                return false;
            }
        }
        if (listen(fd, 4) != 0) {
            close(fd);
            return false;
        }
        struct stat st{};
        if (fstat(fd, &st) == 0) {
            socketIno_ = static_cast<ino_t>(st.st_ino);
        }
        chmod(socketPath_.c_str(), 0600);
        fd_ = fd;
        return true;
    }

    void watchdogLoop() {
        while (running_) {
            for (int i = 0; i < 20 && running_; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_) {
                break;
            }
            if (acquireSocket()) {
                listenThread_ = std::thread(&CapsasrCommitModule::listenLoop, this);
                FCITX_INFO() << "capsasrcommit: 看门狗接管 " << socketPath_;
                return;
            }
        }
    }

    void listenLoop() {
        while (running_) {
            int c = accept(fd_, nullptr, nullptr);
            if (c < 0) {
                if (!running_) {
                    break;
                }
                continue;
            }
            handleClient(c);
            close(c);
        }
    }

    void handleClient(int c) {
        char header[8];
        if (!readAll(c, header, sizeof(header))) {
            return;
        }
        unsigned long len = std::strtoul(std::string(header, 8).c_str(), nullptr, 16);
        char sep;
        if (!readAll(c, &sep, 1) || sep != '\n' || len > kMaxText) {
            sendAll(c, "BAD\n");
            return;
        }
        if (len == 0) {
            sendAll(c, "PONG\n");
            return;
        }

        std::string text(len, '\0');
        if (!readAll(c, text.data(), len)) {
            return;
        }

        // commitString 必须在 fcitx5 事件循环线程执行；
        // promise 用 shared_ptr：主线程回填晚于本线程超时返回时依然安全
        auto prom = std::make_shared<std::promise<std::string>>();
        auto fut = prom->get_future();
        dispatcher_.schedule([this, prom, text]() {
            std::string result;
            try {
                result = commitNow(text);
            } catch (const std::exception &e) {
                FCITX_INFO() << "capsasrcommit: 提交异常 " << e.what();
                result = "BAD";
            }
            prom->set_value(result);
        });
        if (fut.wait_for(kCommitTimeout) == std::future_status::ready) {
            sendAll(c, fut.get() + "\n");
        } else {
            sendAll(c, "TIMEOUT\n");
        }
    }

    std::string commitNow(const std::string &text) {
        auto *ic = instance_->inputContextManager().lastFocusedInputContext();
        if (!ic) {
            return "NOIC";
        }
        ic->commitString(text);
        return "OK";
    }

    static void sendAll(int fd, const std::string &data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t r = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (r <= 0) {
                return;
            }
            off += static_cast<size_t>(r);
        }
    }

    fcitx::Instance *instance_;
    fcitx::EventDispatcher &dispatcher_;
    std::string socketPath_;
    ino_t socketIno_ = 0;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread listenThread_;
    std::thread watchdogThread_;
};

class CapsasrCommitFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new CapsasrCommitModule(manager->instance());
    }
};

FCITX_ADDON_FACTORY(CapsasrCommitFactory);
