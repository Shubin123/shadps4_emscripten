// x86-64 test functions for test_main.cpp, System V calling convention. Assembled into
// guest_code.inc by gen_guest_code.sh.
.intel_syntax noprefix
.text
f_add: mov eax, edi
 add eax, esi
 ret
f_sum: xor eax, eax
 mov ecx, 1
1: cmp rcx, rdi
 jg 2f
 add rax, rcx
 inc rcx
 jmp 1b
2: ret
f_muldiv: mov rax, rdi
 imul rax, rsi
 xor edx, edx
 mov rcx, 10
 div rcx
 ret
f_muldiv2: mov rax, rdi
 imul rax, rsi
 cqo
 mov rcx, 7
 idiv rcx
 ret
f_calls: push rbx
 mov rbx, rdi
 call 3f
 add rax, rbx
 pop rbx
 ret
3: lea rax, [rbx + rbx*2 + 5]
 ret
f_flags: xor eax, eax
 cmp edi, esi
 setl al
 mov ecx, 100
 cmovg eax, ecx
 ret
f_strlen: mov rcx, -1
 xor eax, eax
 repne scasb
 not rcx
 lea rax, [rcx - 1]
 ret
f_memcpy: mov rcx, rdx
 mov rax, rdi
 rep movsb
 ret
f_cmpxchg: mov eax, esi
 lock cmpxchg dword ptr [rdi], edx
 sete al
 movzx eax, al
 ret
f_xadd: mov eax, esi
 lock xadd dword ptr [rdi], eax
 ret
f_sse_scalar: cvtsi2ss xmm0, edi
 cvtsi2ss xmm1, esi
 mulss xmm0, xmm1
 addss xmm0, xmm0
 cvttss2si eax, xmm0
 ret
f_sse_packed: movdqu xmm0, xmmword ptr [rdi]
 movdqu xmm1, xmmword ptr [rsi]
 paddd xmm0, xmm1
 pshufd xmm0, xmm0, 0x1b
 movdqu xmmword ptr [rdi], xmm0
 pxor xmm2, xmm2
 ptest xmm2, xmm2
 setz al
 movzx eax, al
 ret
f_pshufb: movdqu xmm0, xmmword ptr [rdi]
 movdqu xmm1, xmmword ptr [rsi]
 pshufb xmm0, xmm1
 movdqu xmmword ptr [rdi], xmm0
 ret
f_avx: vmovups ymm0, ymmword ptr [rdi]
 vmovups ymm1, ymmword ptr [rsi]
 vaddps ymm0, ymm0, ymm1
 vmovups ymmword ptr [rdi], ymm0
 vextractf128 xmm2, ymm0, 1
 vmovd eax, xmm2
 vzeroupper
 ret
f_x87: mov dword ptr [rsp - 8], edi
 fild dword ptr [rsp - 8]
 fld st(0)
 fmulp st(1), st(0)
 fistp dword ptr [rsp - 8]
 mov eax, dword ptr [rsp - 8]
 ret
f_double: addsd xmm0, xmm1
 mulsd xmm0, xmm0
 ret
f_hle: push rbx
 mov rbx, rdi
 mov edi, 40
 mov rsi, rbx
 movsd xmm0, qword ptr [rip + 4f]
 call qword ptr [rbx]
 pop rbx
 ret
4: .quad 0x4004000000000000
f_tls: mov rax, qword ptr fs:[0]
 ret
f_sum8: lea rax, [rdi + rsi]
 add rax, rdx
 add rax, rcx
 add rax, r8
 add rax, r9
 add rax, qword ptr [rsp + 8]
 add rax, qword ptr [rsp + 16]
 ret
f_dsum9: addsd xmm0, xmm1
 addsd xmm0, xmm2
 addsd xmm0, xmm3
 addsd xmm0, xmm4
 addsd xmm0, xmm5
 addsd xmm0, xmm6
 addsd xmm0, xmm7
 addsd xmm0, qword ptr [rsp + 8]
 ret
// Paleblood hooks: f_pb_original (3a + b) is the function a replacement takes over; it's longer
// than the 14-byte jump the hook writes. f_pb_caller calls it the way game code would.
f_pb_original: lea eax, [rdi + rdi*2]
 add eax, esi
 nop
 nop
 nop
 nop
 nop
 nop
 nop
 nop
 nop
 ret
f_pb_caller: push rbx
 mov ebx, edi
 call f_pb_original
 add eax, ebx
 pop rbx
 ret
// An original the replacement calls by name (RT_ORIGINAL): a * a.
f_pb_square: mov eax, edi
 imul eax, edi
 ret
// A vtable slot the replacement calls (rt::guest_fn): b * object->bias.
f_pb_slot: mov eax, esi
 imul eax, dword ptr [rdi + 8]
 ret
