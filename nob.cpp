/* Calyx build driver: a small C++ implementation inspired by nob.h's process model. */
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if !defined(__linux__)
#error "The Calyx build driver currently requires Linux."
#endif

namespace {

volatile std::sig_atomic_t signal_received = 0;
void on_signal(int value) noexcept { signal_received = value; }

void info(std::string_view text) { std::cout << "[INFO] " << text << '\n'; }
void warn(std::string_view text) { std::cerr << "[WARN] " << text << '\n'; }
void error(std::string_view text) { std::cerr << "[ERROR] " << text << '\n'; }

struct Result {
    int code{1};
    std::string output;
    [[nodiscard]] bool ok() const noexcept { return code == 0; }
};

class Command final {
public:
    Command& add(std::string_view value) { args_.emplace_back(value); return *this; }
    Command& quiet() noexcept { quiet_stderr_ = true; return *this; }
    Command& buildkit() noexcept { buildkit_ = true; return *this; }
    [[nodiscard]] const std::vector<std::string>& args() const noexcept { return args_; }
    [[nodiscard]] bool quiet_stderr() const noexcept { return quiet_stderr_; }
    [[nodiscard]] bool use_buildkit() const noexcept { return buildkit_; }
private:
    std::vector<std::string> args_;
    bool quiet_stderr_{false};
    bool buildkit_{false};
};

class Process final {
public:
    [[nodiscard]] static Result run(const Command& command, bool capture = false)
    {
        const auto& args = command.args();
        if (args.empty()) { error("Cannot execute an empty command."); return {}; }
        std::string rendered;
        for (const auto& arg : args) {
            if (!rendered.empty()) rendered.push_back(' ');
            if (arg.find_first_of(" \t\n'\"\\$\x60!()[]{};&|<>*?") == std::string::npos && !arg.empty()) {
                rendered += arg;
            } else {
                rendered.push_back('\'');
                for (char ch : arg) { if (ch == '\'') rendered += "'\\''"; else rendered.push_back(ch); }
                rendered.push_back('\'');
            }
        }
        info(rendered);

        int fds[2]{-1, -1};
        if (capture && ::pipe(fds) < 0) { error(std::string("pipe: ") + std::strerror(errno)); return {}; }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        const pid_t pid = ::fork();
        if (pid < 0) {
            if (capture) { ::close(fds[0]); ::close(fds[1]); }
            error(std::string("fork: ") + std::strerror(errno)); return {};
        }
        if (pid == 0) {
            (void)::setpgid(0, 0);
            if (capture) {
                (void)::close(fds[0]);
                if (::dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
                (void)::close(fds[1]);
            }
            if (command.quiet_stderr()) {
                const int null_fd = ::open("/dev/null", O_WRONLY);
                if (null_fd >= 0) { (void)::dup2(null_fd, STDERR_FILENO); (void)::close(null_fd); }
            }
            if (command.use_buildkit()) (void)::setenv("DOCKER_BUILDKIT", "1", 1);
            ::execvp(argv[0], argv.data());
            (void)::dprintf(STDERR_FILENO, "exec %s: %s\n", argv[0], std::strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }
        (void)::setpgid(pid, pid);
        if (capture) (void)::close(fds[1]);

        Result result;
        if (capture) {
            std::array<char, 4096> buffer{};
            bool terminated = false;
            for (;;) {
                if (signal_received != 0 && !terminated) { (void)::kill(-pid, SIGTERM); terminated = true; }
                const ssize_t count = ::read(fds[0], buffer.data(), buffer.size());
                if (count > 0) { result.output.append(buffer.data(), static_cast<std::size_t>(count)); continue; }
                if (count == 0) break;
                if (errno == EINTR) continue;
                error(std::string("read: ") + std::strerror(errno)); break;
            }
            (void)::close(fds[0]);
        }
        int status = 0;
        for (;;) {
            if (::waitpid(pid, &status, 0) == pid) break;
            if (errno == EINTR) { if (signal_received != 0) (void)::kill(-pid, SIGTERM); continue; }
            error(std::string("waitpid: ") + std::strerror(errno)); return result;
        }
        if (WIFEXITED(status)) result.code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) result.code = 128 + WTERMSIG(status);
        return result;
    }
private:
    Process() = delete;
};

[[nodiscard]] std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
[[nodiscard]] std::vector<std::string> split_lines(std::string_view value)
{
    std::vector<std::string> result;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto end = value.find('\n', pos);
        auto line = trim(std::string(value.substr(pos, end == std::string_view::npos ? value.size() - pos : end - pos)));
        if (!line.empty()) result.push_back(std::move(line));
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    return result;
}
[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix)
{ return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix; }
[[nodiscard]] std::string host_arch()
{
    struct utsname data{};
    if (::uname(&data) != 0) throw std::runtime_error(std::string("uname: ") + std::strerror(errno));
    return data.machine;
}
[[nodiscard]] std::filesystem::path executable_path()
{
    std::array<char, 4096> buffer{};
    const ssize_t size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size < 0) throw std::runtime_error(std::string("readlink: ") + std::strerror(errno));
    return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(size)));
}
[[nodiscard]] std::string read_text(const std::filesystem::path& path)
{
    std::ifstream file(path);
    return file ? std::string(std::istreambuf_iterator<char>(file), {}) : std::string{};
}
[[nodiscard]] bool ensure_directory(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) { error("create directory '" + path.string() + "': " + ec.message()); return false; }
    return true;
}

