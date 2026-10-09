#include "check.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace check {

namespace {

// How long one case may take before it is reported as having hung.
constexpr unsigned case_seconds = 30;

// Shared with the parent, so the reason a child failed survives the child.
constexpr std::size_t failure_size = 1024;
char *failure = nullptr;

std::string directory;

void record(const std::string &why) {
    std::string flat = why;
    for (char &c : flat) {
        if (c == '\n') {
            c = ' ';
        }
    }
    std::snprintf(failure, failure_size, "%s", flat.c_str());
}

} // namespace

const std::string &dir() { return directory; }

void sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

long long ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string read(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int run(const std::vector<Case> &cases) {
    void *shared = mmap(nullptr, failure_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        std::perror("check: mmap");
        return 1;
    }
    failure = static_cast<char *>(shared);
    int status = 0;
    for (const Case &c : cases) {
        failure[0] = '\0';
        // A short path: a Unix socket's name is bounded, and the cases bind several under it.
        char made[] = "/tmp/yoke-cpp-XXXXXX";
        if (mkdtemp(made) == nullptr) {
            std::perror("check: mkdtemp");
            return 1;
        }
        directory = made;
        std::cout.flush();
        pid_t child = fork();
        if (child == 0) {
            alarm(case_seconds);
            try {
                c.run();
            } catch (const Failed &f) {
                record(f.what());
            } catch (const std::exception &e) {
                record(say("it threw: ", e.what()));
            }
            std::cout.flush();
            _exit(failure[0] == '\0' ? 0 : 1);
        }
        int waited = 0;
        waitpid(child, &waited, 0);
        if (WIFSIGNALED(waited)) {
            const char *why =
                WTERMSIG(waited) == SIGALRM ? "it did not finish in time" : strsignal(WTERMSIG(waited));
            std::cout << "FAIL  " << c.id << " — " << why << (failure[0] ? "; " : "") << failure << "\n";
            status = 1;
        } else if (WEXITSTATUS(waited) != 0 || failure[0] != '\0') {
            std::cout << "FAIL  " << c.id << " — " << (failure[0] ? failure : "it exited non-zero") << "\n";
            status = 1;
        } else {
            std::cout << "pass  " << c.id << "\n";
        }
        std::cout.flush();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    return status;
}

} // namespace check
