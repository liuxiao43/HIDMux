#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <unistd.h>

struct VirtualMouse {
    uint8_t report_id;
    uint8_t buttons;
    int16_t x;
    int16_t y;
    int8_t  wheel;
} __attribute__((packed));

struct VirtualKeyboard {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keys[6];
} __attribute__((packed));

struct Config {
    std::string mouse_in = "/dev/hidraw0";
    std::string key_in = "/dev/hidraw1";
    std::string mouse_out = "/dev/hidg0";
    std::string key_out = "/dev/hidg1";

    std::string bind_addr = "127.0.0.1";
    uint16_t udp_port = 4242;
    bool allow_remote = false;

    useconds_t poll_us = 1000;
};

static std::atomic<int32_t> g_net_dx{0};
static std::atomic<int32_t> g_net_dy{0};
static std::atomic<int32_t> g_net_wheel{0};
static std::atomic<uint8_t> g_net_buttons{0};

static void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  --mouse-in PATH       Physical mouse hidraw input device\n"
        << "                        Default: /dev/hidraw0\n"
        << "  --key-in PATH         Physical keyboard/button hidraw input device\n"
        << "                        Default: /dev/hidraw1\n"
        << "  --mouse-out PATH      Virtual mouse HID gadget output device\n"
        << "                        Default: /dev/hidg0\n"
        << "  --key-out PATH        Virtual keyboard HID gadget output device\n"
        << "                        Default: /dev/hidg1\n"
        << "  --bind ADDRESS        UDP bind address\n"
        << "                        Default: 127.0.0.1\n"
        << "  --udp-port PORT       UDP port for network mouse packets\n"
        << "                        Default: 4242\n"
        << "  --allow-remote        Allow non-local UDP bind addresses\n"
        << "  --poll-us MICROSEC    Poll interval in microseconds\n"
        << "                        Default: 1000\n"
        << "  --help                Show this help text\n\n"
        << "Network packet format:\n"
        << "  6 bytes total:\n"
        << "    bytes 0-1: dx, int16 little-endian\n"
        << "    bytes 2-3: dy, int16 little-endian\n"
        << "    byte  4:   wheel, int8\n"
        << "    byte  5:   buttons, uint8\n";
}

static uint16_t parse_u16_arg(const std::string& value, const std::string& name) {
    try {
        unsigned long parsed = std::stoul(value, nullptr, 0);
        if (parsed > 65535) {
            throw std::out_of_range("value too large");
        }
        return static_cast<uint16_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for " + name + ": " + value);
    }
}

static useconds_t parse_poll_arg(const std::string& value) {
    try {
        unsigned long parsed = std::stoul(value, nullptr, 0);
        if (parsed == 0 || parsed > 1000000) {
            throw std::out_of_range("poll interval out of range");
        }
        return static_cast<useconds_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for --poll-us: " + value);
    }
}

static Config parse_args(int argc, char** argv) {
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto require_value = [&](const std::string& option) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + option);
            }
            return argv[++i];
        };

        if (arg == "--mouse-in") {
            cfg.mouse_in = require_value(arg);
        } else if (arg == "--key-in") {
            cfg.key_in = require_value(arg);
        } else if (arg == "--mouse-out") {
            cfg.mouse_out = require_value(arg);
        } else if (arg == "--key-out") {
            cfg.key_out = require_value(arg);
        } else if (arg == "--bind") {
            cfg.bind_addr = require_value(arg);
        } else if (arg == "--udp-port") {
            cfg.udp_port = parse_u16_arg(require_value(arg), arg);
        } else if (arg == "--allow-remote") {
            cfg.allow_remote = true;
        } else if (arg == "--poll-us") {
            cfg.poll_us = parse_poll_arg(require_value(arg));
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    if (cfg.bind_addr != "127.0.0.1" &&
        cfg.bind_addr != "localhost" &&
        cfg.bind_addr != "::1" &&
        !cfg.allow_remote) {
        throw std::runtime_error(
            "Refusing to bind UDP listener to a non-local address without "
            "--allow-remote. Use --bind 127.0.0.1 for local-only input, or "
            "--allow-remote --bind 0.0.0.0 only on a trusted network."
        );
    }

    return cfg;
}

static int open_device(const std::string& path, int flags, const std::string& label) {
    int fd = open(path.c_str(), flags);
    if (fd < 0) {
        std::cerr << "Failed to open " << label << " (" << path << "): "
                  << std::strerror(errno) << std::endl;
    }
    return fd;
}