struct Options {
    bool clean{}, fresh{}, rebuild{}, update{}, help{};
    std::string platform;
};
void help(std::string_view name)
{
    std::cout << "Usage: " << name << " [OPTIONS]\n\n"
              << "Build a fully static musl binary using Docker.\n\n"
              << "  -c, --clean            Remove Calyx Docker containers, volumes, and images\n"
              << "  -f, --fresh-build      Clear build cache only (forces CMake reconfiguration)\n"
              << "  -r, --rebuild-image    Rebuild toolchain image and clear all project caches\n"
              << "  -u, --update-image     Pull latest builder image from GHCR before compiling\n"
              << "  -p, --platform <arch>  Target Docker platform: linux/amd64 or linux/arm64\n"
              << "  -h, --help             Show this help message\n";
}
[[nodiscard]] bool parse(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "-c" || arg == "--clean") options.clean = true;
        else if (arg == "-f" || arg == "--fresh-build") options.fresh = true;
        else if (arg == "-r" || arg == "--rebuild-image") options.rebuild = true;
        else if (arg == "-u" || arg == "--update-image") options.update = true;
        else if (arg == "-h" || arg == "--help") options.help = true;
        else if (arg == "-p" || arg == "--platform") {
            if (++i >= argc) { error("--platform requires an argument."); return false; }
            options.platform = argv[i];
        } else { error("Unknown option: " + std::string(arg)); return false; }
    }
    if (!options.platform.empty() && options.platform != "linux/amd64" && options.platform != "linux/arm64") {
        error("Unsupported platform '" + options.platform + "'. Use linux/amd64 or linux/arm64."); return false;
    }
    return true;
}

class RunningContainer final {
public:
    explicit RunningContainer(std::string name) : name_(std::move(name)) {}
    RunningContainer(const RunningContainer&) = delete;
    RunningContainer& operator=(const RunningContainer&) = delete;
    ~RunningContainer() { stop(); }
    void stop() noexcept
    {
        if (!active_) return;
        Command cmd; cmd.add("docker").add("container").add("stop").add("-t").add("0").add(name_);
        (void)Process::run(cmd); active_ = false;
    }
    void remove_on_interrupt() noexcept
    {
        stop();
        Command cmd; cmd.add("docker").add("container").add("rm").add("-f").add(name_);
        (void)Process::run(cmd);
    }
private:
    std::string name_;
    bool active_{true};
};

class Build final {
public:
    Build(Options options, std::filesystem::path root)
        : options_(std::move(options)), root_(std::move(root)), source_mount_(root_.string() + ":/src:Z")
    {
        std::string suffix;
        if (!options_.platform.empty()) {
            suffix = options_.platform;
            for (char& ch : suffix) if (ch == '/') ch = '-';
            suffix.insert(0, "-");
        }
        image_ = "calyx-builder" + suffix;
        container_ = "calyx-compile" + suffix;
        build_volume_ = "calyx-build-cache" + suffix;
        ccache_volume_ = "calyx-ccache" + suffix;
        const char* remote = std::getenv("GHCR_IMAGE");
        ghcr_image_ = remote != nullptr && remote[0] != '\0' ? remote : "ghcr.io/relvinarsenio/calyx-builder";
    }

