#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <csignal>
#endif

using Bits = std::vector<uint64_t>;
using Clock = std::chrono::steady_clock;

#ifndef __EMSCRIPTEN__
volatile std::sig_atomic_t interruptRequested = 0;

void requestInterrupt(int) {
    interruptRequested = 1;
}
#endif

std::array<int, 5> feedback(const std::string& guess, const std::string& answer) {
    std::array<int, 26> count{};
    std::array<int, 5> tiles{};
    for (int p = 0; p < 5; ++p) {
        if (guess[p] == answer[p]) tiles[p] = 2;
        else ++count[answer[p] - 'a'];
    }
    for (int p = 0; p < 5; ++p) {
        if (tiles[p] != 2 && count[guess[p] - 'a']) {
            tiles[p] = 1;
            --count[guess[p] - 'a'];
        }
    }
    return tiles;
}

std::string normalize(std::string word) {
    for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (word.size() != 5 || word.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos)
        throw std::runtime_error("Expected a five-letter ASCII word: " + word);
    return word;
}

struct Move { int id; int count; uint32_t used; int bound; };

class Solver {
public:
    std::vector<std::string> words;
    std::vector<Bits> keep;
    std::vector<uint32_t> letters;
    std::vector<int> bestPath;
    uint64_t nodes = 0;
    int best = -1;
    bool interrupted = false;
    Clock::time_point started = Clock::now();
    double seconds;
    int target;
    size_t blocks;
    std::unordered_set<std::string> visited;
    Clock::time_point exploreUntil = started;
#ifdef __EMSCRIPTEN__
    bool reportProgress = false;
    int rootUpperBound = 0;
    Clock::time_point nextProgress = started;
#endif

    Solver(std::vector<std::string> dictionary, const std::string& answer, double limit)
        : words(std::move(dictionary)), seconds(limit) {
        const auto it = std::lower_bound(words.begin(), words.end(), answer);
        if (it == words.end() || *it != answer) throw std::runtime_error("Answer is not in the dictionary: " + answer);
        target = static_cast<int>(it - words.begin());
        blocks = (words.size() + 63) / 64;
        letters.resize(words.size());
        std::array<std::array<Bits, 26>, 5> position;
        std::array<std::array<Bits, 6>, 26> frequency;
        for (auto& row : position) for (auto& mask : row) mask.resize(blocks);
        for (auto& row : frequency) for (auto& mask : row) mask.resize(blocks);
        for (size_t i = 0; i < words.size(); ++i) {
            std::array<int, 26> counts{};
            for (int p = 0; p < 5; ++p) {
                int ch = words[i][p] - 'a';
                ++counts[ch];
                letters[i] |= 1u << ch;
                position[p][ch][i / 64] |= uint64_t{1} << (i % 64);
            }
            for (int ch = 0; ch < 26; ++ch)
                frequency[ch][counts[ch]][i / 64] |= uint64_t{1} << (i % 64);
        }
        keep.assign(words.size(), all());
        // Legal successors equal the exact feedback class, including duplicate-count bounds.
        for (size_t i = 0; i < words.size(); ++i) {
            const auto tiles = feedback(words[i], answer);
            std::array<int, 26> positive{}, total{};
            for (int p = 0; p < 5; ++p) {
                const int ch = words[i][p] - 'a';
                ++total[ch];
                positive[ch] += tiles[p] != 0;
                for (size_t b = 0; b < blocks; ++b)
                    keep[i][b] &= tiles[p] == 2 ? position[p][ch][b] : ~position[p][ch][b];
            }
            for (int ch = 0; ch < 26; ++ch) {
                if (!total[ch]) continue;
                int max = total[ch] > positive[ch] ? positive[ch] : 5;
                for (size_t b = 0; b < blocks; ++b) {
                    uint64_t allowed = 0;
                    for (int count = positive[ch]; count <= max; ++count) allowed |= frequency[ch][count][b];
                    keep[i][b] &= allowed;
                }
            }
        }
    }

