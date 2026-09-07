// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Benchmarks for the work every den command repeats, plus the gate that
// locks their numbers in both directions.
//
//     make bench             run them and print the results
//     make bench-lock        make this run the new baseline
//     make bench-gate        compare against docs/perf/baseline.txt
//     make bench-gate-loose  compare only the counted metrics
//
// Nine of den's CLI callbacks open with `load_index(cfg.cache /
// "index.json")`, so on this machine every `den install`, `den search`,
// `den info` and `den list` parses a 21 MB JSON document before it does
// anything a user asked for. That is the thing to measure.
//
// Two kinds of number come out of a run. Timings are the FLOOR over
// many rounds, never an average: load on the machine can only ever make
// a round slower, so the fastest round is the closest thing to an
// uncontended measurement. They are still only comparable against a
// baseline recorded on the same machine, so they are skipped under
// --loose. Counted metrics — packages loaded, bytes on disk — are
// functions of the input and the code and are compared exactly.
//
// The corpus is generated, never the user's own ~/.den/cache. A
// benchmark that reads a developer's real index is not reproducible and
// not safe to commit numbers from.

#include "index/index.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;

namespace {

// How long each benchmark is sampled for before its per-iteration cost
// is reported.
constexpr double kSampleSeconds = 0.5;

// Every benchmark runs at least this many rounds, however slow it is,
// so its floor is taken over more than a couple of samples.
constexpr int kMinRounds = 5;

// Timing tolerance, as a fraction of the baseline. Deliberately wide: a
// gate that fails on scheduler noise gets switched off, which is worse
// than a coarse one.
constexpr double kTimingTolerance = 0.25;

// The corpus size. Homebrew's index is around seven thousand formulae
// and the cache built from it is tens of megabytes; this generates the
// same order without depending on anything on the machine.
constexpr int kPackageCount = 7000;

// Where the locked baseline lives, relative to the source tree.
constexpr const char* kBaseline = "docs/perf/baseline.txt";

struct Metric {
    std::string name;
    double value;
};

struct Sample {
    std::string name;
    double ns = 0;
    std::vector<Metric> counted;

