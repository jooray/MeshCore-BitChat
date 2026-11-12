#ifdef NRF52_SERIES

#include <Arduino.h>
#include "../../helpers/bitchat/BitchatDebugSerial.h"
#include <FreeRTOS.h>
#include <task.h>
#include <malloc.h>

// External declarations for heap monitoring
extern "C" char* sbrk(int incr);
extern char __HeapBase;  // Linker symbol for heap start

// Stack usage monitoring - NRF52 linker symbols
extern unsigned long __StackTop;   // Top of stack (defined in linker script)
extern unsigned long __StackLimit; // Bottom limit of stack (defined in linker script)

static uint32_t crash_count = 0;

// Fault status register addresses
#define SCB_CFSR  (*(volatile uint32_t *)0xE000ED28)  // Configurable Fault Status Register
#define SCB_HFSR  (*(volatile uint32_t *)0xE000ED2C)  // HardFault Status Register
#define SCB_DFSR  (*(volatile uint32_t *)0xE000ED30)  // Debug Fault Status Register
#define SCB_AFSR  (*(volatile uint32_t *)0xE000ED3C)  // Auxiliary Fault Status Register
#define SCB_BFAR  (*(volatile uint32_t *)0xE000ED38)  // Bus Fault Address Register
#define SCB_MMAR  (*(volatile uint32_t *)0xE000ED34)  // MemManage Fault Address Register

// CFSR bit definitions
#define CFSR_IACCVIOL    (1 << 0)   // Instruction access violation
#define CFSR_DACCVIOL    (1 << 1)   // Data access violation
#define CFSR_MUNSTKERR   (1 << 3)   // MemManage fault on unstacking
#define CFSR_MSTKERR     (1 << 4)   // MemManage fault on stacking
#define CFSR_MLSPERR     (1 << 5)   // MemManage fault during lazy FP state preservation
#define CFSR_MMARVALID   (1 << 7)   // MMAR valid flag
#define CFSR_IBUSERR     (1 << 8)   // Instruction bus error
#define CFSR_PRECISERR   (1 << 9)   // Precise data bus error
#define CFSR_IMPRECISERR (1 << 10)  // Imprecise data bus error
#define CFSR_UNSTKERR    (1 << 11)  // Bus fault on unstacking
#define CFSR_STKERR      (1 << 12)  // Bus fault on stacking
#define CFSR_LSPERR      (1 << 13)  // Bus fault during lazy FP state preservation
#define CFSR_BFARVALID   (1 << 15)  // BFAR valid flag
#define CFSR_UNDEFINSTR  (1 << 16)  // Undefined instruction
#define CFSR_INVSTATE    (1 << 17)  // Invalid state
#define CFSR_INVPC       (1 << 18)  // Invalid PC
#define CFSR_NOCP        (1 << 19)  // No coprocessor
#define CFSR_UNALIGNED   (1 << 24)  // Unaligned access
#define CFSR_DIVBYZERO   (1 << 25)  // Divide by zero

// HFSR bit definitions
#define HFSR_VECTTBL     (1 << 1)   // Vector table read fault
#define HFSR_FORCED      (1 << 30)  // Forced hard fault
#define HFSR_DEBUGEVT    (1 << 31)  // Debug event

extern "C" void HardFault_Handler(void) __attribute__((naked));

extern "C" void HardFault_Handler(void) {
    // Save registers and call C handler
    __asm volatile (
        "tst lr, #4\n"                  // Test bit 2 of LR
        "ite eq\n"                       // If-Then-Else
        "mrseq r0, msp\n"               // Use MSP if bit 2 was 0
        "mrsne r0, psp\n"               // Use PSP if bit 2 was 1
        "b HardFault_Handler_C\n"       // Branch to C handler with stack pointer in r0
        : : : "r0"
    );
}

