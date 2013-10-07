;---------------------------------------------------------------------
; vim:ft=nasm:foldmethod=marker
; File: src/interrupt_handler.asm
; Description:
;       It provides interrupt handler entry point
;---------------------------------------------------------------------
    bits 32

%macro push_all 0
        push DS
        push ES
        pushad
%endmacro


%macro pop_all 0
        popad
        pop ES
        pop DS
%endmacro


; 割り込みハンドラのひな形
%macro asm_interrupt_handler 1
    global asm_interrupt_handler0x%1
    extern interrupt_handler0x%1

asm_interrupt_handler0x%1:
    push_all

    call interrupt_handler0x%1

    pop_all

    iretd
%endmacro


section .text

; 割り込みハンドラを作成
asm_interrupt_handler 0x20
