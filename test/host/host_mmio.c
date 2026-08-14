#include "host_mmio.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Peripheral blocks. Sized from the real ST structs, so a driver writing a register at the
   far end of ETH_TypeDef (4464 bytes on this part) stays in bounds. */
static ETH_TypeDef s_eth;
static RCC_TypeDef s_rcc;
static GPIO_TypeDef s_gpioa, s_gpiob, s_gpioc, s_gpiog;
static SBS_TypeDef s_sbs;

ETH_TypeDef *host_eth = &s_eth;
RCC_TypeDef *host_rcc = &s_rcc;
GPIO_TypeDef *host_gpioa = &s_gpioa;
GPIO_TypeDef *host_gpiob = &s_gpiob;
GPIO_TypeDef *host_gpioc = &s_gpioc;
GPIO_TypeDef *host_gpiog = &s_gpiog;
SBS_TypeDef *host_sbs = &s_sbs;

HostHalLog host_hal_log;

uint32_t SystemCoreClock = 250000000U;

void host_mmio_reset(void) {
    memset(&s_eth, 0, sizeof(s_eth));
    memset(&s_rcc, 0, sizeof(s_rcc));
    memset(&s_gpioa, 0, sizeof(s_gpioa));
    memset(&s_gpiob, 0, sizeof(s_gpiob));
    memset(&s_gpioc, 0, sizeof(s_gpioc));
    memset(&s_gpiog, 0, sizeof(s_gpiog));
    memset(&s_sbs, 0, sizeof(s_sbs));
    memset(&host_hal_log, 0, sizeof(host_hal_log));
    host_msg_clear();
}

/* --- MSG() capture ---------------------------------------------------------- */

/* Generous: the driver's DMA-error reporting can emit several lines in one pass, and a
   truncated buffer would turn an assertion about a message into a flake. */
static char s_msg[16384];
static size_t s_msg_len;

const char *host_msg_text(void) { return s_msg; }

void host_msg_clear(void) {
    s_msg[0] = '\0';
    s_msg_len = 0;
}

static void msg_append(const char *text) {
    size_t n = strlen(text);
    if (s_msg_len + n >= sizeof(s_msg)) {
        n = sizeof(s_msg) - s_msg_len - 1;
    }
    memcpy(s_msg + s_msg_len, text, n);
    s_msg_len += n;
    s_msg[s_msg_len] = '\0';
}

void MSG_init(void) {}

void MSG(const char *format, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    msg_append(line);
}

void MSGchar(char c) {
    const char s[2] = {c, '\0'};
    msg_append(s);
}

void MSGraw(const char *str) { msg_append(str); }

/* --- HAL stubs -------------------------------------------------------------- */

/* Only the five HAL entry points mac_drv.c actually calls. Anything else it starts calling
   will fail to link, which is the intended behaviour: an unnoticed new hardware dependency
   should break the host build loudly rather than be silently stubbed. */

/* Signatures must match ST's headers exactly -- they are included here, so a drift is a
   compile error rather than a silent ODR violation. That already caught one: HAL_GPIO_Init
   takes a const pointer. */
void HAL_GPIO_Init(GPIO_TypeDef *GPIOx, const GPIO_InitTypeDef *pGPIO_Init) {
    (void)GPIOx;
    (void)pGPIO_Init;
    host_hal_log.gpio_init_calls++;
}

void HAL_NVIC_SetPriority(IRQn_Type irqn, uint32_t preempt, uint32_t sub) {
    (void)sub;
    host_hal_log.nvic_setpriority_calls++;
    host_hal_log.last_nvic_irqn = (uint32_t)irqn;
    host_hal_log.last_nvic_priority = preempt;
}

void HAL_NVIC_EnableIRQ(IRQn_Type irqn) {
    host_hal_log.nvic_enable_calls++;
    host_hal_log.last_nvic_irqn = (uint32_t)irqn;
}

void HAL_SBS_ETHInterfaceSelect(uint32_t iface) {
    host_hal_log.sbs_eth_iface_calls++;
    host_hal_log.last_sbs_iface = iface;
}