extern "C" void HardFault_Handler_C(uint32_t *hardfault_args) {
    crash_count++;

    uint32_t stacked_r0 = hardfault_args[0];
    uint32_t stacked_r1 = hardfault_args[1];
    uint32_t stacked_r2 = hardfault_args[2];
    uint32_t stacked_r3 = hardfault_args[3];
    uint32_t stacked_r12 = hardfault_args[4];
    uint32_t stacked_lr = hardfault_args[5];
    uint32_t stacked_pc = hardfault_args[6];
    uint32_t stacked_psr = hardfault_args[7];

    uint32_t cfsr = SCB_CFSR;
    uint32_t hfsr = SCB_HFSR;
    uint32_t dfsr = SCB_DFSR;
    uint32_t afsr = SCB_AFSR;
    uint32_t bfar = SCB_BFAR;
    uint32_t mmar = SCB_MMAR;

    BITCHAT_SERIAL.println("\n\n");
    BITCHAT_SERIAL.println("========================================");
    BITCHAT_SERIAL.println("=== HARDFAULT HANDLER TRIGGERED ===");
    BITCHAT_SERIAL.println("========================================");
    BITCHAT_SERIAL.print("Crash count: ");
    BITCHAT_SERIAL.println((unsigned)crash_count);
    BITCHAT_SERIAL.print("Uptime: ");
    BITCHAT_SERIAL.print((unsigned)millis());
    BITCHAT_SERIAL.println(" ms");
    BITCHAT_SERIAL.println();

    BITCHAT_SERIAL.println("--- CPU Registers ---");
    BITCHAT_SERIAL.print("R0  = 0x"); BITCHAT_SERIAL.println(stacked_r0, HEX);
    BITCHAT_SERIAL.print("R1  = 0x"); BITCHAT_SERIAL.println(stacked_r1, HEX);
    BITCHAT_SERIAL.print("R2  = 0x"); BITCHAT_SERIAL.println(stacked_r2, HEX);
    BITCHAT_SERIAL.print("R3  = 0x"); BITCHAT_SERIAL.println(stacked_r3, HEX);
    BITCHAT_SERIAL.print("R12 = 0x"); BITCHAT_SERIAL.println(stacked_r12, HEX);
    BITCHAT_SERIAL.print("LR  = 0x"); BITCHAT_SERIAL.println(stacked_lr, HEX);
    BITCHAT_SERIAL.print("PC  = 0x"); BITCHAT_SERIAL.println(stacked_pc, HEX);
    BITCHAT_SERIAL.print("PSR = 0x"); BITCHAT_SERIAL.println(stacked_psr, HEX);
    BITCHAT_SERIAL.println();

    BITCHAT_SERIAL.println("--- Fault Status Registers ---");
    BITCHAT_SERIAL.print("CFSR = 0x"); BITCHAT_SERIAL.println(cfsr, HEX);
    BITCHAT_SERIAL.print("HFSR = 0x"); BITCHAT_SERIAL.println(hfsr, HEX);
    BITCHAT_SERIAL.print("DFSR = 0x"); BITCHAT_SERIAL.println(dfsr, HEX);
    BITCHAT_SERIAL.print("AFSR = 0x"); BITCHAT_SERIAL.println(afsr, HEX);

    if (cfsr & CFSR_MMARVALID) {
        BITCHAT_SERIAL.print("MMAR = 0x"); BITCHAT_SERIAL.println(mmar, HEX);
    }
    if (cfsr & CFSR_BFARVALID) {
        BITCHAT_SERIAL.print("BFAR = 0x"); BITCHAT_SERIAL.println(bfar, HEX);
    }
    BITCHAT_SERIAL.println();

    BITCHAT_SERIAL.println("--- Fault Analysis ---");

    // MemManage faults (CFSR bits 0-7)
    if (cfsr & 0xFF) {
        BITCHAT_SERIAL.println("MemManage Fault:");
        if (cfsr & CFSR_IACCVIOL) BITCHAT_SERIAL.println("  - Instruction access violation");
        if (cfsr & CFSR_DACCVIOL) BITCHAT_SERIAL.println("  - Data access violation");
        if (cfsr & CFSR_MUNSTKERR) BITCHAT_SERIAL.println("  - MemManage fault on exception return");
        if (cfsr & CFSR_MSTKERR) BITCHAT_SERIAL.println("  - MemManage fault on exception entry");
        if (cfsr & CFSR_MLSPERR) BITCHAT_SERIAL.println("  - MemManage fault during lazy FP state save");
        if (cfsr & CFSR_MMARVALID) {
            BITCHAT_SERIAL.print("  - Fault address: 0x");
            BITCHAT_SERIAL.println(mmar, HEX);
        }
    }

    // BusFault (CFSR bits 8-15)
    if (cfsr & 0xFF00) {
        BITCHAT_SERIAL.println("Bus Fault:");
        if (cfsr & CFSR_IBUSERR) BITCHAT_SERIAL.println("  - Instruction bus error");
        if (cfsr & CFSR_PRECISERR) BITCHAT_SERIAL.println("  - Precise data bus error");
        if (cfsr & CFSR_IMPRECISERR) BITCHAT_SERIAL.println("  - Imprecise data bus error");
        if (cfsr & CFSR_UNSTKERR) BITCHAT_SERIAL.println("  - Bus fault on exception return");
        if (cfsr & CFSR_STKERR) BITCHAT_SERIAL.println("  - Bus fault on exception entry (STACK OVERFLOW?)");
        if (cfsr & CFSR_LSPERR) BITCHAT_SERIAL.println("  - Bus fault during lazy FP state save");
        if (cfsr & CFSR_BFARVALID) {
            BITCHAT_SERIAL.print("  - Fault address: 0x");
            BITCHAT_SERIAL.println(bfar, HEX);
        }
    }

    // UsageFault (CFSR bits 16-25)
    if (cfsr & 0x03FF0000) {
        BITCHAT_SERIAL.println("Usage Fault:");
        if (cfsr & CFSR_UNDEFINSTR) BITCHAT_SERIAL.println("  - Undefined instruction");
        if (cfsr & CFSR_INVSTATE) BITCHAT_SERIAL.println("  - Invalid state");
        if (cfsr & CFSR_INVPC) BITCHAT_SERIAL.println("  - Invalid PC");
        if (cfsr & CFSR_NOCP) BITCHAT_SERIAL.println("  - No coprocessor");
        if (cfsr & CFSR_UNALIGNED) BITCHAT_SERIAL.println("  - Unaligned access");
        if (cfsr & CFSR_DIVBYZERO) BITCHAT_SERIAL.println("  - Divide by zero");
    }

    // HardFault status
    if (hfsr & HFSR_VECTTBL) BITCHAT_SERIAL.println("HardFault: Vector table read error");
    if (hfsr & HFSR_FORCED) BITCHAT_SERIAL.println("HardFault: Escalated from configurable fault");
    if (hfsr & HFSR_DEBUGEVT) BITCHAT_SERIAL.println("HardFault: Debug event");

    BITCHAT_SERIAL.println();

    // Print stack trace (best effort - may be corrupted)
    BITCHAT_SERIAL.println("--- Stack Trace (last 10 frames) ---");
    uint32_t *stack_ptr = hardfault_args;
    for (int i = 0; i < 10; i++) {
        BITCHAT_SERIAL.print("SP+");
        BITCHAT_SERIAL.print((unsigned)(i * 4));
        BITCHAT_SERIAL.print(": 0x");
        BITCHAT_SERIAL.println(stack_ptr[i], HEX);
    }
    BITCHAT_SERIAL.println();

    BITCHAT_SERIAL.println("========================================");
    BITCHAT_SERIAL.println("System halted. Reset required.");
    BITCHAT_SERIAL.println("========================================");
    BITCHAT_SERIAL.flush();

    // Halt the system
    while (1) {
        __WFI();  // Wait for interrupt (low power mode)
    }
}