    Sample& with(const std::string& metric, double value) {
        counted.push_back({metric, value});
        return *this;
    }
};

double NowSeconds() {
    return std::chrono::duration<double>(clock_type::now().time_since_epoch()).count();
}

// Run `body` repeatedly for kSampleSeconds and report the floor of the
// per-iteration cost. Each of these benchmarks takes milliseconds, so
// there is no batching: a single iteration already dwarfs the clock's
// granularity.
Sample Bench(const std::string& name, const std::function<void()>& body) {
    body(); // One untimed pass; a first call pays for cold pages.

    double start = NowSeconds();
    double floor = -1;
    int rounds = 0;
    for (;;) {
        double round_start = NowSeconds();
        body();
        double elapsed = NowSeconds() - round_start;
        if (floor < 0 || elapsed < floor) {
            floor = elapsed;
        }
        rounds++;
        if (rounds >= kMinRounds && NowSeconds() - start >= kSampleSeconds) {
            break;
        }
    }
    Sample s;
    s.name = name;
    s.ns = floor * 1e9;
    return s;
}

// Write an index cache shaped like the one `den update` produces: a
// packages array of objects with the fields package_from_json reads.
fs::path WriteCorpus(const fs::path& dir) {
    json root;
    root["packages"] = json::array();
    for (int i = 0; i < kPackageCount; i++) {
        json p;
        p["name"] = "formula-" + std::to_string(i);
        p["version"] = "1." + std::to_string(i % 40) + ".0";
        p["description"] = "A package that does something useful, number " + std::to_string(i);
        p["homepage"] = "https://example.invalid/formula-" + std::to_string(i);
        p["license"] = "Apache-2.0";
        p["kind"] = "formula";
        json deps = json::array();
        for (int d = 0; d < 8; d++) {
            deps.push_back("formula-" + std::to_string((i + d * 37) % kPackageCount));
        }
        p["dependencies"] = deps;
        json bottles = json::object();
        for (const char* platform : {"arm64_sequoia", "arm64_sonoma", "x86_64_linux"}) {
            json b;
            b["url"] = std::string("https://example.invalid/bottles/") + platform + "/formula-" +
                       std::to_string(i) + ".tar.gz";
            b["sha256"] = "0000000000000000000000000000000000000000000000000000000000000000";
            bottles[platform] = b;
        }
        p["bottles"] = bottles;
        root["packages"].push_back(std::move(p));
    }

    fs::path path = dir / "index.json";
    std::ofstream out(path);
    out << root.dump();
    out.close();
    return path;
}

// Read a whole file in one go: size it, allocate once, read once.
//
// The obvious spelling — constructing a std::string from a pair of
// istreambuf_iterators — is not this. It goes through the stream one
// character at a time and reallocates as it grows, and it cost 15 ms on
// a 6 MB file where this costs under 2. The first version of this
// benchmark used it, and made parsing from a string look slower than
// parsing from a stream. Measure the measurement.
std::string ReadWhole(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::string text;
    text.resize(static_cast<size_t>(fs::file_size(path)));
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    return text;
}

void Render(const std::vector<Sample>& results, std::ostream& out) {
    out << "# den benchmarks — see docs/perf/baseline.md\n";
    for (const auto& s : results) {
        out << s.name << "\t" << static_cast<long long>(s.ns) << " ns/op";
        for (const auto& m : s.counted) {
            out << "\t" << static_cast<long long>(m.value) << " " << m.name;
        }
        out << "\n";
    }
}

std::vector<Sample> ParseBaseline(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "bench: cannot read " << path << "\n";
        std::exit(2);
    }
    std::vector<Sample> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        Sample s;
        std::string field;
        bool first = true;
        while (std::getline(fields, field, '\t')) {
            if (first) {
                s.name = field;
                first = false;
                continue;
            }
            std::istringstream fs_in(field);
            double value = 0;
            std::string unit;
            if (!(fs_in >> value >> unit)) {
                continue;
            }
            if (unit == "ns/op") {
                s.ns = value;
            } else {
                s.with(unit, value);
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

// Compare a fresh run against the baseline. Fails in both directions:
// an improvement means the baseline no longer describes the code, and
// letting it through silently is how a baseline stops guarding
// anything.
bool Compare(const std::vector<Sample>& base, const std::vector<Sample>& got, bool loose) {
    int failed = 0;
    std::cout << "\n";
    for (const auto& b : base) {
        const Sample* g = nullptr;
        for (const auto& candidate : got) {
            if (candidate.name == b.name) {
                g = &candidate;
            }
        }
        if (g == nullptr) {
            std::cout << b.name << ": MISSING from this run\n";
            failed++;
            continue;
        }
        for (const auto& m : b.counted) {
            double have = -1;
            for (const auto& n : g->counted) {
                if (n.name == m.name) {
                    have = n.value;
                }
            }
            if (have != m.value) {
                std::cout << b.name << " " << m.name << ": locked " << m.value << ", measured "
                          << have
                          << " — a rise is a regression, a fall must be "
                             "re-locked in the same commit\n";
                failed++;
            }
        }
        if (loose) {
            continue;
        }
        double delta = (g->ns - b.ns) / b.ns;
        if (delta > kTimingTolerance || delta < -kTimingTolerance) {
            std::cout << b.name << " ns/op: locked " << b.ns << ", measured " << g->ns << " ("
                      << delta * 100 << "%) — "
                      << (delta > 0 ? "a regression"
                                    : "an improvement; re-lock in the same "
                                      "commit (make bench-lock)")
                      << "\n";
            failed++;
        }
    }
    for (const auto& g : got) {
        bool known = false;
        for (const auto& b : base) {
            known = known || b.name == g.name;
        }
        if (!known) {
            std::cout << g.name << ": NEW — lock it before gating on it\n";
        }
    }
    std::cout << "\n"
              << (failed == 0 ? "bench gate: within tolerance" : "bench gate: FAILED") << "\n";
    return failed == 0;
}

} // namespace

int main(int argc, char** argv) {
    bool lock = false, gate = false, loose = false;
    for (int i = 1; i < argc; i++) {
        lock = lock || std::strcmp(argv[i], "--lock") == 0;
        gate = gate || std::strcmp(argv[i], "--gate") == 0;
        loose = loose || std::strcmp(argv[i], "--loose") == 0;
    }

    fs::path dir = fs::temp_directory_path() / "den-bench";
    fs::create_directories(dir);
    fs::path corpus = WriteCorpus(dir);
    const auto corpus_bytes = static_cast<double>(fs::file_size(corpus));

    // The shipped path: what nine CLI callbacks do before anything else.
    auto loaded = den::load_index(corpus);
    const auto package_count = static_cast<double>(loaded.packages.size());

    std::vector<Sample> results;
    results.push_back(Bench("LoadIndex",
                            [&] {
                                auto idx = den::load_index(corpus);
                                if (idx.packages.empty()) {
                                    std::cerr << "bench: empty index\n";
                                    std::exit(2);
                                }
                            })
                          .with("packages", package_count)
                          .with("index-bytes", corpus_bytes));

    // Attribution. load_index does two things to the file: nlohmann
    // builds a DOM from it, and a loop converts every element of that
    // DOM into a Package. Measuring them apart is what says which one to
    // work on — and the first is measured twice, once the way the
    // product does it and once from a string, because nlohmann reading
    // through a stream iterator and nlohmann reading contiguous memory
    // are not the same speed.
    results.push_back(Bench("Attr_ParseFromStream", [&] {
                          std::ifstream f(corpus);
                          json j = json::parse(f);
                          if (!j.contains("packages")) {
                              std::exit(2);
                          }
                      }).with("index-bytes", corpus_bytes));

    results.push_back(Bench("Attr_ParseFromString", [&] {
                          std::string text = ReadWhole(corpus);
                          json j = json::parse(text);
                          if (!j.contains("packages")) {
                              std::exit(2);
                          }
                      }).with("index-bytes", corpus_bytes));

    results.push_back(Bench("Attr_ReadFileOnly", [&] {
                          std::string text = ReadWhole(corpus);
                          if (text.size() < 2) {
                              std::exit(2);
                          }
                      }).with("index-bytes", corpus_bytes));

    Render(results, std::cout);

    if (lock) {
        fs::path out_path = fs::path(DEN_SOURCE_DIR) / kBaseline;
        fs::create_directories(out_path.parent_path());
        std::ofstream out(out_path);
        Render(results, out);
        std::cout << "\nlocked " << kBaseline << " — update docs/perf/baseline.md alongside it\n";
        return 0;
    }
    if (gate) {
        auto base = ParseBaseline(fs::path(DEN_SOURCE_DIR) / kBaseline);
        return Compare(base, results, loose) ? 0 : 1;
    }
    return 0;
}
