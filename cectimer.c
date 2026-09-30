#include <string.h>
#include <stdio.h>     // NULL

#ifdef ENABLE_AM_FIX
    #include "am_fix.h"
#endif
#include "app/app.h"
#include "app/dtmf.h"
#include "audio.h"
#include "board.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/system.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "helper/battery.h"
#include "helper/boot.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/lock.h"
#include "ui/welcome.h"
#include "ui/menu.h"
#include "version.h"
#include "app/uart.h"


#include "driver/adc.h"

#include "font.h"
#include "driver/st7565.h"
#include "ui/helper.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "external/printf/printf.h"

#include "ARMCM0.h"
#include "driver/systick.h"
#include "misc.h"

#include "cectimer.h"
#include "ceccommon.h"

// UV-K5 V3 / UV-K1: TIM14 clocked at 1 MHz replaces the DP32G030 TIMER_BASE0
#include "py32f071_ll_bus.h"
#include "py32f071_ll_tim.h"

//Timer Handler
uint32_t timeIncVal = 0;
void TIM14_IRQHandler(void)
{
    if (LL_TIM_IsActiveFlag_UPDATE(TIM14))
    {
        LL_TIM_ClearFlag_UPDATE(TIM14);
        timeIncVal++;
    }
}

//interrupt
void CECTimer0Enable(uint8_t timerType)
{
    //milisecnd and increase timeIncVal, uint32_t range is 4,294,967,295 / 1000 (sec) / 60 (min) / 60 / (housr) / 24 (day) is about 50day, enough
    uint32_t period = 1000;             //1msec

    if (timerType == CEC_TIMER_APRS)    //1000(msec) / 1200 (1200bps)  = 0.833 (with process time)
        period = 820;
    else if (timerType == CEC_TIMER_FT8)
        period = 4;

    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_TIM14);

    LL_TIM_DisableCounter(TIM14);
    LL_TIM_SetPrescaler(TIM14, SystemCoreClock / 1000000 - 1);    //1us tick
    LL_TIM_SetAutoReload(TIM14, period - 1);
    LL_TIM_SetCounter(TIM14, 0);
    LL_TIM_GenerateEvent_UPDATE(TIM14);
    LL_TIM_ClearFlag_UPDATE(TIM14);
    LL_TIM_EnableIT_UPDATE(TIM14);
    LL_TIM_EnableCounter(TIM14);

    NVIC_SetPriority(TIM14_IRQn, 1);
    NVIC_EnableIRQ(TIM14_IRQn);
}

void CECTimer0Disable()
{
    LL_TIM_DisableCounter(TIM14);
    LL_TIM_DisableIT_UPDATE(TIM14);
    NVIC_DisableIRQ(TIM14_IRQn);
}
