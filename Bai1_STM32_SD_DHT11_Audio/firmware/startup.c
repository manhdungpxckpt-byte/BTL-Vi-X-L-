#include <stdint.h>
extern uint32_t _sidata,_sdata,_edata,_sbss,_ebss,_estack;
int main(void);
void SysTick_Handler(void); void TIM3_IRQHandler(void); void TIM1_UP_IRQHandler(void);
void Reset_Handler(void){
  uint32_t *s=&_sidata,*d=&_sdata; while(d<&_edata)*d++=*s++;
  for(d=&_sbss;d<&_ebss;)*d++=0;
  main(); for(;;);
}
void Default_Handler(void){ for(;;); }
__attribute__((section(".isr_vector"))) void (* const vectors[76])(void)={
  (void(*)(void))&_estack, Reset_Handler, Default_Handler, Default_Handler,
  Default_Handler,Default_Handler,Default_Handler,0,0,0,0,Default_Handler,Default_Handler,0,Default_Handler,
  SysTick_Handler,
  [16+25]=TIM1_UP_IRQHandler,
  [16+29]=TIM3_IRQHandler,
};
