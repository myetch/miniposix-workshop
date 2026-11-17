#include "miniposix/pipe.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

    constexpr std::size_t kMib = 1024 * 1024;
    constexpr std::size_t kPatternPeriod = 251;

    // baseline: computes and checks offset % 251 one byte at a time.
    // table:    copies/compares slices of a precomputed repeating pattern with
    //           memcpy/memcmp. Same bytes on the wire, same syscalls.
    enum class Variant { baseline, table };

    std::string_view variant_name(Variant variant) {
        return variant == Variant::baseline ? "baseline" : "table";
    }

    struct Options {
        std::filesystem::path output = "bench-results/pipe.csv";
        std::size_t total_bytes = 8 * kMib;
        std::size_t repeats = 3;
        std::vector<std::size_t> buffer_sizes = {512, 4096, 65536};
        std::vector<Variant> variants = {Variant::baseline};
    };

    struct Result {
        Variant variant;
        std::size_t buffer_bytes;
        std::size_t total_bytes;
        std::size_t repeat;
        double seconds;
        double throughput_mib_s;
    };

    std::size_t positive_integer(std::string_view text, std::string_view option) {
        std::size_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
            throw std::invalid_argument(std::string(option) + " requires a positive integer");
        }
        return value;
    }

    std::vector<std::size_t> parse_buffer_sizes(std::string_view text) {
        std::vector<std::size_t> values;
        while (!text.empty()) {
            const auto separator = text.find(',');
            const auto piece = text.substr(0, separator);
            values.push_back(positive_integer(piece, "--buffer-sizes"));
            if (separator == std::string_view::npos) {
                break;
            }
            text.remove_prefix(separator + 1);
            if (text.empty()) {
                throw std::invalid_argument("--buffer-sizes cannot end with a comma");
            }
        }
        if (values.empty()) {
            throw std::invalid_argument("--buffer-sizes requires a comma-separated list");
        }
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }

    std::vector<Variant> parse_variants(std::string_view text) {
        std::vector<Variant> values;
        while (true) {
            const auto separator = text.find(',');
            const auto piece = text.substr(0, separator);
            if (piece == "baseline") {
                values.push_back(Variant::baseline);
            } else if (piece == "table") {
                values.push_back(Variant::table);
            } else {
                throw std::invalid_argument("--variants accepts baseline and table");
            }
            if (separator == std::string_view::npos) {
                break;
            }
            text.remove_prefix(separator + 1);
        }
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }

    void usage(const char *program) {
        std::cout << "Usage: " << program
                  << " [--output FILE] [--bytes N] [--repeats N] [--buffer-sizes N,N,...] [--variants baseline,table]\n"
                  << "Defaults: bench-results/pipe.csv, 8388608 bytes, 3 repeats, 512,4096,65536 byte buffers, baseline variant\n";
    }

    Options parse_options(int argc, char **argv) {
        Options options;
        for (int index = 1; index < argc; ++index) {
            const std::string_view option(argv[index]);
            if (option == "--help") {
                usage(argv[0]);
                std::exit(0);
            }
            if (index + 1 == argc) {
                throw std::invalid_argument(std::string(option) + " requires a value");
            }
            const std::string_view value(argv[++index]);
            if (option == "--output") {
                if (value.empty()) {
                    throw std::invalid_argument("--output requires a file path");
                }
                options.output = value;
            } else if (option == "--bytes") {
                options.total_bytes = positive_integer(value, option);
            } else if (option == "--repeats") {
                options.repeats = positive_integer(value, option);
            } else if (option == "--buffer-sizes") {
                options.buffer_sizes = parse_buffer_sizes(value);
            } else if (option == "--variants") {
                options.variants = parse_variants(value);
            } else {
                throw std::invalid_argument("unknown option: " + std::string(option));
            }
        }
        return options;
    }

    unsigned char byte_at(std::size_t offset) {
        return static_cast<unsigned char>(offset % 251);
    }

    // Pattern long enough that any slice starting at offset % 251 covers a
    // full buffer: pattern[i] == byte_at(i) for every i.
    std::vector<unsigned char> make_pattern(std::size_t buffer_bytes) {
        std::vector<unsigned char> pattern(buffer_bytes + kPatternPeriod);
        for (std::size_t index = 0; index < pattern.size(); ++index) {
            pattern[index] = byte_at(index);
        }
        return pattern;
    }

    void write_exactly(Variant variant, int descriptor, std::size_t total_bytes, std::size_t buffer_bytes) {
        std::vector<unsigned char> buffer(buffer_bytes);
        const auto pattern = variant == Variant::table ? make_pattern(buffer_bytes) : std::vector<unsigned char>{};
        std::size_t sent = 0;
        while (sent < total_bytes) {
            const auto count = std::min(buffer_bytes, total_bytes - sent);
            if (variant == Variant::table) {
                std::memcpy(buffer.data(), pattern.data() + sent % kPatternPeriod, count);
            } else {
                for (std::size_t offset = 0; offset < count; ++offset) {
                    buffer[offset] = byte_at(sent + offset);
                }
            }
            std::size_t cursor = 0;
            while (cursor < count) {
                const auto written = ::write(descriptor, buffer.data() + cursor, count - cursor);
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                if (written < 0) {
                    throw std::system_error(errno, std::generic_category(), "pipe write");
                }
                if (written == 0) {
                    throw std::runtime_error("pipe write made no progress");
                }
                cursor += static_cast<std::size_t>(written);
            }
            sent += count;
        }
    }

    bool read_and_verify(Variant variant, int descriptor, std::size_t total_bytes, std::size_t buffer_bytes) {
        std::vector<unsigned char> buffer(buffer_bytes);
        const auto pattern = variant == Variant::table ? make_pattern(buffer_bytes) : std::vector<unsigned char>{};
        std::size_t received = 0;
        bool correct = true;
        while (received < total_bytes) {
            const auto count = std::min(buffer_bytes, total_bytes - received);
            const auto read_count = ::read(descriptor, buffer.data(), count);
            if (read_count < 0 && errno == EINTR) {
                continue;
            }
            if (read_count < 0) {
                throw std::system_error(errno, std::generic_category(), "pipe read");
            }
            if (read_count == 0) {
                throw std::runtime_error("pipe reached EOF before the expected byte count");
            }
            const auto got = static_cast<std::size_t>(read_count);
            if (variant == Variant::table) {
                if (std::memcmp(buffer.data(), pattern.data() + received % kPatternPeriod, got) != 0) {
                    correct = false;
                }
            } else {
                for (std::size_t offset = 0; offset < got; ++offset) {
                    if (buffer[offset] != byte_at(received + offset)) {
                        correct = false;
                    }
                }
            }
            received += static_cast<std::size_t>(read_count);
        }
        return correct;
    }

    Result measure(Variant variant, std::size_t total_bytes, std::size_t buffer_bytes, std::size_t repeat) {
        auto pipe = miniposix::Pipe::create();
        std::exception_ptr writer_failure;
        std::exception_ptr reader_failure;
        bool correct = false;

        const auto start = std::chrono::steady_clock::now();
        std::jthread writer([&] {
            try {
                write_exactly(variant, pipe.write_end().get(), total_bytes, buffer_bytes);
            } catch (...) {
                writer_failure = std::current_exception();
            }
            pipe.write_end().reset();
        });
        try {
            correct = read_and_verify(variant, pipe.read_end().get(), total_bytes, buffer_bytes);
        } catch (...) {
            reader_failure = std::current_exception();
            // Release the reader so a failed run cannot leave the writer blocked.
            pipe.read_end().reset();
        }
        writer.join();
        const auto end = std::chrono::steady_clock::now();

        if (writer_failure) {
            std::rethrow_exception(writer_failure);
        }
        if (reader_failure) {
            std::rethrow_exception(reader_failure);
        }
        if (!correct) {
            throw std::runtime_error("pipe transferred corrupted data");
        }

        const double seconds = std::chrono::duration<double>(end - start).count();
        if (seconds <= 0) {
            throw std::runtime_error("clock did not advance during measurement");
        }
        return {variant, buffer_bytes, total_bytes, repeat, seconds,
                (static_cast<double>(total_bytes) / static_cast<double>(kMib)) / seconds};
    }

    void save_results(const std::filesystem::path &output, const std::vector<Result> &results) {
        if (!output.parent_path().empty()) {
            std::filesystem::create_directories(output.parent_path());
        }
        const bool needs_header = !std::filesystem::exists(output) || std::filesystem::file_size(output) == 0;
        std::ofstream file(output, std::ios::app);
        if (!file) {
            throw std::runtime_error("cannot open CSV output: " + output.string());
        }
        if (needs_header) {
            file << "variant,buffer_bytes,total_bytes,seconds,throughput_mib_s,repeat\n";
        }
        file << std::setprecision(12);
        for (const auto &result: results) {
            // Baseline keeps the original name so older CSVs stay comparable.
            file << (result.variant == Variant::baseline ? "miniposix_pipe" : "miniposix_pipe_table") << ','
                 << result.buffer_bytes << ',' << result.total_bytes << ',' << result.seconds << ','
                 << result.throughput_mib_s << ',' << result.repeat << '\n';
        }
        if (!file) {
            throw std::runtime_error("failed to write CSV output: " + output.string());
        }
    }

} // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parse_options(argc, argv);
        // A failed reader closes its end; EPIPE then lets the writer exit and join.
        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            throw std::system_error(errno, std::generic_category(), "ignore SIGPIPE");
        }
        std::vector<Result> results;
        for (const auto buffer_bytes: options.buffer_sizes) {
            for (std::size_t repeat = 1; repeat <= options.repeats; ++repeat) {
                // Variants alternate within each repeat so slow drift in system
                // load affects them equally.
                for (const auto variant: options.variants) {
                    const auto result = measure(variant, options.total_bytes, buffer_bytes, repeat);
                    results.push_back(result);
                    std::cout << variant_name(variant) << ", " << buffer_bytes << " bytes, repeat " << repeat
                              << ": " << std::fixed << std::setprecision(2) << result.throughput_mib_s << " MiB/s\n";
                }
            }
        }
        save_results(options.output, results);
        std::cout << "Saved " << results.size() << " measurements to " << options.output << '\n';
    } catch (const std::exception &error) {
        std::cerr << "bench_pipe: " << error.what() << '\n';
        return 1;
    }
}
