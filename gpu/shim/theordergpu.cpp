#include "port_write_log.h"
#include "video_core/cache_storage.h"
// port: glue between the C loader and the vendored shadPS4 video core.
#include "port_copy.h"
#include "port_overlay.h"
#include "port_png.h"
#include "port_settings.h"

#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "../theordergpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "port_toggles.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void *data,
                                 uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end,
                          int *mapped);
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read,
                                int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_read_tsc(void);
int32_t *runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap,
                                  RuntimeGpuRange invalidate);
// runtime.c: exit counters (and the ISA site report), as on the guest's own
// exit
void runtime_report(void);
}

Frontend::WindowSDL *g_window = nullptr;

namespace Libraries::GnmDriver {
void RegisterLib(Core::Loader::SymbolsResolver *sym);
}
namespace Libraries::AvPlayer {
void RegisterLib(Core::Loader::SymbolsResolver *sym);
}
namespace Libraries::VideoOut {
void RegisterLib(Core::Loader::SymbolsResolver *sym);
}
namespace Libraries::Kernel {
void RegisterEventQueue(Core::Loader::SymbolsResolver *sym);
}

namespace {
struct Symbol {
  std::string nid, library, module;
  u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char *nid, const char *library,
                                const char *module, SymbolType, u64 address) {
  g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
  static void FillElfInfo(const TheOrderGpuConfig &config) {
    auto &info = Common::ElfInfo::Instance();
    info.initialized = true;
    info.game_serial = config.serial ? config.serial : "UNKNOWN";
    info.title = config.title ? config.title : "";
    info.sdk_ver = config.sdk_version;
    info.psf_attributes.raw = config.psf_attributes;
  }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer *rasterizer_) {
  rasterizer = rasterizer_;
  // Existing guest mappings are replayed by the runtime when hooks are
  // installed.
  runtime_memory_set_gpu_hooks(
      [](uintptr_t address, uint64_t size) {
        auto *r = Memory::Instance()->GetRasterizer();
        r->RegisterMemory(address, size);
        r->MapMemory(address, size);
      },
      [](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->UnmapMemory(address, size);
      },
      [](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
      });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
  if (rasterizer)
    rasterizer->InvalidateMemory(address, size);
}
bool MemoryManager::IsValidGpuMapping(VAddr virtual_addr, u64 size) {
  if (virtual_addr < 0x10000ULL ||
      virtual_addr + size >= 0x100'0000'0000ULL) {
    return false;
  }
  const u64 check_size = std::max<u64>(size, 1);
  if (rasterizer && rasterizer->IsMapped(virtual_addr, check_size)) {
    return true;
  }
  return runtime_memory_is_mapped(virtual_addr, check_size) != 0;
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
  return runtime_memory_clamp(virtual_addr, size);
}
// Through the backing view, as upstream: a host copy thread reading a page the GPU tracking
// protects for reads (precise readbacks) faulted into ReadMemory, which waits for the draw
// recorder: a deadlock when the copy ran on the recorder itself.
static void CopySparseSerial(VAddr source, u8 *dest, u64 size) {
  runtime_memory_read_backing(source, dest, size);
}
void MemoryManager::CopySparseMemory(VAddr source, u8 *dest, u64 size) {
  // port: large uploads (streaming) are split across the copy threads.
  constexpr u64 Chunk = 512 * 1024;
  if (size < 4 * Chunk || !PortCopy::Enabled()) {
    return CopySparseSerial(source, dest, size);
  }
  PortCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
    const u64 offset = i * Chunk;
    CopySparseSerial(source + offset, dest + offset,
                     std::min(Chunk, size - offset));
  });
}
bool MemoryManager::TryWriteBacking(void *address, const void *data, u64 size) {
  PortWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size,
                     PortWriteLog::Backing);
  const bool written = runtime_memory_write_backing(
                           reinterpret_cast<uintptr_t>(address), data, size) != 0;
  // The write skipped page tracking: textures over it must reload. Labels and fences
  // (<= 8 bytes) never hold texels; each would send every texture through the locked path.
  if (written && rasterizer && size > 8) {
    rasterizer->GetTextureCache().NoteBackingWrite(
        reinterpret_cast<VAddr>(address), size);
  }
  return written;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size,
                           MemoryPermission perms) {
  PortStats::Timer timer{PortStats::t_protect};
  const u64 pages = (size + 4095) / 4096;
  PortStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
  PortStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
  if (!True(perms & MemoryPermission::Write)) {
    PortStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
    PortStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
  }
  runtime_memory_gpu_protect(virtual_addr, size,
                             True(perms & MemoryPermission::Read),
                             True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
  boost::icl::interval_set<VAddr> set;
  set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL,
                                                 0xfc'0000'0000ULL);
  return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return runtime_read_tsc(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() {
  return runtime_process_time_counter();
}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() {
  return 1'000'000'000;
}
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32 *ver) {
  if (!ver)
    return ORBIS_KERNEL_ERROR_EINVAL;
  *ver = s32(g_sdk_version);
  return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
  std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
  return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host
// thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 PortDisplayRefreshHz() {
  static const u32 hz = [] {
    const SDL_DisplayMode *mode =
        SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    const u32 rate =
        mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
    std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
    return std::max<u32>(rate, 60);
  }();
  return hz;
}

#ifdef THEORDER_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (THEORDER_PGO=generate): the game often ends through _exit
// (watchdog, guest exit), which skips the profile write at exit. Write every 30
// s and reset: the files sum the intervals.
static void StartProfileWriter() {
  std::thread([] {
    Common::SetCurrentThreadName("port:pgo");
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(30));
      __gcov_dump();
      __gcov_reset();
      std::printf("PGO: profile written\n");
    }
  }).detach();
}
#endif