    [[nodiscard]] int run()
    {
        if (options_.clean) return clean() ? 0 : 1;
        if (!check_platform()) return 1;
        if (options_.rebuild) {
            info("Rebuilding toolchain image and clearing project caches.");
            if (!clean() || !build_image(true)) return 1;
        } else if (!resolve_image()) return 1;

        if (!prepare_container()) {
            if (signal_received != 0) {
                const int sig = signal_received; signal_received = 0;
                (void)remove_container(); return 128 + sig;
            }
            return 1;
        }
        RunningContainer running(container_);
        if (!build_binary()) {
            if (signal_received != 0) {
                const int sig = signal_received; signal_received = 0;
                running.remove_on_interrupt(); return 128 + sig;
            }
            return 1;
        }
        running.stop();
        return show_result() ? 0 : 1;
    }

private:
    [[nodiscard]] static Result run(const Command& cmd, bool capture = false) { return Process::run(cmd, capture); }
    [[nodiscard]] bool docker(const Command& cmd) const { return run(cmd).ok(); }
    [[nodiscard]] Result capture(const Command& cmd) const { return run(cmd, true); }
    [[nodiscard]] Command base(std::string_view sub, std::string_view action) const
    { Command cmd; cmd.add("docker").add(sub).add(action); return cmd; }

    [[nodiscard]] bool check_platform() const
    {
        const std::string host = host_arch();
        if (host != "x86_64" && host != "aarch64" && host != "arm64") {
            error("Unsupported host architecture '" + host + "'. Calyx supports x86_64 and ARM64."); return false;
        }
        if (options_.platform.empty()) return true;
        Command cmd; cmd.add("docker").add("buildx").add("ls");
        const Result result = capture(cmd);
        if (!result.ok()) { error("Docker Buildx is unavailable."); return false; }
        if (result.output.find(options_.platform) == std::string::npos) {
            error("Docker Buildx does not report support for '" + options_.platform + "'."); return false;
        }
        const bool target_arm = options_.platform == "linux/arm64";
        const bool host_arm = host == "aarch64" || host == "arm64";
        if (target_arm != host_arm && read_text("/proc/sys/fs/binfmt_misc/status").find("enabled") == std::string::npos) {
            warn("Cross-platform build requested but binfmt_misc is disabled; QEMU may fail.");
            warn("Try: docker run --privileged --rm tonistiigi/binfmt --install all");
        }
        return true;
    }

