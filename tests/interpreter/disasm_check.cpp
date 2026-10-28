// Throwaway diagnostic: disassemble the Test E byte sequence with Zydis's own formatter to
// verify the hand-encoded bytes actually mean what the test comments claim.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <Zydis/Zydis.h>

int main() {
    std::vector<std::uint8_t> code = {
        0x48, 0x8B, 0x07,             // mov rax, [rdi]
        0x48, 0x8B, 0x5F, 0x08,       // mov rbx, [rdi+8]
        0x48, 0x8B, 0x0C, 0x24,       // mov rcx, [rsp]
        0x48, 0x8B, 0x54, 0x24, 0x08, // mov rdx, [rsp+8]
        0xFF, 0xD6,                   // call rsi
        0xC3};

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

    std::size_t offset = 0;
    std::uint64_t addr = 0x1000;
    while (offset < code.size()) {
        ZydisDecodedInstruction instr;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderDecodeFull(&decoder, code.data() + offset,
                                                     code.size() - offset, &instr, ops);
        if (!ZYAN_SUCCESS(status)) {
            std::printf("%#llx: decode failed\n", (unsigned long long)addr);
            break;
        }
        char buf[256];
        ZydisFormatterFormatInstruction(&formatter, &instr, ops, instr.operand_count_visible,
                                         buf, sizeof(buf), addr, nullptr);
        std::printf("%#llx: %s\n", (unsigned long long)addr, buf);
        offset += instr.length;
        addr += instr.length;
    }
    return 0;
}
