; Startup file cho STM32F103 - Keil ARM Compiler 5 (armasm)
Stack_Size      EQU     0x00000800
                AREA    STACK, NOINIT, READWRITE, ALIGN=3
Stack_Mem       SPACE   Stack_Size
__initial_sp

Heap_Size       EQU     0x00000200
                AREA    HEAP, NOINIT, READWRITE, ALIGN=3
__heap_base
Heap_Mem        SPACE   Heap_Size
__heap_limit

                PRESERVE8
                THUMB

                AREA    RESET, DATA, READONLY
                EXPORT  __Vectors
__Vectors       DCD     __initial_sp
                DCD     Reset_Handler
                DCD     Default_Handler     ; NMI
                DCD     Default_Handler     ; HardFault
                DCD     Default_Handler     ; MemManage
                DCD     Default_Handler     ; BusFault
                DCD     Default_Handler     ; UsageFault
                DCD     0,0,0,0
                DCD     Default_Handler     ; SVC
                DCD     Default_Handler     ; DebugMon
                DCD     0
                DCD     Default_Handler     ; PendSV
                DCD     SysTick_Handler
                ; IRQ 0..28
                DCD     Default_Handler,Default_Handler,Default_Handler,Default_Handler
                DCD     Default_Handler,Default_Handler,Default_Handler,Default_Handler
                DCD     Default_Handler,Default_Handler,Default_Handler,Default_Handler
                DCD     Default_Handler,Default_Handler,Default_Handler,Default_Handler
                DCD     Default_Handler,Default_Handler,Default_Handler,Default_Handler
                DCD     Default_Handler,Default_Handler,Default_Handler,Default_Handler
                DCD     Default_Handler,Default_Handler,Default_Handler,Default_Handler
                DCD     Default_Handler
                DCD     TIM3_IRQHandler     ; IRQ 29

                AREA    |.text|, CODE, READONLY
Reset_Handler   PROC
                EXPORT  Reset_Handler
                IMPORT  __main
                LDR     R0, =__main
                BX      R0
                ENDP

Default_Handler PROC
                EXPORT  Default_Handler   [WEAK]
                B       .
                ENDP

SysTick_Handler PROC
                EXPORT  SysTick_Handler   [WEAK]
                B       .
                ENDP

TIM3_IRQHandler PROC
                EXPORT  TIM3_IRQHandler   [WEAK]
                B       .
                ENDP
                ALIGN

                IF      :DEF:__MICROLIB
                EXPORT  __initial_sp
                EXPORT  __heap_base
                EXPORT  __heap_limit
                ELSE
                IMPORT  __use_two_region_memory
                EXPORT  __user_initial_stackheap
__user_initial_stackheap
                LDR     R0, = Heap_Mem
                LDR     R1, = (Stack_Mem + Stack_Size)
                LDR     R2, = (Heap_Mem +  Heap_Size)
                LDR     R3, = Stack_Mem
                BX      LR
                ALIGN
                ENDIF
                END