    [[nodiscard]] std::string ghcr_tag() const
    {
        if (options_.platform == "linux/arm64") return "arm64";
        if (options_.platform == "linux/amd64") return "amd64";
        const std::string host = host_arch();
        return host == "aarch64" || host == "arm64" ? "arm64" : "amd64";
    }
    [[nodiscard]] bool image_exists() const
    {
        Command cmd = base("image", "inspect"); cmd.add(image_).quiet();
        return capture(cmd).ok();
    }
    [[nodiscard]] bool container_exists() const
    {
        Command cmd = base("container", "inspect"); cmd.add(container_).quiet();
        return capture(cmd).ok();
    }
    [[nodiscard]] bool remove_container() const
    {
        if (!container_exists()) return true;
        Command cmd = base("container", "rm"); cmd.add("-f").add(container_);
        return docker(cmd);
    }
    [[nodiscard]] bool resolve_image()
    {
        if (!options_.update && image_exists()) return true;
        const std::string remote = ghcr_image_ + ":" + ghcr_tag();
        info("Pulling builder image " + remote);
        Command pull; pull.add("docker").add("pull");
        if (!options_.platform.empty()) pull.add("--platform").add(options_.platform);
        pull.add(remote);
        if (docker(pull)) {
            Command tag = base("image", "tag"); tag.add(remote).add(image_);
            return docker(tag) && remove_container();
        }
        warn("Could not pull the prebuilt image; building the toolchain locally.");
        return build_image(false);
    }
    [[nodiscard]] bool build_image(bool no_cache)
    {
        if (!remove_container()) return false;
        Command cmd = base("image", "build");
        cmd.add("--force-rm").buildkit();
        if (no_cache) cmd.add("--no-cache");
        if (!options_.platform.empty()) cmd.add("--platform").add(options_.platform);
        cmd.add("--tag").add(image_).add(root_.string());
        return docker(cmd);
    }
    [[nodiscard]] bool remove_volume(std::string_view name) const
    {
        Command inspect = base("volume", "inspect"); inspect.add(name).quiet();
        if (!capture(inspect).ok()) return true;
        Command remove = base("volume", "rm"); remove.add(name);
        return docker(remove);
    }
    [[nodiscard]] bool prepare_container()
    {
        if (options_.fresh) {
            info("Clearing build cache volume.");
            if (!remove_container() || !remove_volume(build_volume_)) return false;
        } else if (container_exists()) {
            Command inspect = base("container", "inspect");
            inspect.add("-f").add("{{range .Mounts}}{{if eq .Destination \"/src\"}}{{.Source}}{{end}}{{end}}").add(container_);
            const Result mount = capture(inspect);
            if (!mount.ok()) return false;
            if (trim(mount.output) != root_.string()) {
                warn("Existing container belongs to another source directory; recreating it.");
                if (!remove_container()) return false;
            }
        }
        if (!container_exists()) {
            Command create = base("container", "create");
            create.add("--name").add(container_)
                .add("--volume").add(source_mount_)
                .add("--volume").add(build_volume_ + ":/build")
                .add("--volume").add(ccache_volume_ + ":/root/.ccache")
                .add("--interactive");
            if (!options_.platform.empty()) create.add("--platform").add(options_.platform);
            create.add(image_).add("sh");
            if (!docker(create)) return false;
        }
        Command state = base("container", "inspect");
        state.add("-f").add("{{.State.Running}}").add(container_);
        const Result result = capture(state);
        if (!result.ok()) return false;
        if (trim(result.output) != "true") {
            Command start = base("container", "start"); start.add(container_);
            if (!docker(start)) return false;
        }
        return true;
    }
    [[nodiscard]] bool build_binary()
    {
        info("Compiling the static Calyx binary.");
        Command cache = base("container", "exec");
        cache.add(container_).add("test").add("-f").add("/build/CMakeCache.txt");
        if (!capture(cache).ok()) {
            info("Configuring CMake.");
            Command configure = base("container", "exec");
            configure.add(container_).add("cmake")
                .add("-B").add("/build").add("-S").add("/src")
                .add("-DCMAKE_POLICY_DEFAULT_CMP0149=NEW")
                .add("-DCMAKE_BUILD_TYPE=MinSizeRel")
                .add("-DCMAKE_CXX_COMPILER=clang++")
                .add("-DCMAKE_C_COMPILER=clang")
                .add("-DCMAKE_AR=llvm-ar")
                .add("-DCMAKE_RANLIB=llvm-ranlib")
                .add("-DCMAKE_LINKER_TYPE=LLD");
            if (!docker(configure)) return false;
        }
        Command build = base("container", "exec");
        build.add(container_).add("cmake").add("--build").add("/build").add("--parallel");
        if (!docker(build)) return false;
        Command strip = base("container", "exec");
        strip.add(container_).add("llvm-strip").add("/build/calyx");
        if (!docker(strip)) return false;
        const auto dist = root_ / "dist";
        if (!ensure_directory(dist)) return false;
        Command copy = base("container", "cp");
        copy.add(container_ + ":/build/calyx").add((dist / "calyx").string());
        if (!docker(copy)) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dist / "calyx", ec) || ec) {
            error("Docker reported success, but dist/calyx was not created."); return false;
        }
        return true;
    }
    [[nodiscard]] bool show_result() const
    {
        std::error_code ec;
        const auto path = root_ / "dist" / "calyx";
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) { error("Could not inspect generated binary: " + ec.message()); return false; }
        std::cout << "\nBuild complete.\nBinary: " << path.string() << "\nSize: " << size
                  << " bytes (" << (static_cast<double>(size) / (1024.0 * 1024.0)) << " MiB)\n";
        return true;
    }
    [[nodiscard]] bool clean() const
    {
        info("Removing Calyx-related Docker resources.");
        Command containers = base("container", "ls");
        containers.add("--all").add("--filter").add("name=calyx-").add("--format").add("{{.Names}}");
        const Result container_names = capture(containers);
        if (!container_names.ok()) return false;
        for (const auto& name : split_lines(container_names.output)) {
            if (!starts_with(name, "calyx-")) continue;
            Command remove = base("container", "rm"); remove.add("--force").add(name);
            if (!docker(remove)) return false;
        }
        Command volumes = base("volume", "ls");
        volumes.add("--filter").add("name=calyx-").add("--format").add("{{.Name}}");
        const Result volume_names = capture(volumes);
        if (!volume_names.ok()) return false;
        for (const auto& name : split_lines(volume_names.output)) {
            if (!starts_with(name, "calyx-")) continue;
            Command remove = base("volume", "rm"); remove.add(name);
            if (!docker(remove)) return false;
        }
        Command images = base("image", "ls"); images.add("--format").add("{{.Repository}}:{{.Tag}}");
        const Result image_names = capture(images);
        if (!image_names.ok()) return false;
        for (const auto& name : split_lines(image_names.output)) {
            if (!starts_with(name, "calyx-builder") && name.find("/calyx-") == std::string::npos) continue;
            Command remove = base("image", "rm"); remove.add("--force").add(name);
            if (!docker(remove)) return false;
        }
        info("Calyx Docker resources removed. Other Docker projects were left untouched.");
        return true;
    }

    Options options_;
    std::filesystem::path root_;
    std::string source_mount_, image_, container_, build_volume_, ccache_volume_, ghcr_image_;
};

