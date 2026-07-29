// void co_switch(void **save_up,void *new_sp);
// rdi : 存放[舊rsp]的位置	rsi = 要切換過去的新 rsp
	.text
	.global co_switch
	.type	co_switch, @function
	.align 16
co_switch:
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	
	movq %rsp, (%rdi) //舊rsp -> (*save_up)
	movq %rsi, %rsp   //<-換stack

	//倒序
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	ret //從新stack拿返回位置跳回去
	.size co_switch , .-co_switch
	.section .note.GNU-stack,"",@progbits
