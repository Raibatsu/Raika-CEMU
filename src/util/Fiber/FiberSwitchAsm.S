// FiberCtx byte offsets:
//   0x00 x19  0x08 x20  0x10 x21  0x18 x22  0x20 x23  0x28 x24
//   0x30 x25  0x38 x26  0x40 x27  0x48 x28  0x50 x29(fp) 0x58 x30(lr)
//   0x60 sp
//   0x68 d8   0x70 d9  ... 0x98 d15

	.text
	.balign 4

	.global _fiberSwap
	.type _fiberSwap, %function
// void _fiberSwap(FiberCtx* from, FiberCtx* to)
_fiberSwap:
	stp x19, x20, [x0, #0x00]
	stp x21, x22, [x0, #0x10]
	stp x23, x24, [x0, #0x20]
	stp x25, x26, [x0, #0x30]
	stp x27, x28, [x0, #0x40]
	stp x29, x30, [x0, #0x50]
	mov x2, sp
	str x2,       [x0, #0x60]
	stp d8,  d9,  [x0, #0x68]
	stp d10, d11, [x0, #0x78]
	stp d12, d13, [x0, #0x88]
	stp d14, d15, [x0, #0x98]
	ldp x19, x20, [x1, #0x00]
	ldp x21, x22, [x1, #0x10]
	ldp x23, x24, [x1, #0x20]
	ldp x25, x26, [x1, #0x30]
	ldp x27, x28, [x1, #0x40]
	ldp x29, x30, [x1, #0x50]
	ldr x2,       [x1, #0x60]
	mov sp, x2
	ldp d8,  d9,  [x1, #0x68]
	ldp d10, d11, [x1, #0x78]
	ldp d12, d13, [x1, #0x88]
	ldp d14, d15, [x1, #0x98]
	ret
	.size _fiberSwap, .-_fiberSwap

	.global _fiberTrampoline
	.type _fiberTrampoline, %function
// Fresh fibers enter with the function in x19 and argument in x20.
_fiberTrampoline:
	mov x0, x20
	blr x19
0:	b 0b          // Cemu fiber entry points never return
	.size _fiberTrampoline, .-_fiberTrampoline

	.section .note.GNU-stack, "", %progbits