[[nodiscard]] bool rebuild_self(const std::filesystem::path& exe, const std::filesystem::path& src, int argc, char** argv)
{
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec) || ec) return true;
    const auto source_time = std::filesystem::last_write_time(src, ec);
    if (ec) { error("Could not inspect nob.cpp timestamp: " + ec.message()); return false; }
    const auto binary_time = std::filesystem::last_write_time(exe, ec);
    if (ec) { error("Could not inspect nob timestamp: " + ec.message()); return false; }
    if (source_time <= binary_time) return true;

    const char* env_cxx = std::getenv("CXX");
    const std::string cxx = env_cxx != nullptr && env_cxx[0] != '\0' ? env_cxx : "c++";
    const auto temp = std::filesystem::path(exe.string() + ".new");
    const auto backup = std::filesystem::path(exe.string() + ".old");
    std::filesystem::remove(temp, ec);
    info("nob.cpp changed; rebuilding the build driver.");
    Command compile;
    compile.add(cxx).add("-std=c++20").add("-O2").add("-Wall").add("-Wextra").add("-Wpedantic")
        .add("-Wconversion").add("-Wsign-conversion").add("-Wshadow")
        .add("-o").add(temp.string()).add(src.string());
    if (!Process::run(compile).ok()) { std::filesystem::remove(temp, ec); return false; }

    const bool had_old = std::filesystem::exists(exe, ec) && !ec;
    if (had_old) {
        std::filesystem::remove(backup, ec); ec.clear();
        std::filesystem::rename(exe, backup, ec);
        if (ec) { error("Could not preserve previous nob: " + ec.message()); std::filesystem::remove(temp); return false; }
    }
    std::filesystem::rename(temp, exe, ec);
    if (ec) {
        error("Could not install rebuilt nob: " + ec.message());
        if (had_old) { std::error_code restore; std::filesystem::rename(backup, exe, restore); }
        std::filesystem::remove(temp); return false;
    }
    std::vector<char*> args;
    args.reserve(static_cast<std::size_t>(argc) + 1);
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);
    ::execv(exe.c_str(), args.data());
    error(std::string("Could not restart rebuilt nob: ") + std::strerror(errno));
    std::filesystem::remove(exe, ec);
    if (had_old) { ec.clear(); std::filesystem::rename(backup, exe, ec); }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try {
        const auto exe = executable_path();
        const auto root = exe.parent_path();
        std::error_code ec;
        std::filesystem::remove(exe.string() + ".old", ec);
        if (!rebuild_self(exe, root / "nob.cpp", argc, argv)) return 1;
        Options options;
        if (!parse(argc, argv, options)) { help(exe.filename().string()); return 2; }
        if (options.help) { help(exe.filename().string()); return 0; }
        Build build(std::move(options), root);
        return build.run();
    } catch (const std::exception& exception) {
        error(exception.what()); return 1;
    }
}
