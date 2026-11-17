// Per-upload parallel prefault (atomic page cursor over a worker pool) vs plain reads, on 3 MiB uploads.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <latch>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
using u8 = uint8_t;
constexpr size_t Page = 4096;

static uint64_t Faults() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PageFaultCount;
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt + ru.ru_majflt;
#endif
}
static double Now() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Pool {
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> q;
    std::mutex m;
    std::condition_variable cv;
    size_t scheduled = 0, done = 0, n;
    bool stop = false;
    explicit Pool(size_t n_) : n{n_} {
        for (size_t i = 0; i < n; ++i) threads.emplace_back([this] {
            for (;;) {
                std::function<void()> f;
                {
                    std::unique_lock l{m};
                    cv.wait(l, [this] { return stop || !q.empty(); });
                    if (stop) return;
                    f = std::move(q.front());
                    q.pop();
                }
                f();
                std::scoped_lock l{m};
                ++done;
            }
        });
    }
    bool TryQueue(std::function<void()> f) {
        {
            std::scoped_lock l{m};
            if (scheduled - done >= n) return false;
            q.push(std::move(f));
            ++scheduled;
        }
        cv.notify_one();
        return true;
    }
    ~Pool() {
        { std::scoped_lock l{m}; stop = true; }
        cv.notify_all();
        for (auto& t : threads) t.join();
    }
};

static void Prefault(Pool* pool, const u8* base, size_t size) {
    const size_t pages = size / Page;
    constexpr size_t Batch = 64;
    constexpr size_t MaxHelpers = 7;
    std::atomic<size_t> cursor{0};
    const auto touch = [&] {
        for (;;) {
            const size_t begin = cursor.fetch_add(Batch, std::memory_order_relaxed);
            if (begin >= pages) return;
            const size_t end = (std::min)(begin + Batch, pages);
            for (size_t i = begin; i < end; ++i) static_cast<void>(*reinterpret_cast<const volatile u8*>(base + i * Page));
        }
    };
    std::latch helpers{static_cast<std::ptrdiff_t>(MaxHelpers)};
    size_t queued = 0;
    if (pool) {
        for (; queued < MaxHelpers && queued * Batch * 2 < pages; ++queued) {
            if (!pool->TryQueue([&] { touch(); helpers.count_down(); })) break;
        }
    }
    helpers.count_down(static_cast<std::ptrdiff_t>(MaxHelpers - queued));
    touch();
    helpers.wait();
}

static uint64_t Untile(const u8* src, u8* dst, size_t size) {
    // Stand-in for block-linear untiling: 64-byte rows gathered from 512-byte GOBs.
    for (size_t gob = 0; gob < size; gob += 512)
        for (size_t row = 0; row < 8; ++row) std::memcpy(dst + (gob / 512) * 64 * 8 + row * 64, src + gob + row * 64, 64);
    return dst[size / 2];
}

int main() {
    constexpr size_t Upload = 3 << 20;
    constexpr size_t Uploads = 96; // 288 MiB per run, a cutscene burst
    constexpr size_t Runs = 6;
    const size_t size = Upload * Uploads * Runs;
#ifdef _WIN32
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, DWORD(uint64_t(size) >> 32), DWORD(size), nullptr);
    u8* a = static_cast<u8*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size));
    u8* b = static_cast<u8*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size));
#else
    int fd = memfd_create("bench", 0);
    if (ftruncate(fd, size) != 0) return 1;
    u8* a = static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    u8* b = static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
#endif
    for (size_t i = 0; i < size; i += 64) std::memset(a + i, int(i >> 12) | 1, 64);
    std::vector<u8> staging(Upload);
    std::memset(staging.data(), 0, Upload);
    const unsigned hw = std::thread::hardware_concurrency();
    std::printf("hardware threads %u\n", hw);
    struct Arm { const char* name; size_t workers; };
    const Arm arms[] = {{"no prefault", 0}, {"prefault, back thread only", 0}, {"prefault + 1 worker", 1}, {"prefault + 2 workers", 2}, {"prefault + 3 workers", 3}, {"prefault + 4 workers", 4}};
    size_t run = 0;
    for (const Arm& arm : arms) {
        if (run >= Runs) break;
        Pool pool{arm.workers ? arm.workers : 1};
        const bool prefault = &arm != &arms[0];
        const u8* base = b + run * Upload * Uploads;
        const uint64_t f0 = Faults();
        double pre = 0;
        const double t0 = Now();
        uint64_t sum = 0;
        for (size_t u = 0; u < Uploads; ++u) {
            const u8* src = base + u * Upload;
            if (prefault) {
                const double p0 = Now();
                Prefault(arm.workers ? &pool : nullptr, src, Upload);
                pre += Now() - p0;
            }
            sum += Untile(src, staging.data(), Upload);
        }
        const double ms = Now() - t0;
        const double mib = double(Upload * Uploads) / (1 << 20);
        std::printf("%-28s %6.1f ms for %4.0f MiB = %.3f ms/MiB (prefault %.3f ms/MiB), %llu faults (%llu)\n", arm.name, ms, mib, ms / mib, pre / mib,
                    (unsigned long long)(Faults() - f0), (unsigned long long)sum);
        std::fflush(stdout);
        ++run;
    }
    return 0;
}