static int32_t clamp_i32(int32_t value, int32_t lo, int32_t hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int16_t read_le_i16(const uint8_t* data) {
    uint16_t value = static_cast<uint16_t>(data[0]) |
                     static_cast<uint16_t>(data[1] << 8);
    return static_cast<int16_t>(value);
}

static bool write_exact(int fd, const void* data, size_t size, const char* label) {
    ssize_t written = write(fd, data, size);
    if (written != static_cast<ssize_t>(size)) {
        std::cerr << "Warning: failed to write complete " << label
                  << " report: " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

/**
 * Listens for UDP mouse packets and accumulates movement/button state for the
 * mouse bridge thread.
 *
 * Packet format is six bytes:
 *   bytes 0-1: dx, int16 little-endian
 *   bytes 2-3: dy, int16 little-endian
 *   byte  4:   wheel, int8
 *   byte  5:   buttons, uint8
 *
 * @param bind_addr Address to bind the UDP socket to.
 * @param port UDP port to listen on.
 */
void network_thread(const std::string& bind_addr, uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "UDP socket failed: " << std::strerror(errno) << std::endl;
        return;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Warning: SO_REUSEADDR failed: "
                  << std::strerror(errno) << std::endl;
    }

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Invalid IPv4 bind address: " << bind_addr << std::endl;
        close(sock);
        return;
    }

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "UDP bind failed on " << bind_addr << ":" << port
                  << ": " << std::strerror(errno) << std::endl;
        close(sock);
        return;
    }

    std::cout << "UDP listener ready on " << bind_addr << ":" << port << std::endl;

    uint8_t packet[6];

    while (true) {
        ssize_t n = recv(sock, packet, sizeof(packet), 0);
        if (n != static_cast<ssize_t>(sizeof(packet))) {
            continue;
        }

        int16_t dx = read_le_i16(&packet[0]);
        int16_t dy = read_le_i16(&packet[2]);
        int8_t wheel = static_cast<int8_t>(packet[4]);
        uint8_t buttons = packet[5];

        g_net_dx.fetch_add(dx, std::memory_order_relaxed);
        g_net_dy.fetch_add(dy, std::memory_order_relaxed);
        g_net_wheel.fetch_add(wheel, std::memory_order_relaxed);
        g_net_buttons.store(buttons, std::memory_order_relaxed);
    }
}

/**
 * Bridges physical mouse reports from a hidraw device to a USB HID gadget while
 * merging network-provided mouse input.
 *
 * Expected physical mouse input layout:
 *   byte  0:   physical button mask
 *   byte  1:   padding
 *   bytes 2-3: X delta, int16 little-endian
 *   bytes 4-5: Y delta, int16 little-endian
 *   byte  6:   wheel delta, int8
 *
 * Physical button state is preserved across non-blocking reads so network-only
 * movement does not accidentally release held physical buttons.
 *
 * @param move_fd Physical mouse hidraw input file descriptor.
 * @param gadget_fd Virtual mouse HID gadget output file descriptor.
 * @param poll_us Poll interval in microseconds.
 */
void mouse_thread(int move_fd, int gadget_fd, useconds_t poll_us) {
    uint8_t in[64];

    VirtualMouse out {};
    out.report_id = 0x01;

    uint8_t phys_buttons = 0;
    uint8_t prev_buttons = 0;

    while (true) {
        ssize_t bytes = read(move_fd, in, sizeof(in));

        int32_t net_dx = g_net_dx.exchange(0, std::memory_order_relaxed);
        int32_t net_dy = g_net_dy.exchange(0, std::memory_order_relaxed);
        int32_t net_wh = g_net_wheel.exchange(0, std::memory_order_relaxed);
        uint8_t net_bt = g_net_buttons.load(std::memory_order_relaxed);

        if (bytes >= 7) {
            phys_buttons = in[0];

            int32_t total_x = read_le_i16(&in[2]) + net_dx;
            int32_t total_y = read_le_i16(&in[4]) + net_dy;
            int32_t total_wh = static_cast<int8_t>(in[6]) + net_wh;

            out.x = static_cast<int16_t>(clamp_i32(total_x, -32768, 32767));
            out.y = static_cast<int16_t>(clamp_i32(total_y, -32768, 32767));
            out.wheel = static_cast<int8_t>(clamp_i32(total_wh, -127, 127));
            out.buttons = phys_buttons | net_bt;

            bool buttons_changed = (out.buttons != prev_buttons);
            if (out.x != 0 || out.y != 0 || out.wheel != 0 || buttons_changed) {
                write_exact(gadget_fd, &out, sizeof(out), "mouse");
                prev_buttons = out.buttons;
            }
        } else if (net_dx || net_dy || net_wh) {
            out.x = static_cast<int16_t>(clamp_i32(net_dx, -32768, 32767));
            out.y = static_cast<int16_t>(clamp_i32(net_dy, -32768, 32767));
            out.wheel = static_cast<int8_t>(clamp_i32(net_wh, -127, 127));
            out.buttons = phys_buttons | net_bt;

            write_exact(gadget_fd, &out, sizeof(out), "mouse");
            prev_buttons = out.buttons;
        }

        usleep(poll_us);
    }
}