extern "C" int theordergpu_audio_volume_percent(void) {
    return PortSettings::Get().master_volume.load(std::memory_order_relaxed);
}

extern "C" int theordergpu_audio_channels(void) {
    return PortSettings::Get().audio_channels.load(std::memory_order_relaxed);
}

extern "C" void theordergpu_wait_while_paused(void) {
    PortOverlay::WaitWhilePaused("sound");
}

namespace {
/// Exit to Desktop and closing the window both report and flush the shader cache once: the
/// second waits for the first to end the process.
std::atomic<bool> g_exiting{false};
void ExitOnce() {
    if (g_exiting.exchange(true)) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
} // namespace

extern "C" void theordergpu_exit_to_desktop(void) {
    ExitOnce();
    std::printf("Overlay: Exit to Desktop\n");
    runtime_report();
    theordergpu_flush_cache();
    std::fflush(stdout);
    std::_Exit(0);
}

extern "C" void theordergpu_flush_cache(void) {
    const char* env = std::getenv("THEORDER_CACHE_FLUSH_ON_EXIT");
    static const bool enabled = !(env && env[0] == '0');
    auto& cache = Storage::DataBase::Instance();
    if (!enabled || !cache.IsOpened()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const bool done = cache.Flush(std::chrono::seconds(2));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::printf("Shader cache: %s at exit in %lld ms\n",
                done ? "queue drained" : "queued writes NOT finished",
                static_cast<long long>(ms));
}

extern "C" int theordergpu_init(const TheOrderGpuConfig *config) {
  PortSettings::Load();
#ifdef THEORDER_PGO_GENERATE
  StartProfileWriter();
#endif
  g_sdk_version = config->sdk_version;
#ifdef _WIN32
  if (config->user_dir && !std::getenv("THEORDER_GPU_USER_DIR"))
    _putenv_s("THEORDER_GPU_USER_DIR", config->user_dir);
#else
  if (config->user_dir)
    setenv("THEORDER_GPU_USER_DIR", config->user_dir, 0);
#endif
  Core::Emulator::FillElfInfo(*config);
  const std::string title = config->title ? config->title : "The Order: 1886";
  const auto& s = PortSettings::Get();
  s32 width = config->width, height = config->height;
  if (s.output_res >= 0 && s.output_res < PortSettings::OutputCount) {
    width = PortSettings::OutputWidths[s.output_res];
    height = PortSettings::OutputHeights[s.output_res];
  }
  g_window_thread = std::thread([title, width, height] {
    Common::SetCurrentThreadName("port:window");
    auto *window = new Frontend::WindowSDL(width, height, title.c_str());
    {
      std::scoped_lock lock{g_window_mutex};
      g_window = window;
      g_window_ready = true;
    }
    g_window_cv.notify_all();
    while (window->PollEvents()) {
      SDL_Delay(2);
    }
    ExitOnce();
    LOG_INFO(Frontend, "Window closed by user");
    runtime_report();
    theordergpu_flush_cache();
    std::fflush(stdout);
    std::_Exit(0);
  });
  g_window_thread.detach();
  {
    std::unique_lock lock{g_window_mutex};
    g_window_cv.wait(lock, [] { return g_window_ready; });
  }
  Core::Loader::SymbolsResolver resolver;
  // GnmDriver creates the presenter that the VideoOut present thread uses.
  Libraries::GnmDriver::RegisterLib(&resolver);
  Libraries::VideoOut::RegisterLib(&resolver);
  return 0;
}

namespace Libraries::Kernel {
void StartKernelService();
}
extern "C" void theordergpu_register_kernel(void) {
  Libraries::Kernel::StartKernelService();
  Core::Loader::SymbolsResolver resolver;
  Libraries::Kernel::RegisterEventQueue(&resolver);
  Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t theordergpu_resolve(const char *scoped_nid) {
  const char *hash = std::strchr(scoped_nid, '#');
  const size_t length =
      hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
  for (const auto &symbol : g_symbols) {
    if (symbol.nid.size() == length &&
        !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
      return uintptr_t(symbol.address);
    }
  }
  return 0;
}

extern "C" int theordergpu_handle_fault(void *ucontext, void *address) {
  return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address)
             ? 1
             : 0;
}

extern "C" unsigned theordergpu_symbol_count(void) {
  return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
  std::unique_lock lock{m_asio_req};
  ++asio_requests;
  cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
  Common::SetCurrentThreadName("port:kernel_service");
  while (!stoken.stop_requested()) {
    {
      std::unique_lock lock{m_asio_req};
      cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
    }
    if (stoken.stop_requested())
      break;
    io_context.run();
    io_context.restart();
    asio_requests = 0;
  }
}

void StartKernelService() {
  service_thread = std::jthread{KernelServiceThread};
}

int *PS4_SYSV_ABI __Error() { return runtime_errno(); }
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path &, const std::string &) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" void theordergpu_set_fov_sites(double *const *sites, int count) {
  PortSettings::SetFovSites(sites, count);
}

