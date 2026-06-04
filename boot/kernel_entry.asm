[bits 32]
[global _start]
[extern k_main]

_start:
    call k_main
    cli
    hlt