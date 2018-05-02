static inline double dmaclksPerClk32(){
   double dmaclks;

   if(pllIsOn()){
      uint16_t pllcr = registerArrayRead16(PLLCR);
      uint16_t pllfsr = registerArrayRead16(PLLFSR);
      double p = pllfsr & 0x00FF;
      double q = (pllfsr & 0x0F00) >> 8;
      dmaclks = 2.0 * (14.0 * (p + 1.0) + q + 1.0);

      //prescaler 1 enabled, divide by 2
      if(pllcr & 0x0080)
         dmaclks /= 2.0;

      //prescaler 2 enabled, divides value from prescaler 1 by 2
      if(pllcr & 0x0020)
         dmaclks /= 2.0;
   }
   else{
      dmaclks = 0;
   }

   return dmaclks;
}

static inline double sysclksPerClk32(){
   uint8_t sysclkSelect = registerArrayRead16(PLLCR) >> 8 & 0x0003;

   //> 4 means run at full speed, no divider
   if(sysclkSelect > 4)
      return dmaclksPerClk32();

   //divide DMACLK by 2 to the power of PLLCR SYSCLKSEL
   return dmaclksPerClk32() / (2 << sysclkSelect);
}

static inline void rtiInterruptClk32(){
   //this function is part of clk32();
   uint16_t triggeredRtiInterrupts = 0x0000;

   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 512) == 0){
      //RIS7 - 512HZ
      triggeredRtiInterrupts |= 0x8000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 256) == 0){
      //RIS6 - 256HZ
      triggeredRtiInterrupts |= 0x4000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 128) == 0){
      //RIS5 - 128HZ
      triggeredRtiInterrupts |= 0x2000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 64) == 0){
      //RIS4 - 64HZ
      triggeredRtiInterrupts |= 0x1000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 32) == 0){
      //RIS3 - 32HZ
      triggeredRtiInterrupts |= 0x0800;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 16) == 0){
      //RIS2 - 16HZ
      triggeredRtiInterrupts |= 0x0400;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 8) == 0){
      //RIS1 - 8HZ
      triggeredRtiInterrupts |= 0x0200;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 4) == 0){
      //RIS0 - 4HZ
      triggeredRtiInterrupts |= 0x0100;
   }

   triggeredRtiInterrupts &= registerArrayRead16(RTCIENR);
   if(triggeredRtiInterrupts){
      registerArrayWrite16(RTCISR, registerArrayRead16(RTCISR) | triggeredRtiInterrupts);
      setIprIsrBit(INT_RTI);
   }
}

static inline void timer12Clk32(){
   //this function is part of clk32();
   uint16_t timer1Control = registerArrayRead16(TCTL1);
   uint16_t timer1Compare = registerArrayRead16(TCMP1);
   uint16_t timer1OldCount = registerArrayRead16(TCN1);
   uint16_t timer1Count = timer1OldCount;
   double timer1Prescaler = (registerArrayRead16(TPRER1) & 0x00FF) + 1;
   uint16_t timer2Control = registerArrayRead16(TCTL2);
   uint16_t timer2Compare = registerArrayRead16(TCMP2);
   uint16_t timer2OldCount = registerArrayRead16(TCN2);
   uint16_t timer2Count = timer2OldCount;
   double timer2Prescaler = (registerArrayRead16(TPRER2) & 0x00FF) + 1;
   double sysclks = sysclksPerClk32();

   //timer 1
   if(timer1Control & 0x0001){
      //enabled
      switch((timer1Control & 0x000E) >> 1){

         case 0x0000://stop counter
         case 0x0003://TIN pin / timer prescaler, nothing is attached to TIN
            //do nothing
            break;

         case 0x0001://SYSCLK / timer prescaler
            timerCycleCounter[0] += sysclks / timer1Prescaler;
            break;

         case 0x0002://SYSCLK / 16 / timer prescaler
            timerCycleCounter[0] += sysclks / 16.0 / timer1Prescaler;
            break;

         default://CLK32 / timer prescaler
            timerCycleCounter[0] += 1.0 / timer1Prescaler;
            break;
      }

      if(timerCycleCounter[0] >= 1.0){
         timer1Count += (uint16_t)timerCycleCounter[0];
         timerCycleCounter[0] -= (uint16_t)timerCycleCounter[0];
      }

      if(timer1OldCount < timer1Compare && timer1Count >= timer1Compare){
         //the timer is not cycle accurate and may not hit the value in the compare register perfectly so check if it would have during in the emulated time
         if(timer1Control & 0x0010){
            //interrupt enabled
            setIprIsrBit(INT_TMR1);
         }

         //set timer triggered bit
         registerArrayWrite16(TSTAT1, registerArrayRead16(TSTAT1) | 0x0001);
         timerStatusReadAcknowledge[0] &= 0xFFFE;//lock bit until next read

         if(!(timer1Control & 0x0100)){
            //not free running, reset to 0, to prevent loss of ticks after compare event just subtract timerXCompare
            timer1Count -= timer1Compare;
         }
      }

      registerArrayWrite16(TCN1, timer1Count);
   }

   //timer 2
   if(timer2Control & 0x0001){
      //enabled
      switch((timer2Control & 0x000E) >> 1){

         case 0x0000://stop counter
         case 0x0003://TIN pin / timer prescaler, nothing is attached to TIN
            //do nothing
            break;

         case 0x0001://SYSCLK / timer prescaler
            timerCycleCounter[1] += sysclks / timer2Prescaler;
            break;

         case 0x0002://SYSCLK / 16 / timer prescaler
            timerCycleCounter[1] += sysclks / 16.0 / timer2Prescaler;
            break;

         default://CLK32 / timer prescaler
            timerCycleCounter[1] += 1.0 / timer2Prescaler;
            break;
      }

      if(timerCycleCounter[1] >= 1.0){
         timer2Count += (uint16_t)timerCycleCounter[1];
         timerCycleCounter[1] -= (uint16_t)timerCycleCounter[1];
      }

      if(timer2OldCount < timer2Compare && timer2Count >= timer2Compare){
         //the timer is not cycle accurate and may not hit the value in the compare register perfectly so check if it would have during in the emulated time
         if(timer2Control & 0x0010){
            //interrupt enabled
            setIprIsrBit(INT_TMR2);
         }

         //set timer triggered bit
         registerArrayWrite16(TSTAT2, registerArrayRead16(TSTAT2) | 0x0001);
         timerStatusReadAcknowledge[1] &= 0xFFFE;//lock bit until next read

         if(!(timer2Control & 0x0100)){
            //not free running, reset to 0, to prevent loss of ticks after compare event just subtract timerXCompare
            timer2Count -= timer2Compare;
         }
      }

      registerArrayWrite16(TCN2, timer2Count);
   }
}