/**
 * Bridges button reports from a hidraw keyboard-style input device to a USB HID
 * keyboard gadget.
 *
 * Expected physical input layout:
 *   byte 6: button mask
 *
 * Button mappings:
 *   0x04 -> HID keycode 0x26, '9'
 *   0x08 -> HID keycode 0x27, '0'
 *
 * A new keyboard report is written only when the input mask changes.
 *
 * @param key_fd Physical keyboard/button hidraw input file descriptor.
 * @param gadget_fd Virtual keyboard HID gadget output file descriptor.
 * @param poll_us Poll interval in microseconds.
 */
void keyboard_thread(int key_fd, int gadget_fd, useconds_t poll_us) {
    uint8_t in[64];
    VirtualKeyboard out {};

    uint8_t prev_mask = 0;

    while (true) {
        ssize_t bytes = read(key_fd, in, sizeof(in));

        if (bytes >= 7) {
            uint8_t mask = in[6];

            if (mask != prev_mask) {
                std::memset(&out, 0, sizeof(out));

                int k = 0;
                if (mask & 0x04) out.keys[k++] = 0x26;
                if (mask & 0x08) out.keys[k++] = 0x27;

                write_exact(gadget_fd, &out, sizeof(out), "keyboard");
                prev_mask = mask;
            }
        }

        usleep(poll_us);
    }
}

/**
 * Opens the configured physical HID inputs and virtual HID gadget outputs, then
 * starts the UDP listener, mouse bridge, and keyboard bridge worker threads.
 *
 * @return 0 if all worker threads exit normally, or 1 if setup fails.
 */
int main(int argc, char** argv) {
    Config cfg;

    try {
        cfg = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << std::endl;
        std::cerr << "Run with --help for usage." << std::endl;
        return 1;
    }

    int mouse_in = open_device(cfg.mouse_in, O_RDONLY | O_NONBLOCK, "mouse input");
    int key_in = open_device(cfg.key_in, O_RDONLY | O_NONBLOCK, "keyboard input");
    int mouse_out = open_device(cfg.mouse_out, O_WRONLY, "mouse gadget output");
    int key_out = open_device(cfg.key_out, O_WRONLY, "keyboard gadget output");

    if (mouse_in < 0 || key_in < 0 || mouse_out < 0 || key_out < 0) {
        if (mouse_in >= 0) close(mouse_in);
        if (key_in >= 0) close(key_in);
        if (mouse_out >= 0) close(mouse_out);
        if (key_out >= 0) close(key_out);
        return 1;
    }

    std::cout << "HIDMux active:" << std::endl;
    std::cout << "  Mouse input:   " << cfg.mouse_in << std::endl;
    std::cout << "  Keyboard input:" << cfg.key_in << std::endl;
    std::cout << "  Mouse output:  " << cfg.mouse_out << std::endl;
    std::cout << "  Keyboard output:" << cfg.key_out << std::endl;
    std::cout << "  UDP input:     " << cfg.bind_addr << ":" << cfg.udp_port << std::endl;
    std::cout << "  Poll interval: " << cfg.poll_us << " us" << std::endl;

    std::thread t_net(network_thread, cfg.bind_addr, cfg.udp_port);
    std::thread t_mouse(mouse_thread, mouse_in, mouse_out, cfg.poll_us);
    std::thread t_keys(keyboard_thread, key_in, key_out, cfg.poll_us);

    t_net.join();
    t_mouse.join();
    t_keys.join();

    close(mouse_in);
    close(key_in);
    close(mouse_out);
    close(key_out);

    return 0;
}