    Bits all() const {
        Bits bits(blocks, ~uint64_t{0});
        if (words.size() % 64) bits.back() = (uint64_t{1} << (words.size() % 64)) - 1;
        return bits;
    }

    int count(const Bits& bits) const {
        int result = 0;
        for (auto block : bits) result += std::popcount(block);
        return result;
    }

    Bits after(const Bits& bits, int id) const {
        Bits next(blocks);
        for (size_t b = 0; b < blocks; ++b) next[b] = bits[b] & keep[id][b];
        return next;
    }

    std::vector<Move> moves(const Bits& bits, uint32_t used, int depth) {
        std::vector<Move> result;
        std::vector<size_t> active;
        for (size_t b = 0; b < blocks; ++b) if (bits[b]) active.push_back(b);
        for (size_t b = 0; b < blocks; ++b) {
            uint64_t pending = bits[b];
            while (pending) {
                const int id = static_cast<int>(b * 64 + std::countr_zero(pending));
                pending &= pending - 1;
                if (id == target) continue;
                int survivors = 0;
                for (size_t k : active) survivors += std::popcount(bits[k] & keep[id][k]);
                if (survivors < depth) continue;
                const uint32_t nextUsed = used | letters[id];
                // Every further non-answer guess eliminates itself; letters never become unused.
                const int bound = (survivors - depth + 1) * (26 - std::popcount(nextUsed));
                if (bound > best) result.push_back({id, survivors, nextUsed, bound});
            }
        }
        std::sort(result.begin(), result.end(), [](const Move& a, const Move& b) {
            if (a.bound != b.bound) return a.bound > b.bound;
            return a.id < b.id;
        });
        return result;
    }

    bool expired() {
#ifdef __EMSCRIPTEN__
        emitProgress();
#else
        if (interruptRequested) interrupted = true;
#endif
        if (seconds > 0 && std::chrono::duration<double>(Clock::now() - started).count() >= seconds)
            interrupted = true;
        return interrupted;
    }

    void record(int score, const std::vector<int>& path) {
        if (score <= best) return;
        best = score;
        bestPath = path;
#ifdef __EMSCRIPTEN__
        emitProgress(true);
#else
        std::cerr << "Best complete score: " << best << "  [";
        for (int id : path) std::cerr << words[id] << ' ';
        std::cerr << "]\n";
#endif
    }