static inline void rtcAddSecondClk32(){
   //this function is part of clk32();

   //RTC
   if(registerArrayRead16(RTCCTL) & 0x0080){
      //RTC enable bit set
      uint16_t rtcInterruptEvents;
      uint32_t newRtcTime;
      uint32_t oldRtcTime = registerArrayRead32(RTCTIME);
      uint32_t hours = oldRtcTime >> 24;
      uint32_t minutes = (oldRtcTime >> 16) & 0x0000003F;
      uint32_t seconds = oldRtcTime & 0x0000003F;

      seconds++;
      rtcInterruptEvents = 0x0010;//1 second interrupt
      if(seconds >= 60){
         minutes++;
         seconds = 0;
         rtcInterruptEvents |= 0x0002;//1 minute interrupt
         if(minutes >= 60){
            hours++;
            minutes = 0;
            rtcInterruptEvents |= 0x0020;//1 hour interrupt
            if(hours >= 24){
               hours = 0;
               uint16_t days = registerArrayRead16(DAYR);
               days++;
               registerArrayWrite16(DAYR, days & 0x01FF);
               rtcInterruptEvents |= 0x0008;//1 day interrupt
            }
         }
      }

      rtcInterruptEvents &= registerArrayRead16(RTCIENR);
      if(rtcInterruptEvents){
         registerArrayWrite16(RTCISR, registerArrayRead16(RTCISR) | rtcInterruptEvents);
         setIprIsrBit(INT_RTC);
      }

      newRtcTime = seconds & 0x0000003F;
      newRtcTime |= minutes << 16;
      newRtcTime |= hours << 24;
      registerArrayWrite32(RTCTIME, newRtcTime);
   }

   //watchdog
   uint16_t watchdogState = registerArrayRead16(WATCHDOG);
   if(watchdogState & 0x0001){
      //watchdog enabled
      watchdogState += 0x0100;//add second to watchdog timer
      watchdogState &= 0x0383;//cap overflow
      if((watchdogState & 0x0200) == 0x0200){
         //time expired
         if(watchdogState & 0x0002){
            //interrupt
            setIprIsrBit(INT_WDT);
         }
         else{
            //reset
            emulatorReset();
            return;
         }
      }
      registerArrayWrite16(WATCHDOG, watchdogState);
   }
}

void clk32(){
   registerArrayWrite16(PLLFSR, registerArrayRead16(PLLFSR) ^ 0x8000);

   //second position counter
   if(clk32Counter >= CRYSTAL_FREQUENCY - 1){
      clk32Counter = 0;
      rtcAddSecondClk32();
   }
   else{
      clk32Counter++;
   }

   //PLLCR wake select wait
   if(pllWakeWait != -1){
      if(pllWakeWait == 0){
         //reenable PLL and CPU
         registerArrayWrite16(PLLCR, registerArrayRead16(PLLCR) & 0xFFF7);
         debugLog("PLL reenabled, CPU is on!\n");
      }
      pllWakeWait--;
   }

   rtiInterruptClk32();
   timer12Clk32();

   checkInterrupts();
}
