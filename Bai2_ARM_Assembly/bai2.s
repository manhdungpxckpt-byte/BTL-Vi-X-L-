; =====================================================================
; Bai 2 - Hop ngu ARM (Cortex-M3)
; "Hello World" luu tai 0x77FFFFFF -> copy sang 0x88FFFFFF
; -> doi hoa/thuong ("hELLO wORLD") luu vao 0x99FFFFFF
; Dung voi startup chuan cua Keil (Device -> Startup), ham main viet bang ASM
; =====================================================================
                PRESERVE8
                THUMB
                AREA    |.text|, CODE, READONLY

main            PROC
                EXPORT  main

; ---- B0: dat chuoi "Hello World" vao dia chi 0x77FFFFFF ----
                LDR     R0, =SRC_STR
                LDR     R1, =0x77FFFFFF
INIT_LOOP       LDRB    R2, [R0], #1
                STRB    R2, [R1], #1
                CMP     R2, #0
                BNE     INIT_LOOP

; ---- B1: copy 0x77FFFFFF -> 0x88FFFFFF ----
                LDR     R0, =0x77FFFFFF
                LDR     R1, =0x88FFFFFF
COPY_LOOP       LDRB    R2, [R0], #1
                STRB    R2, [R1], #1
                CMP     R2, #0              ; gap '\0' thi dung
                BNE     COPY_LOOP

; ---- B2: doi hoa <-> thuong, luu vao 0x99FFFFFF ----
                LDR     R0, =0x88FFFFFF
                LDR     R1, =0x99FFFFFF
CASE_LOOP       LDRB    R2, [R0], #1
                CMP     R2, #0
                BEQ     CASE_DONE
                CMP     R2, #0x41           ; 'A'
                BLT     CASE_STORE
                CMP     R2, #0x5A           ; 'Z'
                BLE     CASE_FLIP
                CMP     R2, #0x61           ; 'a'
                BLT     CASE_STORE
                CMP     R2, #0x7A           ; 'z'
                BGT     CASE_STORE
CASE_FLIP       EOR     R2, R2, #0x20       ; dao bit 5
CASE_STORE      STRB    R2, [R1], #1
                B       CASE_LOOP
CASE_DONE       MOVS    R2, #0
                STRB    R2, [R1]            ; ket thuc chuoi

STOP_HERE       B       STOP_HERE           ; dung tai day, mo Memory de xem
                ENDP

SRC_STR         DCB     "Hello World", 0
                ALIGN
                END
