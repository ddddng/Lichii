// Fault cost of reading a section page through a second view (HostMemory backing vs fastmem view).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
constexpr size_t Region = size_t{256} << 20;
constexpr size_t Regions = 12;

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

static u8* view_a;
static u8* view_b;
static std::vector<u8> sink(Region);

static void Write(u8* p, size_t n) {
    for (size_t i = 0; i < n; i += 64) {
        std::memset(p + i, int(i >> 12) | 1, 64);
    }
}
static uint64_t Read(const u8* p, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; i += 64) {
        s += *reinterpret_cast<const volatile uint64_t*>(p + i);
    }
    return s;
}
static void Copy(const u8* p, size_t n) {
    std::memcpy(sink.data(), p, n);
}
static void TouchParallel(const u8* p, size_t n, int threads) {
    std::vector<std::thread> t;
    std::atomic<uint64_t> s{};
    for (int k = 0; k < threads; ++k) {
        t.emplace_back([=, &s] {
            uint64_t x = 0;
            const size_t pages = n / Page;
            const size_t begin = pages * k / threads, end = pages * (k + 1) / threads;
            for (size_t i = begin; i < end; ++i) x += *reinterpret_cast<const volatile u8*>(p + i * Page);
            s += x;
        });
    }
    for (auto& x : t) x.join();
}

struct Result { double pre_ms; uint64_t pre_faults; double ms; uint64_t faults; };
static void Report(const char* name, const Result& r) {
    const double mib = double(Region) / (1 << 20);
    std::printf("%-44s prep %8.1f ms %8llu faults | read %8.1f ms %8llu faults  %6.2f GB/s  %.2f us/fault(read)\n", name,
                r.pre_ms, (unsigned long long)r.pre_faults, r.ms, (unsigned long long)r.faults,
                mib / 1024 / (r.ms / 1000), r.faults ? r.ms * 1000 / r.faults : 0.0);
    std::fflush(stdout);
}

template <class Prep, class Use>
static void Run(const char* name, size_t region, Prep prep, Use use) {
    u8* a = view_a + region * Region;
    u8* b = view_b + region * Region;
    Write(a, Region);
    Result r{};
    uint64_t f0 = Faults();
    double t0 = Now();
    prep(a, b);
    r.pre_ms = Now() - t0;
    r.pre_faults = Faults() - f0;
    f0 = Faults();
    t0 = Now();
    use(a, b);
    r.ms = Now() - t0;
    r.faults = Faults() - f0;
    Report(name, r);
}

int main() {
    const size_t size = Region * Regions;
#ifdef _WIN32
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                        DWORD(uint64_t(size) >> 32), DWORD(size), nullptr);
    view_a = static_cast<u8*>(MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size));
    view_b = static_cast<u8*>(MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size));
    SIZE_T wmin = 0, wmax = 0;
    GetProcessWorkingSetSize(GetCurrentProcess(), &wmin, &wmax);
    std::printf("working set min %zu KiB max %zu KiB\n", wmin >> 10, wmax >> 10);
#else
    int fd = memfd_create("bench", 0);
    ftruncate(fd, size);
    view_a = static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    view_b = static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
#endif
    if (!view_a || !view_b) { std::printf("map failed\n"); return 1; }
    std::memset(sink.data(), 0, sink.size());
    auto none = [](u8*, u8*) {};
    auto read_b = [](u8*, u8* b) { Read(b, Region); };
    auto copy_b = [](u8*, u8* b) { Copy(b, Region); };
    size_t i = 0;
    Run("control: write A, read A", i++, none, [](u8* a, u8*) { Read(a, Region); });
    Run("write A, read B", i++, none, read_b);
    Run("write A, memcpy B", i++, none, copy_b);
    Run("write B, read B", i++, [](u8*, u8* b) { Write(b, Region); }, read_b);
    for (int threads : {2, 4, 8}) {
        char name[64];
        std::snprintf(name, sizeof(name), "write A, touch B x%d threads, read B", threads);
        Run(name, i++, [threads](u8*, u8* b) { TouchParallel(b, Region, threads); }, read_b);
    }
#ifdef _WIN32
    Run("write A, PrefetchVirtualMemory B, read B", i++, [](u8*, u8* b) {
        WIN32_MEMORY_RANGE_ENTRY e{b, Region};
        if (!PrefetchVirtualMemory(GetCurrentProcess(), 1, &e, 0)) std::printf("prefetch err %lu\n", GetLastError());
    }, read_b);
    Run("write A, VirtualLock+Unlock B, read B", i++, [](u8*, u8* b) {
        SIZE_T wmin = 0, wmax = 0;
        GetProcessWorkingSetSize(GetCurrentProcess(), &wmin, &wmax);
        if (!SetProcessWorkingSetSize(GetCurrentProcess(), wmin + Region + (16 << 20), wmax + Region + (32 << 20)))
            std::printf("setws err %lu\n", GetLastError());
        if (!VirtualLock(b, Region)) std::printf("lock err %lu\n", GetLastError());
        VirtualUnlock(b, Region);
        SetProcessWorkingSetSize(GetCurrentProcess(), wmin, wmax);
    }, read_b);
    Run("write A, ReadProcessMemory B -> sink", i++, none, [](u8*, u8* b) {
        SIZE_T done = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), b, sink.data(), Region, &done)) std::printf("rpm err %lu\n", GetLastError());
    });
    Run("write A, read B, VirtualUnlock B (HintCold), read B", i++, [](u8*, u8* b) {
        Read(b, Region);
        VirtualUnlock(b, Region);
    }, read_b);
    Run("write A, VirtualUnlock A+B (HintCold both), read B", i++, [](u8* a, u8* b) {
        Read(b, Region);
        VirtualUnlock(a, Region);
        VirtualUnlock(b, Region);
    }, read_b);
#else
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif
    Run("write A, MADV_POPULATE_READ B, read B", i++, [](u8*, u8* b) {
        if (madvise(b, Region, MADV_POPULATE_READ) != 0) std::perror("populate");
    }, read_b);
    Run("write A, MADV_WILLNEED B, read B", i++, [](u8*, u8* b) { madvise(b, Region, MADV_WILLNEED); }, read_b);
#endif
    return 0;
}