    // Limited discrepancy search: pass k follows the top-ranked move except at exactly k
    // nodes, spending those departures at the earliest guesses first. It only raises `best`
    // and never touches `visited`, so the exhaustive search that follows remains a proof.
    void explore(const Bits& bits, uint32_t used, int depth, std::vector<int>& path, double limit) {
        exploreUntil = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(limit));
        for (int k = 0; k <= depth && !exploreExpired(); ++k) probe(bits, used, depth, k, path);
    }

    bool exploreExpired() {
        return expired() || Clock::now() >= exploreUntil;
    }

    void probe(const Bits& bits, uint32_t used, int depth, int k, std::vector<int>& path) {
        ++nodes;
        if (exploreExpired()) return;
        if (!depth) {
            record(count(bits) * (26 - std::popcount(used)), path);
            return;
        }
        const auto choices = moves(bits, used, depth);
        auto step = [&](const Move& move, int left) {
            path.push_back(move.id);
            probe(after(bits, move.id), move.used, depth - 1, left, path);
            path.pop_back();
        };
        if (k) for (size_t i = 1; i < choices.size() && choices[i].bound > best; ++i) {
            step(choices[i], k - 1);
            if (exploreExpired()) return;
        }
        if (!choices.empty() && k < depth && choices[0].bound > best) step(choices[0], k);
    }

    void search(const Bits& bits, uint32_t used, int depth, std::vector<int>& path) {
        ++nodes;
        if (expired()) return;
        const int n = count(bits);
        if (!depth) {
            record(n * (26 - std::popcount(used)), path);
            return;
        }
        if (n <= depth || (n - depth) * (26 - std::popcount(used)) <= best) return;
        // Exact transpositions: no probabilistic hash-only pruning. Cap memory, not search.
        std::string key(reinterpret_cast<const char*>(bits.data()), blocks * sizeof(uint64_t));
        key.append(reinterpret_cast<const char*>(&used), sizeof used);
        key.push_back(static_cast<char>(depth));
        if (visited.contains(key)) return;
        if (visited.size() < 100000) visited.insert(std::move(key));
        const auto choices = moves(bits, used, depth);
        for (const auto& move : choices) {
            if (move.bound <= best) break;
            path.push_back(move.id);
            const auto next = after(bits, move.id);
            search(next, move.used, depth - 1, path);
            path.pop_back();
            if (interrupted) return;
        }
    }

    void printResult(const std::string& status, int upper, bool json) const {
        const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        const auto& answer = words[target];
        if (json) {
            std::cout << "{\"status\":\"" << status << "\",\"answer\":\"" << answer << "\",\"score\":" << best
                << ",\"upper_bound\":" << upper
                << ",\"nodes\":" << nodes << ",\"seconds\":" << elapsed << ",\"steps\":[";
        } else {
            std::cout << status << " | answer " << answer << " | final score " << best << '\n';
            if (interrupted) {
#ifdef __EMSCRIPTEN__
                std::cout << "Search timed out";
#else
                std::cout << (interruptRequested ? "Search interrupted" : "Search timed out");
#endif
                std::cout << "; global upper bound " << upper << ". Optimality NOT proven.\n";
            }
            std::cout << " #  GUESS  CLUES  WORDS  UNUSED  SCORE\n";
        }
        auto state = all();
        uint32_t used = 0;
        for (size_t i = 0; i < bestPath.size(); ++i) {
            const int id = bestPath[i];
            state = after(state, id); used |= letters[id];
            const int n = count(state), unused = 26 - std::popcount(used);
            std::string clues;
            for (int tile : feedback(words[id], answer)) clues += ".YG"[tile];
            if (json) {
                if (i) std::cout << ',';
                std::cout << "{\"guess\":\"" << words[id] << "\",\"feedback\":\"" << clues
                    << "\",\"remaining_words\":" << n << ",\"unused_letters\":" << unused << ",\"score\":" << n * unused << '}';
            } else std::cout << ' ' << i + 1 << "  " << words[id] << "  " << clues << "  " << n << "  " << unused << "  " << n * unused << '\n';
        }
        if (json) std::cout << "]}";
        else std::cout << nodes << " search nodes, " << elapsed << " seconds.";
    }

#ifdef __EMSCRIPTEN__
    void emitProgress(bool force = false) {
        if (!reportProgress) return;
        const auto now = Clock::now();
        if (!force && now < nextProgress) return;
        nextProgress = now + std::chrono::milliseconds(250);
        std::cout << "{\"type\":\"progress\",\"result\":";
        printResult("SEARCHING", rootUpperBound, true);
        std::cout << "}\n" << std::flush;
    }
#endif
};

