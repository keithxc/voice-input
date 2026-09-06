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
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
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
        running_ = true;
        thread_ = std::thread([this] { listenLoop(); });
    }

    ~VoiceInputAddon() override {
        running_ = false;
        if (server_ >= 0) {
            shutdown(server_, SHUT_RDWR);
            close(server_);
            server_ = -1;
        }
        if (thread_.joinable()) thread_.join();
        unlink(socketPath().c_str());
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

    void commit(std::string text) {
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
        } else {
            FCITX_LOGC(voiceInputLog, Warn)
                << "No focused input context; transcript was not committed";
        }
    }

    void handle(int client) {
        uint32_t networkLength = 0;
        if (!receiveAll(client, &networkLength, sizeof(networkLength))) return;
        const uint32_t length = ntohl(networkLength);
        if (length == 0 || length > 65535U) return;
        std::string text(length, '\0');
        if (!receiveAll(client, text.data(), text.size())) return;
        dispatcher_.schedule([this, text = std::move(text)]() mutable {
            commit(std::move(text));
        });
        const unsigned char acknowledgment = 1;
        (void)send(client, &acknowledgment, 1, MSG_NOSIGNAL);
    }

    void listenLoop() {
        const std::string path = socketPath();
        const size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            const std::string directory = path.substr(0, slash);
            if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) return;
        }
        unlink(path.c_str());
        server_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (server_ < 0) return;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) return;
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (bind(server_, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)) < 0 ||
            chmod(path.c_str(), 0600) < 0 || listen(server_, 4) < 0) {
            return;
        }
        while (running_) {
            int client = accept4(server_, nullptr, nullptr, SOCK_CLOEXEC);
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
};

class VoiceInputAddonFactory final : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new VoiceInputAddon(manager->instance());
    }
};

} // namespace fcitx

FCITX_ADDON_FACTORY(fcitx::VoiceInputAddonFactory)
