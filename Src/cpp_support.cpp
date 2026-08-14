/* The project's first C++ translation unit.
 *
 * WHY THIS FILE EXISTS
 *
 * Every prerequisite for C++ was already configured in this project, and none of it had ever
 * been exercised because no .cpp file existed:
 *
 *   - enable_language(CXX) and C++20                        CMakeLists.txt
 *   - -fno-rtti -fno-exceptions -fno-threadsafe-statics     cmake/gcc-arm-none-eabi.cmake
 *   - -lstdc++ -lsupc++ on the link line                    CMakeLists.txt
 *   - .init_array / .fini_array / .ARM.extab / .ARM.exidx   stm32h563zitx_FLASH.ld
 *   - bl __libc_init_array before main()                    Startup/startup_stm32h563zitx.s
 *
 * A configuration that merely *looks* right is exactly the class of thing this project has
 * been bitten by before (F62, F70): both were correct-looking code that the toolchain
 * quietly did something else with. So the point of this file is to make the C++ runtime
 * observable on hardware rather than assumed -- specifically that `__libc_init_array()`
 * really does reach `.init_array` and run a file-scope constructor before `main()`.
 *
 * It is deliberately tiny, and it is checked by `cpp_ctor` in tools/hil/.
 *
 * RULES FOR C++ IN THIS FIRMWARE
 *
 *   - No dynamic allocation. No new, no std::vector/string/function. There is no RTOS heap
 *     to allocate from, and operator new would fall through to newlib malloc -> _sbrk.
 *   - No exceptions, no RTTI. Already off by compiler flag; do not rely on either.
 *   - extern "C" at every boundary. flexPTP's NSD, NetX Duo's driver entry and the MAC
 *     driver's callbacks are all C; C++ stays on the inside.
 *   - Static storage and fixed-capacity containers only.
 */

#include <cstdint>

namespace {

/* A file-scope object with a non-trivial constructor. The compiler emits an entry in
 * .init_array for it; the linker script KEEPs that section; __libc_init_array() walks it
 * before main(). If any link in that chain is broken, `ran` stays false and the CLI says so
 * instead of the firmware silently behaving as though C++ were fine.
 *
 * `ran` is zero-initialised (it lands in .bss, cleared by the startup code) and set by the
 * constructor, so the two phases are distinguishable: false means dynamic initialisation did
 * not happen, not merely that this object is missing.
 */
class StaticInitProbe {
  public:
    StaticInitProbe() : magic_(kMagic) { ran_ = true; }

    static bool ran() { return ran_; }

    /** Guards against the constructor having run but the object then being clobbered --
     *  a wrong .data/.bss layout would show up here rather than as a plausible "ok". */
    bool intact() const { return magic_ == kMagic; }

  private:
    static constexpr std::uint32_t kMagic = 0x5AFEC0DEu;
    std::uint32_t magic_;
    static bool ran_;
};

bool StaticInitProbe::ran_ = false;

/* Not `static`: an anonymous-namespace object already has internal linkage, and the
 * .init_array entry is what keeps it alive through --gc-sections. */
const StaticInitProbe g_probe;

}  // namespace

extern "C" {

/**
 * One-word summary of the C++ runtime state, for `osinfo`.
 *
 * "ok" is only returned when the constructor ran AND the object still holds its magic.
 */
const char *cpp_static_init_status(void) {
    if (!StaticInitProbe::ran()) {
        return "DID NOT RUN";
    }
    if (!g_probe.intact()) {
        return "RAN BUT CLOBBERED";
    }
    return "ok";
}

/** The C++ standard this was compiled against, so `osinfo` can report it factually. */
unsigned long cpp_standard(void) { return __cplusplus; }

}  // extern "C"