extern "C" int theordergpu_overlay_captures_input(void) {
  return PortOverlay::CapturesInput() ? 1 : 0;
}

extern "C" int theordergpu_text_input_begin(const char *initial,
                                        const char *prompt) {
  if (!g_window)
    return 0;
  g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
  return 1;
}

extern "C" int theordergpu_choice_begin(const char *title, const char *const *items,
                                    const char *const *images, int count,
                                    int focus) {
  if (!g_window || count <= 0)
    return 0;
  // Pictures are decoded here, on the game's thread, before the list is shown.
  std::vector<PortPng::Image> pictures(count);
  for (int i = 0; images && i < count; ++i) {
    if (images[i] && !PortPng::LoadRgba(images[i], pictures[i])) {
      LOG_WARNING(Lib_SaveDataDialog, "save list: no picture from {}",
                  images[i]);
    }
  }
  PortOverlay::BeginChoice(title ? title : "",
                           std::vector<std::string>(items, items + count),
                           std::move(pictures), focus);
  return 1;
}

extern "C" int theordergpu_choice_poll(void) { return PortOverlay::PollChoice(); }

extern "C" int theordergpu_text_input_poll(char *out, uint64_t size) {
  if (!g_window)
    return 2;
  std::string text;
  const int state = g_window->PollTextInput(text);
  if (size) {
    const size_t n = std::min<size_t>(text.size(), size - 1);
    std::memcpy(out, text.data(), n);
    out[n] = 0;
  }
  return state;
}
