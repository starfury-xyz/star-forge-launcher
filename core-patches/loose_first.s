# Replaces the file table sort call in FUN_14001aee0 (game.exe 1.99.0.167293 and 1.99.0.167301).
# rcx = begin, rdx = end of the {char *name; int32 id; pad} table, r8 = count, r9 = sort state.
# Packed entries have negative ids, loose ones >= 0, and the sort orders equal
# names by signed id, so packed files win. Flipping bit 31 around the sort makes
# that an unsigned order: loose files first (by datasource order), then packs (in
# their original order). The ids are restored before anything else reads them.
        .intel_syntax noprefix
        # sort, table and count are given to the linker:
        #   --defsym sort=0x14001f730 --defsym table=0x140f81b10 --defsym count=0x140f81b18
        .globl _start
_start:
        mov rax, rcx
1:      cmp rax, rdx
        jae 2f
        xor dword ptr [rax + 8], 0x80000000
        add rax, 16
        jmp 1b
2:      sub rsp, 0x28
        call sort
        add rsp, 0x28
        mov rax, qword ptr [rip + table]
        mov edx, dword ptr [rip + count]
        shl rdx, 4
        add rdx, rax
3:      cmp rax, rdx
        jae 4f
        xor dword ptr [rax + 8], 0x80000000
        add rax, 16
        jmp 3b
4:      ret
