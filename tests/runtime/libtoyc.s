# 本地验证用的 ToyC 运行时，行为对齐课程说明：
# putint 只写十进制数字，不附加空格或换行；getint 从标准输入读一个整数。
# _start 调用 main，并用 main 的返回值退出。评测链接的是课程提供的 libtoyc.a。

.section .text
.globl _start
_start:
	call main
	li a7, 93
	ecall

.globl putint
putint:
	addi sp, sp, -48
	sext.w t0, a0
	addi t1, sp, 32
	slt t3, t0, zero
	beqz t3, .Lput_abs
	negw t0, t0
.Lput_abs:
	beqz t0, .Lput_zero
.Lput_loop:
	li t2, 10
	remuw t4, t0, t2
	divuw t0, t0, t2
	addi t4, t4, 48
	addi t1, t1, -1
	sb t4, 0(t1)
	bnez t0, .Lput_loop
	beqz t3, .Lput_write
	addi t1, t1, -1
	li t4, 45
	sb t4, 0(t1)
	j .Lput_write
.Lput_zero:
	addi t1, t1, -1
	li t4, 48
	sb t4, 0(t1)
.Lput_write:
	addi t5, sp, 32
	sub a2, t5, t1
	mv a1, t1
	li a0, 1
	li a7, 64
	ecall
	addi sp, sp, 48
	ret

.globl getint
getint:
	addi sp, sp, -16
	sd ra, 8(sp)
.Lget_skip:
	call .Lreadc
	li t0, -1
	beq a0, t0, .Lget_eof
	li t0, 32
	beq a0, t0, .Lget_skip
	li t0, 9
	beq a0, t0, .Lget_skip
	li t0, 10
	beq a0, t0, .Lget_skip
	li t0, 13
	beq a0, t0, .Lget_skip
	li t1, 0
	li t0, 45
	bne a0, t0, .Lget_digit
	li t1, 1
	call .Lreadc
.Lget_digit:
	li t2, 0
.Lget_loop:
	li t0, 48
	blt a0, t0, .Lget_done
	li t0, 57
	bgt a0, t0, .Lget_done
	addi a0, a0, -48
	li t0, 10
	mul t2, t2, t0
	add t2, t2, a0
	call .Lreadc
	j .Lget_loop
.Lget_done:
	li t0, -1
	beq a0, t0, .Lget_sign
	la t0, has_push
	li t3, 1
	sd t3, 0(t0)
	la t0, pushed
	sd a0, 0(t0)
.Lget_sign:
	beqz t1, .Lget_out
	sub t2, zero, t2
.Lget_out:
	sext.w a0, t2
	ld ra, 8(sp)
	addi sp, sp, 16
	ret
.Lget_eof:
	li a0, 0
	ld ra, 8(sp)
	addi sp, sp, 16
	ret

.Lreadc:
	la t0, has_push
	ld t1, 0(t0)
	beqz t1, .Lread_sys
	sd zero, 0(t0)
	la t0, pushed
	ld a0, 0(t0)
	ret
.Lread_sys:
	addi sp, sp, -16
	li a0, 0
	mv a1, sp
	li a2, 1
	li a7, 63
	ecall
	blez a0, .Lread_eof
	lbu a0, 0(sp)
	addi sp, sp, 16
	ret
.Lread_eof:
	addi sp, sp, 16
	li a0, -1
	ret

.section .bss
.lcomm pushed, 8
.lcomm has_push, 8
