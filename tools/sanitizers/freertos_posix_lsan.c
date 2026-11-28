/* LeakSanitizer's suppressions for FreeRTOS's POSIX port, which two host tests run on.
 *
 * The port keeps per-thread bookkeeping that it frees when a task is deleted, and not for a task
 * still alive when the process exits: a 1-byte marker that tags a pthread as a FreeRTOS thread
 * (prvMarkAsFreeRTOSThread), and the event each task waits on (event_create, from
 * pxPortInitialiseStack). LeakSanitizer runs at exit and reports both. Measured under
 * --config=asan: one object of each, in teardown_test and test_rtos_context -- a task that was
 * never deleted, not a task deleted and leaked. Neither is this project's code to fix.
 *
 * What this costs: a suppression matches any allocation with that function in its stack, so a
 * port that DID leak these per deleted task would be hidden too. Nothing else would catch it --
 * both are plain malloc(), outside the guard-page heap's count -- so the evidence is the measured
 * one: teardown_test creates and deletes the PTP task several times over, and still only one
 * object of each was reported. Re-measure (drop this target's dep) after bumping FreeRTOS.
 *
 * A function, not an LSAN_OPTIONS file, so the suppression travels with the targets that need it
 * and cannot be lost from a command line. It must be linked into the executable itself:
 * LeakSanitizer finds it through a weak symbol the sanitizer runtime defines, and the runtime is
 * loaded before every shared library, so a definition in one of Bazel's per-library .so files
 * would lose to it. Hence linkstatic and alwayslink in BUILD.bazel. Without the sanitizers nothing
 * refers to it and an opt link's --gc-sections drops it, which is harmless; with them the runtime's
 * definition makes the linker export this one, and an exported symbol is kept (checked with
 * --config=asan --config=opt).
 */

const char *__lsan_default_suppressions(void);

const char *__lsan_default_suppressions(void) {
    return "leak:prvMarkAsFreeRTOSThread\n"
           "leak:event_create\n";
}
