#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/instance.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <utility>
#include <unistd.h>

namespace fcitx {

FCITX_DEFINE_LOG_CATEGORY(voiceInputLog, "voice-input")

class VoiceInputAddon final : public AddonInstance {
public:
    explicit VoiceInputAddon(Instance *instance) : instance_(instance) {
        dispatcher_.attach(&instance_->eventLoop());
        if (!prepareSocket()) {
            FCITX_LOGC(voiceInputLog, Warn)
                << "Another voice-input addon owns the output socket; "
                   "leaving this duplicate instance inactive";
            return;
        }
        running_ = true;
        thread_ = std::thread([this] { listenLoop(server_); });
    }

    ~VoiceInputAddon() override {
        running_ = false;
        if (server_ >= 0) {
            shutdown(server_, SHUT_RDWR);
            close(server_);
        }
        if (thread_.joinable()) thread_.join();
        if (lock_ >= 0) {
            unlink(socketPath().c_str());
            close(lock_);
        }
        dispatcher_.detach();
    }

private:
    std::string socketPath() const {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        return std::string(runtime != nullptr ? runtime : "/tmp") +
               "/voice-input/fcitx5.sock";
    }

    static bool receiveAll(int fd, void *data, size_t size) {
        auto *cursor = static_cast<unsigned char *>(data);
        while (size > 0) {
            ssize_t count = recv(fd, cursor, size, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            cursor += count;
            size -= static_cast<size_t>(count);
        }
        return true;
    }

    bool commit(std::string text) {
        InputContext *context = instance_->mostRecentInputContext();
        if (context == nullptr || !context->hasFocus()) {
            context = nullptr;
            instance_->inputContextManager().foreach(
                [&context](InputContext *candidate) {
                    if (candidate != nullptr && candidate->hasFocus()) {
                        context = candidate;
                        return false;
                    }
                    return true;
                });
        }
        if (context != nullptr) {
            context->commitString(text);
            return true;
        } else {
            FCITX_LOGC(voiceInputLog, Warn)
                << "No focused input context; transcript was not committed";
            return false;
        }
    }

    void handle(int client) {
        uint32_t networkLength = 0;
        if (!receiveAll(client, &networkLength, sizeof(networkLength))) return;
        const uint32_t length = ntohl(networkLength);
        if (length == 0 || length > 65535U) return;
        std::string text(length, '\0');
        if (!receiveAll(client, text.data(), text.size())) return;
        auto completion = std::make_shared<std::promise<bool>>();
        std::future<bool> result = completion->get_future();
        dispatcher_.schedule([this, text = std::move(text), completion]() mutable {
            completion->set_value(commit(std::move(text)));
        });
        const bool committed = result.wait_for(std::chrono::seconds(1)) ==
                                   std::future_status::ready &&
                               result.get();
        const unsigned char acknowledgment = committed ? 1 : 0;
        (void)send(client, &acknowledgment, 1, MSG_NOSIGNAL);
    }

    bool prepareSocket() {
        const std::string path = socketPath();
        const size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            const std::string directory = path.substr(0, slash);
            if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) return false;
        }

        const std::string lockPath = path + ".lock";
        lock_ = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lock_ < 0 || flock(lock_, LOCK_EX | LOCK_NB) < 0) {
            if (lock_ >= 0) close(lock_);
            lock_ = -1;
            return false;
        }

        // Only the process holding the lock may replace the socket path. This
        // prevents a short-lived duplicate Fcitx process from unlinking the
        // healthy instance's output endpoint during a desktop reconfiguration.
        unlink(path.c_str());
        server_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (server_ < 0) return false;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) return false;
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (bind(server_, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)) < 0 ||
            chmod(path.c_str(), 0600) < 0 || listen(server_, 4) < 0) {
            return false;
        }
        return true;
    }

    void listenLoop(int server) {
        while (running_) {
            int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                if (errno == EINTR) continue;
                break;
            }
            handle(client);
            close(client);
        }
    }

    Instance *instance_;
    EventDispatcher dispatcher_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int server_{-1};
    int lock_{-1};
};

class VoiceInputAddonFactory final : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new VoiceInputAddon(manager->instance());
    }
};

} // namespace fcitx

FCITX_ADDON_FACTORY(fcitx::VoiceInputAddonFactory)