// Stack usage monitoring function (call from main loop)
extern "C" void print_stack_usage(void) {
    BITCHAT_SERIAL.println("### STACK_MONITOR_START ###");

    // 1. ISR/SoftDevice Stack (2KB - measured via canary scan)
    // Note: ISR stack uses MSP which is only readable in handler mode.
    // From task context, we scan for canary pattern painted at boot.
    uint32_t isr_stack_top = (uint32_t)&__StackTop;
    uint32_t isr_stack_bottom = (uint32_t)&__StackLimit;
    uint32_t isr_stack_size = isr_stack_top - isr_stack_bottom;

    // Scan from bottom upward to find first overwritten canary
    uint32_t* scan_ptr = (uint32_t*)isr_stack_bottom;
    uint32_t isr_stack_unused = 0;
    for (uint32_t i = 0; i < isr_stack_size / 4; i++) {
        if (scan_ptr[i] != 0xa5a5a5a5) {
            break;  // Found overwritten canary
        }
        isr_stack_unused += 4;
    }
    uint32_t isr_stack_used = isr_stack_size - isr_stack_unused;

    BITCHAT_SERIAL.println("\n[ISR/SoftDevice Stack - 2KB via Canary Scan]");
    BITCHAT_SERIAL.print("  Size:       "); BITCHAT_SERIAL.print((unsigned)isr_stack_size); BITCHAT_SERIAL.println(" bytes");
    BITCHAT_SERIAL.print("  Unused:     "); BITCHAT_SERIAL.print((unsigned)isr_stack_unused); BITCHAT_SERIAL.println(" bytes");
    BITCHAT_SERIAL.print("  Peak used:  "); BITCHAT_SERIAL.print((unsigned)isr_stack_used); BITCHAT_SERIAL.println(" bytes");

    if (isr_stack_size > 0) {
        uint32_t isr_usage = (isr_stack_used * 100) / isr_stack_size;
        BITCHAT_SERIAL.print("  Peak usage: "); BITCHAT_SERIAL.print((unsigned)isr_usage); BITCHAT_SERIAL.println("%");
        if (isr_usage > 90) {
            BITCHAT_SERIAL.println("  ⚠️  WARNING: ISR stack usage above 90%!");
        } else if (isr_usage > 75) {
            BITCHAT_SERIAL.println("  ⚠️  CAUTION: ISR stack usage above 75%");
        }
    }

    // 2. FreeRTOS Loop Task Stack (used by loop() and main application)
    // Get current task handle (this function is called from loop, so it's the loop task)
    TaskHandle_t loopHandle = xTaskGetCurrentTaskHandle();
    if (loopHandle != NULL) {
        UBaseType_t task_stack_watermark = uxTaskGetStackHighWaterMark(loopHandle);
        // FreeRTOS watermark is in words (4 bytes each), and shows minimum free space ever reached
        uint32_t task_stack_min_free = task_stack_watermark * 4;
#ifdef LOOP_STACK_SZ
        uint32_t task_stack_size = LOOP_STACK_SZ * 4;  // LOOP_STACK_SZ in words, convert to bytes
#else
        uint32_t task_stack_size = 256 * 4 * 4;  // Default: 1024 words = 4096 bytes
#endif
        uint32_t task_stack_max_used = task_stack_size - task_stack_min_free;

        BITCHAT_SERIAL.print("\n[Loop Task Stack - ");
        BITCHAT_SERIAL.print((unsigned)(task_stack_size / 1024));
        BITCHAT_SERIAL.println("KB]");
        BITCHAT_SERIAL.print("  Size:        "); BITCHAT_SERIAL.print((unsigned)task_stack_size); BITCHAT_SERIAL.println(" bytes");
        BITCHAT_SERIAL.print("  Min free:    "); BITCHAT_SERIAL.print((unsigned)task_stack_min_free); BITCHAT_SERIAL.println(" bytes");
        BITCHAT_SERIAL.print("  Max used:    "); BITCHAT_SERIAL.print((unsigned)task_stack_max_used); BITCHAT_SERIAL.println(" bytes");

        if (task_stack_size > 0) {
            uint32_t task_usage = (task_stack_max_used * 100) / task_stack_size;
            BITCHAT_SERIAL.print("  Peak usage:  "); BITCHAT_SERIAL.print((unsigned)task_usage); BITCHAT_SERIAL.println("%");
            if (task_usage > 90) {
                BITCHAT_SERIAL.println("  ⚠️  WARNING: Task stack usage above 90%!");
            } else if (task_usage > 75) {
                BITCHAT_SERIAL.println("  ⚠️  CAUTION: Task stack usage above 75%");
            }
        }

        // VERIFICATION: Manual canary scan from current SP
        // This provides an independent check of actual stack headroom
        BITCHAT_SERIAL.println("\n  [Canary Verification]");
        uint32_t* sp;
        __asm volatile ("mov %0, sp" : "=r" (sp));
        BITCHAT_SERIAL.printf("  Current SP:  0x%08X\n", (unsigned)(uint32_t)sp);

        // Scan upward from SP counting consecutive canary words (0xa5a5a5a5)
        // FreeRTOS fills stack with this pattern at task creation
        uint32_t canary_count = 0;
        uint32_t* canary_scan = sp;
        // Scan up to 8KB above SP looking for canary pattern
        for (uint32_t i = 0; i < 2048 && (uint32_t)&canary_scan[i] < (uint32_t)sp + task_stack_size; i++) {
            if (canary_scan[i] == 0xa5a5a5a5) {
                canary_count++;
            } else {
                break;  // Found non-canary, stop
            }
        }
        uint32_t canary_headroom = canary_count * 4;
        BITCHAT_SERIAL.printf("  Canary headroom: %u bytes (%u words above SP still 0xa5a5a5a5)\n",
                      (unsigned)canary_headroom, (unsigned)canary_count);

        // The canary scan shows actual available headroom from current position
        // If this differs significantly from FreeRTOS watermark, the watermark may be unreliable
        if (canary_headroom > 0 && task_stack_min_free > 0) {
            int32_t diff = (int32_t)canary_headroom - (int32_t)task_stack_min_free;
            if (diff > 1024 || diff < -1024) {
                BITCHAT_SERIAL.printf("  ⚠️  Mismatch: Canary shows %u bytes, watermark shows %u bytes\n",
                              (unsigned)canary_headroom, (unsigned)task_stack_min_free);
                BITCHAT_SERIAL.println("  Note: FreeRTOS watermark may be unreliable on this platform");
            }
        }
    } else {
        BITCHAT_SERIAL.println("\n[Loop Task Stack]");
        BITCHAT_SERIAL.println("  Task handle not available");
    }

    // 3. Heap Memory Usage
    BITCHAT_SERIAL.println("\n[Heap Memory]");
    struct mallinfo mi = mallinfo();
    BITCHAT_SERIAL.printf("  Arena:       %d bytes (total heap size)\n", mi.arena);
    BITCHAT_SERIAL.printf("  Used:        %d bytes (in use)\n", mi.uordblks);
    BITCHAT_SERIAL.printf("  Free:        %d bytes (available)\n", mi.fordblks);

    // Also check via sbrk for comparison
    char* heap_current = sbrk(0);
    if (heap_current != (char*)-1) {
        uint32_t heap_used_sbrk = (uint32_t)heap_current - (uint32_t)&__HeapBase;
        BITCHAT_SERIAL.printf("  sbrk used:   %u bytes (heap growth from base)\n", (unsigned)heap_used_sbrk);
    }

    // Warning if heap is low (SoftDevice needs heap for BLE buffers)
    if (mi.fordblks < 10240) {
        BITCHAT_SERIAL.println("  ⚠️  WARNING: Free heap < 10KB! SoftDevice BLE may fail.");
    } else if (mi.fordblks < 20480) {
        BITCHAT_SERIAL.println("  ⚠️  CAUTION: Free heap < 20KB. Monitor BLE operations.");
    }

    BITCHAT_SERIAL.println("### STACK_MONITOR_END ###\n");
}

#endif // NRF52_SERIES