int main(int argc, char** argv) {
#ifndef __EMSCRIPTEN__
    std::signal(SIGINT, requestInterrupt);
#endif
    try {
        std::string answer;
#ifdef __EMSCRIPTEN__
        std::string dictionary = "/data/words.txt";
#else
        std::string dictionary = (std::filesystem::canonical("/proc/self/exe").parent_path() / "data/words.txt").string();
#endif
        std::vector<std::string> prefix;
        int turns = 6;
        double seconds = 0;
        bool json = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string { if (++i >= argc) throw std::runtime_error("Missing value for " + arg); return argv[i]; };
            if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: dont-wordle ANSWER [--history WORD,WORD] [--turns 6]\n"
                    "                    [--dictionary FILE] [--time-limit SECONDS] [--json]\n\n"
                    "Searches full strict-legal sequences, excluding the known answer.\n"
                    "Maximizes FINAL remaining valid words * unused distinct letters.\n"
                    "Default: exhaustive branch-and-bound, no time limit. OPTIMAL means proven.\n"
                    "A time limit returns BEST KNOWN, never claims optimality (exit status 2).\n"
#ifndef __EMSCRIPTEN__
                    "Ctrl+C stops safely and returns BEST KNOWN (exit status 2).\n"
#endif
                    "--turns is the total number of guesses, including --history (1..6).\n"
                    "Before the exhaustive search, a limited discrepancy pass varies early guesses\n"
                    "first to find strong sequences sooner: 20% of --time-limit, or 10s without one.\n"
                    "Dictionary: one five-letter word per line; defaults to the bundled site list.\n"
                    "Feedback: G = green, Y = yellow, . = gray. No undos are modeled.\n"
                    "Dictionary source: https://dontwordle.com/static/js/main.468abff3.js\n";
                return 0;
            } else if (arg == "--dictionary") dictionary = value();
            else if (arg == "--history") {
                std::istringstream input(value()); std::string word;
                while (std::getline(input, word, ',')) prefix.push_back(normalize(word));
            } else if (arg == "--turns") {
                const auto text = value(); size_t end = 0; turns = std::stoi(text, &end);
                if (end != text.size()) throw std::runtime_error("Invalid --turns");
            } else if (arg == "--time-limit") {
                const auto text = value(); size_t end = 0; seconds = std::stod(text, &end);
                if (end != text.size() || !(seconds > 0) || seconds > 1e9) throw std::runtime_error("Time limit must be finite and positive");
            } else if (arg == "--json") json = true;
            else if (arg.starts_with('-')) throw std::runtime_error("Unknown option: " + arg);
            else if (answer.empty()) answer = normalize(arg);
            else throw std::runtime_error("Unexpected argument: " + arg);
        }
        if (answer.empty()) throw std::runtime_error("Supply the known answer; use --help for usage");
        if (turns < 1 || turns > 6 || prefix.size() > static_cast<size_t>(turns)) throw std::runtime_error("Turns must be 1..6 and at least history length");
        std::ifstream file(dictionary);
        if (!file) throw std::runtime_error("Cannot open dictionary: " + dictionary);
        std::vector<std::string> words; std::string word;
        while (file >> word) words.push_back(normalize(word));
        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());
        Solver solver(std::move(words), answer, seconds);
        auto state = solver.all();
        uint32_t used = 0;
        std::vector<int> path;
        for (const auto& guess : prefix) {
            const auto it = std::lower_bound(solver.words.begin(), solver.words.end(), guess);
            if (it == solver.words.end() || *it != guess) throw std::runtime_error("History word not in dictionary: " + guess);
            int id = static_cast<int>(it - solver.words.begin());
            if (id == solver.target) throw std::runtime_error("History guesses the answer");
            if (!(state[id / 64] & (uint64_t{1} << (id % 64)))) throw std::runtime_error("History word was already ruled out: " + guess);
            state = solver.after(state, id); used |= solver.letters[id]; path.push_back(id);
            if (solver.count(state) <= turns - static_cast<int>(path.size())) throw std::runtime_error("History reaches an eliminated state");
        }
        const int upper = (solver.count(state) - (turns - static_cast<int>(path.size()))) * (26 - std::popcount(used));
#ifdef __EMSCRIPTEN__
        solver.reportProgress = json;
        solver.rootUpperBound = std::max(0, upper);
        solver.emitProgress(true);
#endif
        const int depth = turns - static_cast<int>(path.size());
        solver.explore(state, used, depth, path, seconds > 0 ? seconds * 0.2 : 10);
        solver.search(state, used, depth, path);
        const std::string status = solver.interrupted ? "BEST KNOWN" : solver.best < 0 ? "NO SOLUTION" : "OPTIMAL";
        solver.printResult(status, solver.interrupted ? std::max(0, upper) : solver.best, json);
        std::cout << '\n';
        return solver.interrupted ? 2 : solver.best < 0 ? 3 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
