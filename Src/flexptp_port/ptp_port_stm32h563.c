#include "ptp_port_stm32h563.h"

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "stm32h5xx_hal.h"

#include <flexptp_options.h>


static unsigned sFreq = 1;

void ptphw_set_pps_freq(double freq) {
    #ifdef PPS_SIMPLE
        int fc_exp;

        if (freq > 0) {
            fc_exp = round(log2f(freq)) + 1;

            // lower limit
            if (fc_exp < ETHHW_PTP_PPS_1Hz) {
                fc_exp = ETHHW_PTP_PPS_1Hz;
            }

            // upper limit
            if (fc_exp > ETHHW_PTP_PPS_16384Hz) {
                fc_exp = ETHHW_PTP_PPS_16384Hz;
            }

            sFreq = exp2(fc_exp - 1);
        } else {
            sFreq = 0;
            fc_exp = 0;
        }

        ETHHW_SetPTPPPSFreq(ETH, fc_exp);

#elif defined(PPS_PULSETRAIN)
        // convert to integer (integer only!)
        uint32_t freq_Hz = (uint32_t)freq;

        if (freq_Hz == 0) {
            // stop pulse train generation
            ETHHW_StopPTPPPSPulseTrain(ETH);
        } else {
            // compute period [ns]
            uint32_t period_ns = NANO_PREFIX / freq_Hz;

            // display warning if frequency is not integer divisor of 1E+09
            if ((NANO_PREFIX % freq_Hz) != 0) {
                MSG("Warning! PPS frequency is not totally accurate, "
                    "choose a frequency corresponding to a period "
                    "that is an integer divisor of 1E+09!\n");
            }

            // set duty cycle (try 50%) by specifying positive pulse length
            uint32_t high_ns = period_ns / 2;

            // start pulse train generation
            ETHHW_StartPTPPPSPulseTrain(ETH, high_ns, period_ns);
        }

        // store frequency setting
        sFreq = freq_Hz;
#else
#error "No decision was made on how the PPS signal is generated! Please define either PPS_SIMPLE or PPS_PULSETRAIN!"
#endif
}

void ptphw_restart_pps() {
    double freq = sFreq;
    ptphw_set_pps_freq(0);
    ptphw_set_pps_freq(freq);
}

#ifdef CLI_REG_CMD

static int CB_pps(const CliToken_Type *ppArgs, uint8_t argc) {
    if (argc >= 1) {
        double freq = atof(ppArgs[0]);
        ptphw_set_pps_freq(freq);
    }

    if (sFreq > 0) {
        MSG("PPS frequency: %u Hz\n", sFreq);
    } else {
        MSG("PPS output is turned off.\n");
    }

    return 0;
}

static void ptphw_register_cli_commands() {
    CLI_REG_CMD("ptp pps {freq} \t\t\tSet or query PPS signal frequency [Hz]", 2, 0, CB_pps);
}

#endif // CLI_REG_CMD

void ptphw_init(uint32_t increment, uint32_t addend) {
    MSG("Turning PTP on!\n");

    // Initialisation order matters, and differs from the H743 port this file derives
    // from. RM0481 requires the subsecond increment to be programmed BEFORE the addend:
    // writing MACTSAR sets TSADDREG, which the hardware only self-clears once the
    // timestamp counter is actually running. With MACSSIR still zero the counter never
    // advances, TSADDREG never clears, and ETHHW_SetPTPAddend() spins forever.
    //
    // The H743 set the addend first and got away with it; on the H563 it hangs the PTP
    // task with MACSSIR = 0 and MACSTSR frozen at 0. Verified on hardware.

    // 1. enable PTP timestamping (TSENA)
    ETHHW_EnablePTPTimeStamping(ETH);

    vTaskDelay(pdMS_TO_TICKS(10));

    // 2. programme the subsecond increment (MACSSIR) -- must precede the addend
    ETHHW_SetPTPSubsecondIncrement(ETH, increment);

    // 3. select fine correction (TSCFUPDT)
    ETHHW_EnablePTPFineCorr(ETH, true);

    // 4. programme the addend (MACTSAR + TSADDREG)
    if (!ETHHW_SetPTPAddend(ETH, addend)) {
        // The timestamp unit is not accepting updates -- almost always its clock. Say so
        // here rather than letting the symptom surface later as a clock that never
        // disciplines (F53 presented exactly that way).
        MSG("PTP: hardware clock did not accept the addend; timestamp unit is not running\n");
    }

    // 5. initialise PTP time (TSINIT)
    ETHHW_InitPTPTime(ETH, 0, 0);

    ptphw_set_pps_freq(1.0);

    __HAL_RCC_GPIOG_CLK_ENABLE();

    // setup PPS-pin
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.Pin = GPIO_PIN_8;
    GPIO_InitStructure.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStructure.Alternate = GPIO_AF11_ETH;
    GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStructure.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStructure);

#ifdef CLI_REG_CMD
    // register cli commands
    ptphw_register_cli_commands();
#endif // CLI_REG_CMD
}

void ptphw_gettime(TimestampU *pTime) {
    do {
        pTime->sec = ETH->MACSTSR;
        pTime->nanosec = ETH->MACSTNR & ETH_MACSTNR_TSSS;
    } while (pTime->sec != ETH->MACSTSR);
}
