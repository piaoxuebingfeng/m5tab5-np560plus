/*
 * dmcu_exec.c - 指令执行主体。
 *
 * 已完整移植（与 helper.c 逐语义对齐）：
 *   exec_move   (7102)   exec_alu (7023)   exec_shift (7238)
 *   exec_bit    (7415)   exec_control 语义核心 (18346，剥离全部观察器)
 *   主循环补充: EXCH(22970) MUL/UMUL(22982) PUSH/POP SFR(23015) LINK(23023)
 *   UNLINK(23029) NEG(22976) CRIT/IGNDS(23064) REPB/REP(23079) RESET(23052)
 *   重复引擎回绕 + IGNDS/CRIT 到期收尾 (24255-24318)
 *
 * 远调用的 mask ROM 服务经 bus->hle_service 钩子委托宿主
 * （对应 QEMU np560_overlay_service / np560_rom_service 契约）。
 *
 * 编码空间划分（CN1916884A + exec_decoded helper.c:20278）：
 *   op<0xC0 且 lo 0..7   ALU
 *   op>=0xE0             位操作（BST/BCLD 等）
 *   0xD0..0xDE           条件近跳转
 *   lo 8..11 (op<0xC0)   MOV/MOVB
 *   lo 12                SHIFT        lo 13  位域(0x6c..0x8d)+杂项
 *   控制流: 0x9a..0x9d 0xac 0xad 0xbc 0xbd 0xc6 0xcc 0xcd 0xce
 */
#include "dmcu_exec.h"
#include "dmcu_irq.h"
#include "esp_attr.h"   /* IRAM_ATTR：host shim 为空；ESP32 搬热点到内部 SRAM */

void dmcu_unsupported(Np560Bus *bus, DMCUCpu *cpu, uint32_t pc, uint16_t word)
{
    (void)bus;
    if (!cpu->halted) {
        cpu->halted = true;
        cpu->fault_pc = pc & MASK24;
        cpu->fault_word = word;
    }
    cpu->unsupported_count++;
    cpu->pc = pc & MASK24;
}

/* ---- 栈/返回辅助（helper.c 6734-6757） ---- */

static inline uint32_t dmcu_near_pc(uint32_t pc, uint32_t low)
{
    return dmcu_code_address(dmcu_code_segment(pc), low);
}

/* RETS 压栈/弹栈只含 16 位 PC（保持当前 CS）。 */
static void near_return_push(Np560Bus *bus, DMCUCpu *cpu, uint32_t pc)
{
    dmcu_push(bus, cpu, dmcu_code_pc(pc));
}

static uint32_t near_return_pop(Np560Bus *bus, DMCUCpu *cpu)
{
    return dmcu_code_address(dmcu_code_segment(cpu->pc), dmcu_pop(bus, cpu));
}

/* SCALL 先压 PC 后压 CS，栈顶为 CS（helper.c 6739）。 */
static void far_return_push(Np560Bus *bus, DMCUCpu *cpu, uint32_t pc)
{
    dmcu_push(bus, cpu, dmcu_code_pc(pc));
    dmcu_push(bus, cpu, dmcu_code_segment(pc));
}

static uint32_t far_return_pop(Np560Bus *bus, DMCUCpu *cpu)
{
    uint32_t cs = dmcu_pop(bus, cpu) & 0xff;
    uint32_t pc = dmcu_pop(bus, cpu);

    return dmcu_code_address(cs, pc);
}

/* ---- exec_move（helper.c:7102 原样移植） ---- */

static bool exec_move(Np560Bus *bus, DMCUCpu *cpu, uint8_t op, uint8_t arg,
                      uint16_t ext)
{
    int bits;
    unsigned dr, sr;
    uint32_t ptr, value, next;

    switch (op) {
    case 0x08: case 0x09:
        bits = op == 0x09 ? 8 : 16;
        dmcu_reg_write(cpu, arg >> 4, dmcu_reg_read(cpu, arg, bits), bits);
        return true;
    case 0x0a: case 0x0b:
        bits = op == 0x0b ? 8 : 16;
        dr = arg >> 4;
        dmcu_pointer_write(bus, cpu, dr, cpu->gpr[dr],
                           dmcu_reg_read(cpu, arg, bits), bits);
        return true;
    case 0x18: case 0x19:
        value = dmcu_reg_read(cpu, arg, 8);
        dmcu_reg_write(cpu, arg >> 4, op == 0x18 ? (uint32_t)dmcu_sext8(value) : value, 16);
        return true;
    case 0x1a: case 0x1b:
        bits = op == 0x1b ? 8 : 16;
        dr = arg >> 4;
        ptr = cpu->gpr[dr];
        next = dmcu_pointer_postincrement(cpu, dr, ptr, bits / 8);
        dmcu_pointer_write(bus, cpu, dr, ptr, dmcu_reg_read(cpu, arg, bits), bits);
        cpu->gpr[dr] = next;
        return true;
    case 0x28: case 0x29:
        bits = op == 0x29 ? 8 : 16;
        sr = arg & 15;
        dmcu_reg_write(cpu, arg >> 4,
                       dmcu_pointer_read(bus, cpu, sr, cpu->gpr[sr], bits), bits);
        return true;
    case 0x2a: case 0x2b:
        bits = op == 0x2b ? 8 : 16;
        dr = arg >> 4;
        sr = arg & 15;
        dmcu_pointer_write(bus, cpu, dr, cpu->gpr[dr],
                           dmcu_pointer_read(bus, cpu, sr, cpu->gpr[sr], bits), bits);
        return true;
    case 0x38: case 0x39:
        bits = op == 0x39 ? 8 : 16;
        sr = arg & 15;
        ptr = cpu->gpr[sr];
        next = dmcu_pointer_postincrement(cpu, sr, ptr, bits / 8);
        value = dmcu_pointer_read(bus, cpu, sr, ptr, bits);
        /* AGU 更新属于操作数取回；装载的目的寄存器在其后写入
         * （原厂 08:2FF4 故意用 R7 同时承担两种角色）。 */
        cpu->gpr[sr] = next;
        dmcu_reg_write(cpu, arg >> 4, value, bits);
        return true;
    case 0x3a: case 0x3b:
        bits = op == 0x3b ? 8 : 16;
        sr = arg & 15;
        ptr = cpu->gpr[sr];
        next = dmcu_pointer_postincrement(cpu, sr, ptr, bits / 8);
        dr = arg >> 4;
        dmcu_pointer_write(bus, cpu, dr, cpu->gpr[dr],
                           dmcu_pointer_read(bus, cpu, sr, ptr, bits), bits);
        cpu->gpr[sr] = next;
        return true;
    case 0x48: case 0x49:
        dmcu_reg_write(cpu, arg >> 4, arg & 15, op == 0x49 ? 8 : 16);
        return true;
    case 0x58: case 0x59:
        bits = op == 0x59 ? 8 : 16;
        dr = arg >> 4;
        cpu->gpr[dr] = (cpu->gpr[dr] - bits / 8) & MASK16;
        dmcu_pointer_write(bus, cpu, dr, cpu->gpr[dr],
                           dmcu_reg_read(cpu, arg, bits), bits);
        return true;
    case 0x5a: case 0x5b: {
        uint32_t dptr, sptr, dnext, snext;

        bits = op == 0x5b ? 8 : 16;
        dr = arg >> 4;
        sr = arg & 15;
        dptr = cpu->gpr[dr];
        sptr = cpu->gpr[sr];
        dnext = dmcu_pointer_postincrement(cpu, dr, dptr, bits / 8);
        /* 双操作数指向同一指针时保留既有的双重更新顺序。 */
        snext = dmcu_pointer_postincrement(cpu, sr, dr == sr ? dnext : sptr,
                                           bits / 8);
        value = dmcu_pointer_read(bus, cpu, sr, sptr, bits);
        dmcu_pointer_write(bus, cpu, dr, dptr, value, bits);
        cpu->gpr[dr] = dnext;
        cpu->gpr[sr] = snext;
        return true;
    }
    case 0x68: case 0x69:
        dmcu_sfr_write(cpu, arg, ext, op == 0x69 ? 8 : 16);
        return true;
    case 0x6a: case 0x6b:
        bits = op == 0x6b ? 8 : 16;
        sr = arg & 15;
        dmcu_pointer_write(bus, cpu, sr, cpu->gpr[sr] + ext,
                           dmcu_reg_read(cpu, arg >> 4, bits), bits);
        return true;
    case 0x78: case 0x79:
        bits = op == 0x79 ? 8 : 16;
        sr = arg & 15;
        dmcu_reg_write(cpu, arg >> 4,
                       dmcu_pointer_read(bus, cpu, sr, cpu->gpr[sr] + ext, bits),
                       bits);
        return true;
    case 0x88: case 0x89:
        bits = op == 0x89 ? 8 : 16;
        dmcu_sfr_write(cpu, arg, dmcu_data_read(bus, cpu, ext, bits), bits);
        return true;
    case 0x8a: case 0x8b:
        if (arg >= 16) {
            return false;
        }
        bits = op == 0x8b ? 8 : 16;
        dmcu_pointer_write(bus, cpu, arg, cpu->gpr[arg],
                           dmcu_data_read(bus, cpu, ext, bits), bits);
        return true;
    case 0x98: case 0x99:
        value = dmcu_data_read(bus, cpu, ext, 8);
        dmcu_sfr_write(cpu, arg, op == 0x98 ? (uint32_t)dmcu_sext8(value) : value, 16);
        return true;
    case 0xa8: case 0xa9:
        bits = op == 0xa9 ? 8 : 16;
        dmcu_data_write(bus, cpu, ext, dmcu_sfr_read(cpu, arg, bits), bits);
        return true;
    case 0xb8: case 0xb9:
        value = dmcu_sfr_read(cpu, arg, 8);
        dmcu_data_write(bus, cpu, ext, op == 0xb8 ? (uint32_t)dmcu_sext8(value) : value, 16);
        return true;
    default:
        return false;
    }
}

/* ---- exec_alu（helper.c:7023 原样移植） ---- */

static bool exec_alu(Np560Bus *bus, DMCUCpu *cpu, uint8_t op, uint8_t arg,
                     uint16_t ext)
{
    unsigned operation = op & 15;
    unsigned hi = op >> 4;
    bool byte_mode;
    int bits;
    uint32_t left, right, result, dst;
    enum { DST_GPR, DST_SFR, DST_MEM } dst_kind;

    if (operation > 7) {
        return false;
    }
    byte_mode = hi == 0 || hi == 2 || hi == 4 ||
                hi == 6 || hi == 8 || hi == 10;
    bits = byte_mode ? 8 : 16;

    if (hi == 0 || hi == 1) {
        dst_kind = DST_GPR;
        dst = arg >> 4;
        left = dmcu_reg_read(cpu, dst, bits);
        right = dmcu_reg_read(cpu, arg, bits);
    } else if (hi == 2 || hi == 3) {
        unsigned ptrreg = 8 + (arg & 7);
        uint32_t ptr = cpu->gpr[ptrreg];
        uint32_t nxt = arg & 8 ?
            dmcu_pointer_postincrement(cpu, ptrreg, ptr, bits / 8) : ptr;

        dst_kind = DST_GPR;
        dst = arg >> 4;
        left = dmcu_reg_read(cpu, dst, bits);
        right = dmcu_pointer_read(bus, cpu, ptrreg, ptr, bits);
        if (arg & 8) {
            cpu->gpr[ptrreg] = nxt;
        }
    } else if (hi == 4 || hi == 5) {
        dst_kind = DST_GPR;
        dst = arg >> 4;
        left = dmcu_reg_read(cpu, dst, bits);
        right = arg & 15;
    } else if (hi == 6 || hi == 7) {
        dst_kind = DST_SFR;
        dst = arg;
        left = dmcu_sfr_read(cpu, dst, bits);
        right = ext & (byte_mode ? 0xff : 0xffff);
    } else if (hi == 8 || hi == 9) {
        dst_kind = DST_SFR;
        dst = arg;
        left = dmcu_sfr_read(cpu, dst, bits);
        right = dmcu_data_read(bus, cpu, ext, bits);
    } else if ((hi == 10 || hi == 11) && operation != 6) {
        dst_kind = DST_MEM;
        dst = ext;
        left = dmcu_data_read(bus, cpu, dst, bits);
        right = dmcu_sfr_read(cpu, arg, bits);
    } else {
        return false;
    }

    result = dmcu_alu_op(cpu, operation, left, right, bits);
    if (operation != 6) {
        if (dst_kind == DST_GPR) {
            dmcu_reg_write(cpu, dst, result, bits);
        } else if (dst_kind == DST_SFR) {
            dmcu_sfr_write(cpu, dst, result, bits);
        } else {
            dmcu_data_write(bus, cpu, dst, result, bits);
        }
    }
    return true;
}

/* ---- exec_shift（helper.c:7238 原样移植） ---- */

static bool exec_shift(Np560Bus *bus, DMCUCpu *cpu, uint8_t op, uint8_t arg)
{
    enum { SH_NONE, SH_SL, SH_SR, SH_ASR, SH_ASL, SH_ROL, SH_ROR,
           SH_SLOSB, SH_SROSB } kind = SH_NONE;
    int count = 0;
    unsigned steps;
    unsigned dr = arg >> 4;
    uint32_t value = cpu->gpr[dr] & MASK16;
    uint32_t incoming_shift_buffer;
    uint32_t outgoing_shift_buffer = 0;
    uint32_t carry = 0;
    bool overflow = false;
    bool mem = false;
    uint32_t addr = 0, next = 0;

    switch (op) {
    case 0x0c: kind = SH_SL; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); break;
    case 0x1c: kind = SH_SLOSB; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); break;
    case 0x0d: kind = SH_SR; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); break;
    case 0x1d: kind = SH_SROSB; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); break;
    case 0x4a: kind = SH_ASR; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); break;
    case 0xc8: kind = SH_ROL; count = dmcu_reg_read(cpu, arg, 8) & 15; break;
    case 0xc9: kind = SH_ROR; count = dmcu_reg_read(cpu, arg, 8) & 15; break;
    case 0x4b: kind = SH_ASR; count = arg & 15; break;
    case 0x4c: kind = SH_SL; count = arg & 15; break;
    case 0x5c: kind = SH_SLOSB; count = arg & 15; break;
    case 0x4d: kind = SH_SR; count = arg & 15; break;
    case 0x5d: kind = SH_SROSB; count = arg & 15; break;
    case 0xca: kind = SH_ROL; count = arg & 15; break;
    case 0xcb: kind = SH_ROR; count = arg & 15; break;
    case 0x2c: kind = SH_SL; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); mem = true; break;
    case 0x2d: kind = SH_SR; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); mem = true; break;
    case 0x3c: kind = SH_SLOSB; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); mem = true; break;
    case 0x3d: kind = SH_SROSB; count = dmcu_sext8(dmcu_reg_read(cpu, arg, 8)); mem = true; break;
    default: return false;
    }
    if (mem) {
        addr = cpu->gpr[dr];
        next = dmcu_pointer_postincrement(cpu, dr, addr, 2);
        value = dmcu_pointer_read(bus, cpu, dr, addr, 16);
    }
    if (count < 0) {
        count = -count;
        if (kind == SH_SL) { kind = SH_SR; }
        else if (kind == SH_SR) { kind = SH_SL; }
        else if (kind == SH_SLOSB) { kind = SH_SROSB; }
        else if (kind == SH_SROSB) { kind = SH_SLOSB; }
        else if (kind == SH_ASR) { kind = SH_ASL; }
    }
    steps = count;
    incoming_shift_buffer = cpu->shift_buffer & MASK16;
    /* 与 QEMU 相同的常数时间 16 位移位流；ASL 饱和在循环内补做。 */
    if (kind == SH_SL || kind == SH_SLOSB) {
        uint64_t shifted = (uint64_t)value << (steps < 32u ? steps : 32u);

        carry = (shifted >> 16) & 1u;
        overflow = (shifted >> 16) != 0;
        if (kind == SH_SL) {
            outgoing_shift_buffer = (shifted >> 16) & MASK16;
        }
        value = shifted & MASK16;
        steps = 0;
    } else if (kind == SH_SR || kind == SH_SROSB || kind == SH_ASR) {
        uint32_t original = value;
        uint64_t shifted = ((uint64_t)value << 16) >> (steps < 32u ? steps : 32u);

        carry = (shifted >> 15) & 1u;
        overflow = steps > 16 ? original != 0 :
                   steps > 1 && (original & ((1u << (steps - 1)) - 1u));
        if (kind == SH_ASR) {
            int32_t signed_value = dmcu_sext16(original);

            value = (uint32_t)(signed_value >> (steps < 16u ? steps : 16u)) & MASK16;
            outgoing_shift_buffer = steps <= 16 ? (uint32_t)(shifted & MASK16) :
                (uint32_t)(signed_value >> ((steps - 16) < 16u ? steps - 16 : 16u)) & MASK16;
            if (steps > 16) {
                carry = original >> 15;
            }
        } else {
            value = (uint32_t)(shifted >> 16);
            if (kind == SH_SR) {
                outgoing_shift_buffer = (uint32_t)(shifted & MASK16);
            }
        }
        steps = 0;
    } else if (kind == SH_ROL || kind == SH_ROR) {
        if (steps) {
            value = kind == SH_ROL ?
                ((value << steps) | (value >> (16 - steps))) & MASK16 :
                ((value >> steps) | (value << (16 - steps))) & MASK16;
            carry = kind == SH_ROL ? value & 1u : value >> 15;
        }
        steps = 0;
    }
    while (steps--) {
        uint32_t out;

        if (kind == SH_SL || kind == SH_SLOSB) {
            out = value >> 15;
            if (kind == SH_SL) {
                outgoing_shift_buffer =
                    ((outgoing_shift_buffer << 1) | out) & MASK16;
            }
            value = (value << 1) & MASK16;
            overflow |= out;
        } else if (kind == SH_SR || kind == SH_SROSB || kind == SH_ASR) {
            out = value & 1;
            if (kind == SH_SR || kind == SH_ASR) {
                outgoing_shift_buffer =
                    (outgoing_shift_buffer >> 1) | (out << 15);
            }
            /* 右移时 V 是 C 之前被丢弃位的 OR。 */
            overflow |= carry;
            if (kind == SH_ASR) {
                value = (value >> 1) | (value & 0x8000);
            } else {
                value >>= 1;
            }
        } else if (kind == SH_ASL) {
            uint32_t sign = value & 0x8000;

            out = (value >> 14) & 1;
            if (!!out != !!sign) {
                overflow = true;
            }
            value = sign | ((value << 1) & 0x7fff);
        } else if (kind == SH_ROL) {
            out = value >> 15;
            value = ((value << 1) | out) & MASK16;
        } else {
            out = value & 1;
            value = (value >> 1) | (out << 15);
        }
        carry = out;
    }
    if (kind == SH_SLOSB || kind == SH_SROSB) {
        /* OSB 指令即使移位数为 0 也消费先前的 SB。 */
        value |= incoming_shift_buffer;
    } else if (kind == SH_SL || kind == SH_SR || kind == SH_ASR ||
               kind == SH_ASL) {
        /* ASR 以负数计数转派 ASL 前先初始化 SB（CN1916884A pp.57-58）。
         * ASL 不收集 SB，但也不能让前一条指令的缓冲泄漏给后续 OSB。 */
        cpu->shift_buffer = outgoing_shift_buffer;
    }
    if (kind == SH_ASL && overflow) {
        value = value & 0x8000 ? 0x8000 : 0x7fff;
    }
    dmcu_set_flag(cpu, DMCU_PSW_C, count ? carry != 0 : false);
    dmcu_set_flag(cpu, DMCU_PSW_Z, value == 0);
    dmcu_set_flag(cpu, DMCU_PSW_N, value & 0x8000);
    dmcu_set_flag(cpu, DMCU_PSW_V,
                  kind == SH_ROL || kind == SH_ROR ? false : overflow);
    if (mem) {
        dmcu_pointer_write(bus, cpu, dr, addr, value, 16);
        cpu->gpr[dr] = next;
    } else {
        cpu->gpr[dr] = value;
    }
    return true;
}

/* ---- exec_bit（helper.c:7403-7454 原样移植） ---- */

/* CN1916884A pp.71/73/76/78：BAND/BOR/BXOR/BCMP 描述两个原始输入位，
 * 而非存储结果。两个 1 位相等时 C=V=1、Z=N=0。 */
static void bit_binary_flags(DMCUCpu *cpu, uint32_t left, uint32_t right)
{
    dmcu_set_flag(cpu, DMCU_PSW_Z, !(left | right));
    dmcu_set_flag(cpu, DMCU_PSW_C, left & right);
    dmcu_set_flag(cpu, DMCU_PSW_N, left ^ right);
    dmcu_set_flag(cpu, DMCU_PSW_V, left | right);
}

static bool exec_bit(Np560Bus *bus, DMCUCpu *cpu, uint8_t op, uint8_t arg,
                     uint16_t ext)
{
    uint32_t za, zb, qa, qb, left, right, result;

    if (op >= 0xe0) {
        result = dmcu_bit_read(bus, cpu, arg, op & 15);
        dmcu_logic_flags(cpu, result, 1);
        dmcu_bit_write(bus, cpu, arg, op & 15, op < 0xf0);
        return true;
    }
    switch (op) {
    case 0x6c: case 0x6d: case 0x7c: case 0x7d: case 0x8c: case 0x8d:
        za = ext >> 8;
        zb = ext & 15;
        qa = arg;
        qb = (ext >> 4) & 15;
        left = dmcu_bit_read(bus, cpu, za, zb);
        right = dmcu_bit_read(bus, cpu, qa, qb);
        if (op == 0x8d) {
            bit_binary_flags(cpu, left, right);
            return true;
        }
        if (op == 0x6c) { result = left | right; }
        else if (op == 0x6d) { result = left ^ right; }
        else if (op == 0x7c) { result = right; }
        else if (op == 0x7d) { result = !right; }
        else { result = left & right; }
        if (op == 0x7c || op == 0x7d) {
            dmcu_logic_flags(cpu, result, 1);
        } else {
            bit_binary_flags(cpu, left, right);
        }
        /* 对 PSR 的位写（如 BMOV C,R1.15）覆盖本指令的普通标志副作用。 */
        dmcu_bit_write(bus, cpu, za, zb, result != 0);
        return true;
    default:
        return false;
    }
}

/* ---- exec_control（helper.c:18346 语义核心，剥离全部观察器） ---- */

static bool exec_control(Np560Bus *bus, DMCUCpu *cpu, uint8_t op, uint8_t arg,
                         uint16_t ext, uint32_t start, uint32_t next,
                         uint16_t word0)
{
    uint32_t target;

    /* 条件近跳转（helper.c:18351）。0xBC 额外压入近返回地址。 */
    if ((op == 0xac || op == 0xbc) && (arg >> 4) == 0) {
        if (dmcu_condition(cpu, arg & 15)) {
            if (op == 0xbc) {
                near_return_push(bus, cpu, next);
            }
            cpu->pc = dmcu_near_pc(start, ext);
        }
        return true;
    }
    /* 远跳转 SJMP / 远调用 SCALL（helper.c:18369 + 19312 服务窗口）。 */
    if (op == 0xad || op == 0xbd) {
        target = dmcu_code_address(arg, ext);
        if (op == 0xbd) {
            bool execute_resident_rom =
                cpu->resident_rom_base != cpu->resident_rom_limit &&
                target >= cpu->resident_rom_base &&
                target < cpu->resident_rom_limit;
            bool execute_auxiliary =
                cpu->auxiliary_code_base != cpu->auxiliary_code_limit &&
                target >= cpu->auxiliary_code_base &&
                target < cpu->auxiliary_code_limit;

            /* 覆盖服务（np560_overlay_service）对任意目标优先。 */
            if (cpu->hle_rom_enabled && bus->hle_service &&
                bus->hle_service(bus->hle_user, cpu, target, start, next,
                                 true)) {
                return true;
            }
            /* mask ROM 服务窗口（helper.c:19326；03:0030 建外层 C 帧）。 */
            if (cpu->hle_rom_enabled && target < DMCU_PROGRAM_BASE &&
                !execute_resident_rom && !execute_auxiliary) {
                if (!bus->hle_service ||
                    !bus->hle_service(bus->hle_user, cpu, target, start,
                                      next, false)) {
                    dmcu_unsupported(bus, cpu, start, word0);
                }
                return true;
            }
            far_return_push(bus, cpu, next);
        }
        cpu->pc = target & MASK24;
        return true;
    }
    /* 经寄存器的条件跳转（helper.c:19407）。 */
    if (op == 0xcc || op == 0xce) {
        if (dmcu_condition(cpu, arg >> 4)) {
            if (op == 0xcc) {
                near_return_push(bus, cpu, next);
            }
            cpu->pc = dmcu_near_pc(
                start, dmcu_data_read(bus, cpu, cpu->gpr[arg & 15], 16));
        }
        return true;
    }
    /* 近调用（helper.c:19416）。 */
    if (op == 0xcd) {
        near_return_push(bus, cpu, next);
        cpu->pc = dmcu_near_pc(start,
                               (dmcu_code_pc(next) + dmcu_sext8(arg) * 2) & MASK16);
        return true;
    }
    /* 条件近跳转（helper.c:19422）。 */
    if (op >= 0xd0 && op <= 0xde) {
        if (dmcu_condition(cpu, op & 15)) {
            cpu->pc = dmcu_near_pc(start,
                                   (dmcu_code_pc(next) + dmcu_sext8(arg) * 2) & MASK16);
        }
        return true;
    }
    /* 位测试条件跳转（helper.c:19429）。 */
    if (op >= 0x9a && op <= 0x9d) {
        unsigned bitno = (ext >> 8) & 15;
        bool value = dmcu_bit_read(bus, cpu, arg, bitno) != 0;
        bool take = (op == 0x9a || op == 0x9c) ? value : !value;

        if (take) {
            if (op == 0x9c) { dmcu_bit_write(bus, cpu, arg, bitno, false); }
            if (op == 0x9d) { dmcu_bit_write(bus, cpu, arg, bitno, true); }
            cpu->pc = dmcu_near_pc(start,
                                   (dmcu_code_pc(next) + dmcu_sext8(ext) * 2) & MASK16);
        }
        return true;
    }
    /* 返回族（helper.c:19441）。 */
    if (op == 0xc6) {
        if (arg == 0x00) {
            cpu->pc = near_return_pop(bus, cpu);
        } else if (arg == 0x20) {
            cpu->pc = far_return_pop(bus, cpu);
        } else if (arg == 0x10) {
            /* RETI（helper.c:19879）：弹 PSW、CS（SOCR.NSEG 时保持
             * 当前段）、PC。帧位置在弹栈前捕获（helper.c:19882）。
             * guest 可用 PUSH REG[01] 或字面 PSR 字自构帧——该字必须
             * 按硬件 PSR 位序解释，而非内部 TCG 标志。 */
            uint32_t cs;
            uint32_t frame_sp = cpu->gpr[DMCU_SP] & MASK16;

            dmcu_sfr_write(cpu, DMCU_SFR_PSW, dmcu_pop(bus, cpu), 16);
            cs = (dmcu_sfr_read(cpu, DMCU_SFR_SOCR, 16) & DMCU_SOCR_NSEG)
                 ? dmcu_code_segment(start) : dmcu_pop(bus, cpu);
            cpu->pc = dmcu_code_address(cs, dmcu_pop(bus, cpu));
            /* 匹配硬件帧才结算：恢复前台重复影子并结束该层
             * （helper.c:19912-19982）。 */
            dmcu_irq_reti_retire(bus, cpu, frame_sp);
        } else {
            return false;
        }
        return true;
    }
    return false;
}

/* ---- 主循环补充指令（helper.c 22976-23102） ---- */

/* MUL/UMUL（helper.c:22982）：32 位积在 R6:R7；R5 不是扩展寄存器。 */
static bool exec_mul(DMCUCpu *cpu, uint8_t op, uint8_t arg)
{
    int64_t product;
    bool overflow = false;
    uint32_t left = cpu->gpr[arg >> 4] & MASK16;
    uint32_t right = cpu->gpr[arg & 15] & MASK16;

    if (op == 0xc0) {
        /* 有符号 MUL：SOCR.FR 小数模式积左移一位（CN1916884A p32）。 */
        product = (int64_t)dmcu_sext16(left) * dmcu_sext16(right);
        if (dmcu_sfr_read(cpu, DMCU_SFR_SOCR, 16) & DMCU_SOCR_FR) {
            product *= 2;
        }
        overflow = product < INT32_MIN || product > INT32_MAX;
    } else {
        product = (int64_t)left * right;
    }
    cpu->gpr[6] = (uint32_t)(product >> 16) & MASK16;
    cpu->gpr[7] = (uint32_t)product & MASK16;
    dmcu_set_flag(cpu, DMCU_PSW_Z, (uint32_t)product == 0);
    dmcu_set_flag(cpu, DMCU_PSW_N, (uint32_t)product & 0x80000000u);
    /* MUL 报告双字溢出；UMUL 明确清 V。 */
    dmcu_set_flag(cpu, DMCU_PSW_V, overflow);
    return true;
}

/* ---- exec_compare_update（helper.c:20006 原样移植） ---- */

/* CMPD/CMPI：寄存器或 SFR 与立即数/内存比较后自增/自减。QEMU 的
 * dmcu_sfr_has_timing IOThread 锁是宿主并发关注点，解释器单线程不需要。 */
static bool exec_compare_update(Np560Bus *bus, DMCUCpu *cpu, uint8_t op,
                                uint8_t arg, uint16_t ext)
{
    int delta;
    uint32_t left, right;

    switch (op) {
    case 0x4e: delta = -1; break;
    case 0x5e: delta = -2; break;
    case 0x4f: delta = 1; break;
    case 0x5f: delta = 2; break;
    case 0x6e: case 0x8e: delta = -1; break;
    case 0x7e: case 0x9e: delta = -2; break;
    case 0x6f: case 0x8f: delta = 1; break;
    case 0x7f: case 0x9f: delta = 2; break;
    default: return false;
    }

    if (op < 0x60) {
        left = dmcu_reg_read(cpu, arg >> 4, 16);
        right = arg & 15;
        dmcu_alu_op(cpu, 2, left, right, 16); /* alu_sub：只设标志 */
        dmcu_reg_write(cpu, arg >> 4, left + (uint32_t)delta, 16);
    } else {
        left = dmcu_sfr_read(cpu, arg, 16);
        right = op < 0x80 ? ext : dmcu_data_read(bus, cpu, ext, 16);
        dmcu_alu_op(cpu, 2, left, right, 16);
        dmcu_sfr_write(cpu, arg, left + (uint32_t)delta, 16);
    }
    return true;
}

/* ---- exec_mac（helper.c:20046 原样移植） ---- */

/* CN1916884A pp.40-48：kind 01=MAC 10=RNDST 11=MULS；AR 为 R5 低字节 +
 * R6:R7，R5 高字节是独立 SE 寄存器（fig.5），不是累加器符号填充。 */
static bool exec_mac(Np560Bus *bus, DMCUCpu *cpu, uint8_t op, uint8_t arg,
                     uint16_t ext)
{
    const uint64_t mask40 = (UINT64_C(1) << 40) - 1;
    const int64_t min40 = -(INT64_C(1) << 39);
    const int64_t max40 = (INT64_C(1) << 39) - 1;
    unsigned kind = (arg >> 4) & 3;
    unsigned signs = (arg >> 2) & 3;
    uint64_t raw, operand, rounded;
    int64_t acc, product = 0, result;
    bool carry = false, overflow, saturated = false;
    bool subtract = arg & 1;
    uint32_t socr;
    uint32_t x = 0, y = 0, xaddr = 0, yaddr = 0;
    uint32_t xnext = 0, ynext = 0, move_addr = 0;
    unsigned xr = 0, yr = 0, xmode = 0, ymode = 0;
    int32_t xstep = 0, ystep = 0;

    if (op != 0xbe || (ext & 0xf0f0)) {
        return false;
    }
    if ((kind == 1 && (arg & 2)) ||
        (kind >= 2 && (arg & 3)) || (kind == 2 && ext)) {
        return false;
    }
    if (kind == 0) {
        xr = 8 + ((ext >> 8) & 3);
        yr = 12 + (ext & 3);
        xmode = (ext >> 10) & 3;
        ymode = (ext >> 2) & 3;
        /* 仅 XM0、XM2 和 YM2 有原厂寄存器映射证据；不猜测其余修饰符
         * 或 XC/YC 寄存器号（helper.c:20076-20077）。 */
        if ((xr != 8 && xr != 10) || yr != 14 ||
            xmode == 3 || ymode == 3) {
            return false;
        }
        xaddr = cpu->gpr[xr] & MASK16;
        yaddr = cpu->gpr[yr] & MASK16;
        if ((xaddr & 1) || (yaddr & 1) ||
            yaddr < 0x8000 || yaddr >= 0xc000 ||
            !dmcu_pointer_circular_valid(cpu, xr, xaddr) ||
            !dmcu_pointer_circular_valid(cpu, yr, yaddr)) {
            /* 无效/未知访问在任何总线副作用或 .m 效果之前停止；
             * 架构 EXR 故障交付待实现（helper.c:20088-20090）。 */
            return false;
        }
        xstep = xmode == 1 ? 2 : xmode == 2 ? -2 : 0;
        ystep = ymode == 1 ? 2 : ymode == 2 ? -2 : 0;
        /* 任何总线副作用之前捕获 AGU 结果；.m 可以以修饰符自身的
         * SFR 地址为目标（helper.c:20094-20095）。 */
        xnext = dmcu_pointer_postincrement(cpu, xr, xaddr, xstep);
        ynext = dmcu_pointer_postincrement(cpu, yr, yaddr, ystep);
        move_addr = dmcu_pointer_postincrement(cpu, xr, xaddr, -xstep);
    }

    raw = ((uint64_t)(cpu->gpr[5] & 0xff) << 32) |
          ((uint64_t)(cpu->gpr[6] & MASK16) << 16) |
          (cpu->gpr[7] & MASK16);
    acc = raw & (UINT64_C(1) << 39) ? (int64_t)raw - (INT64_C(1) << 40)
                                  : (int64_t)raw;
    socr = dmcu_sfr_read(cpu, DMCU_SFR_SOCR, 16);
    if (kind != 2) {
        if (kind == 0) {
            /* fig.9：X 可用 DS 选中的总线，Y 是内部 Y RAM。读两个操作数
             * 先于 .m 可能覆盖别名值（helper.c:20112-20113）。 */
            x = dmcu_pointer_read(bus, cpu, xr, xaddr, 16);
            y = dmcu_internal_data_read(bus, yaddr, 16);
            if ((arg & 2) && xstep) {
                dmcu_pointer_write(bus, cpu, xr, move_addr, x, 16);
            }
        } else {
            x = dmcu_reg_read(cpu, ext >> 8, 16);
            y = dmcu_reg_read(cpu, ext, 16);
        }
        {
            int64_t left = signs & 2 ? (int64_t)x : (int64_t)dmcu_sext16(x);
            int64_t right = signs & 1 ? (int64_t)y : (int64_t)dmcu_sext16(y);

            product = left * right;
            if (!signs && (socr & DMCU_SOCR_FR)) {
                product *= 2;
            }
        }
    }
    if (kind == 3) {
        result = product;
    } else {
        operand = (uint64_t)product & mask40;
        carry = subtract ? raw < operand : raw + operand > mask40;
        result = subtract ? acc - product : acc + product;
    }
    overflow = result < min40 || result > max40;
    if (arg & 0x80) {
        rounded = ((uint64_t)result & mask40) + 0x8000;
        carry |= rounded > mask40;
        result += 0x8000;
        overflow |= result < min40 || result > max40;
        result &= ~INT64_C(0xffff);
    }
    if ((arg & 0x40) || (socr & DMCU_SOCR_MAS)) {
        if (result > INT32_MAX) {
            result = INT32_MAX;
            saturated = true;
        } else if (result < INT32_MIN) {
            result = INT32_MIN;
            saturated = true;
        }
    }
    raw = (uint64_t)result & mask40;
    dmcu_reg_write(cpu, 5, (cpu->gpr[5] & 0xff00) | (raw >> 32), 16);
    cpu->gpr[6] = (uint32_t)(raw >> 16) & MASK16;
    cpu->gpr[7] = (uint32_t)raw & MASK16;
    dmcu_set_flag(cpu, DMCU_PSW_Z, raw == 0);
    dmcu_set_flag(cpu, DMCU_PSW_N, raw & (UINT64_C(1) << 39));
    dmcu_set_flag(cpu, DMCU_PSW_C, carry);
    dmcu_set_flag(cpu, DMCU_PSW_V, overflow);
    /* 保留 PSR 的优先级、CIRQ、MX 及无关位（helper.c:20163）。 */
    cpu->sfr[DMCU_SFR_PSW] &= ~(DMCU_PSR_MV | DMCU_PSR_MS);
    cpu->sfr[DMCU_SFR_PSW] |= (overflow ? DMCU_PSR_MV : 0) |
                              (saturated ? DMCU_PSR_MS : 0);
    if (kind == 0) {
        cpu->gpr[xr] = xnext;
        cpu->gpr[yr] = ynext;
    }
    return true;
}

/* ---- exec_divide（helper.c:20174 原样移植） ---- */

static bool exec_divide(DMCUCpu *cpu, uint8_t op, uint8_t arg)
{
    uint32_t divisor = dmcu_reg_read(cpu, arg, 16);
    int64_t squot = 0, srem = 0;
    uint64_t uquot = 0, urem = 0;
    bool overflow = false;
    bool is_signed;

    if (op < 0xc2 || op > 0xc5 || (arg >> 4) != 0) {
        return false;
    }
    is_signed = op == 0xc2 || op == 0xc4;
    if (divisor == 0) {
        overflow = true;
    } else if (is_signed) {
        int64_t dividend = op == 0xc2 ? (int64_t)dmcu_sext16(cpu->gpr[7])
            : (int32_t)(((cpu->gpr[6] & MASK16) << 16) | (cpu->gpr[7] & MASK16));
        int64_t signed_divisor = dmcu_sext16(divisor);

        squot = dividend / signed_divisor;
        srem = dividend % signed_divisor;
        overflow = squot < INT16_MIN || squot > INT16_MAX;
    } else {
        uint64_t dividend = op == 0xc3 ? cpu->gpr[7] & MASK16
            : (((cpu->gpr[6] & MASK16) << 16) | (cpu->gpr[7] & MASK16));

        uquot = dividend / divisor;
        urem = dividend % divisor;
        overflow = uquot > UINT16_MAX;
    }

    dmcu_set_flag(cpu, DMCU_PSW_C, false);
    dmcu_set_flag(cpu, DMCU_PSW_V, overflow);
    if (!overflow) {
        uint32_t quotient = is_signed ? (uint32_t)squot : (uint32_t)uquot;

        cpu->gpr[7] = quotient & MASK16;
        cpu->gpr[6] = (is_signed ? (uint32_t)srem : (uint32_t)urem) & MASK16;
        dmcu_set_flag(cpu, DMCU_PSW_Z, (quotient & MASK16) == 0);
        dmcu_set_flag(cpu, DMCU_PSW_N, quotient & 0x8000);
    }
    return true;
}

/* ---- exec_exponent（helper.c:20215 原样移植） ---- */

static bool exec_exponent(Np560Bus *bus, DMCUCpu *cpu, uint8_t op, uint8_t arg)
{
    unsigned kind, count = 0;
    uint32_t value, temp;
    bool indirect, block, signed_value;

    if (op != 0x1e || (arg >> 4) > 7) {
        return false;
    }
    kind = arg >> 4;
    indirect = kind & 1;
    block = kind >= 4;
    signed_value = kind < 2 || (kind >= 4 && kind < 6);
    if (indirect) {
        uint32_t addr = cpu->gpr[arg & 15];
        uint32_t next = dmcu_pointer_postincrement(cpu, arg & 15, addr, 2);

        value = dmcu_pointer_read(bus, cpu, arg & 15, addr, 16);
        cpu->gpr[arg & 15] = next;
    } else {
        value = dmcu_reg_read(cpu, arg, 16);
    }

    temp = value;
    if (signed_value) {
        unsigned sign = temp >> 15;

        while (count < 16 && (temp >> 15) == sign) {
            temp = (temp << 1) & MASK16;
            count++;
        }
        count--;
    } else {
        while (count < 16 && !(temp & 0x8000)) {
            temp = (temp << 1) & MASK16;
            count++;
        }
    }
    /* 归一化计数写 RB10（SE 架构别名）；块形式只在更小时写。 */
    if (!block || dmcu_reg_read(cpu, 10, 8) > count) {
        dmcu_reg_write(cpu, 10, count, 8);
    }
    dmcu_set_flag(cpu, DMCU_PSW_Z, value == 0);
    dmcu_set_flag(cpu, DMCU_PSW_C, false);
    dmcu_set_flag(cpu, DMCU_PSW_N, false);
    dmcu_set_flag(cpu, DMCU_PSW_V, false);
    return true;
}

/* ---- 主入口：取指 + 分发 + 完成收尾 ---- */

IRAM_ATTR uint32_t dmcu_exec_step(Np560Bus *bus, DMCUCpu *cpu)
{
    uint32_t start = cpu->pc;
    uint16_t word0 = dmcu_code_read16(bus, start);
    uint16_t ext = 0;
    uint32_t size = dmcu_insn_size(word0);
    uint32_t next = dmcu_code_advance(start, size);
    uint8_t op = word0 >> 8;
    uint8_t arg = word0 & 0xff;
    unsigned lo = op & 15;
    bool handled = false;
    uint16_t old_ds0 = cpu->sfr[DMCU_SFR_DS0] & 0x1ff;
    uint16_t old_ds1 = cpu->sfr[DMCU_SFR_DS1] & 0x1ff;
    bool was_ignoring_ds = cpu->ignore_ds_count != 0;
    bool was_critical = cpu->critical_count != 0;
    bool address_space_changed;

    if (size == 4) {
        /* FF:FFFE 页边界的扩展字运行时取回（translate.c wrapped_extension）。 */
        ext = dmcu_code_read16(bus, dmcu_code_advance(start, 2));
    }

    /* 提交顺序后继；分支族内部改写 pc。 */
    cpu->pc = next;

    /* —— 分发（helper.c exec_decoded 20278 + 主循环补充 22976-23102） —— */

    if (op == 0xc7 && arg == 0) {
        /* NOP。 */
        handled = true;
    } else if (word0 == 0x3fff) {
        /* RESET：在系统边界处理（板级先复位设备再继续）。
         * 挂起期间不执行顺序后继。 */
        cpu->reset_requested = true;
        cpu->halted = true;
        return DMCU_EXEC_EXIT;
    } else if (op < 0xc0 && lo <= 7) {
        handled = exec_alu(bus, cpu, op, arg, ext);
    } else if (op >= 0xe0) {
        handled = exec_bit(bus, cpu, op, arg, ext);
    } else if (op >= 0xd0 && op <= 0xde) {
        handled = exec_control(bus, cpu, op, arg, ext, start, next, word0);
    } else if (op == 0x08 || op == 0x09 || op == 0x0a || op == 0x0b ||
               op == 0x18 || op == 0x19 || op == 0x1a || op == 0x1b ||
               op == 0x28 || op == 0x29 || op == 0x2a || op == 0x2b ||
               op == 0x38 || op == 0x39 || op == 0x3a || op == 0x3b ||
               op == 0x48 || op == 0x49 || op == 0x58 || op == 0x59 ||
               op == 0x5a || op == 0x5b ||
               op == 0x68 || op == 0x69 || op == 0x6a || op == 0x6b ||
               op == 0x78 || op == 0x79 ||
               op == 0x88 || op == 0x89 || op == 0x8a || op == 0x8b ||
               op == 0x98 || op == 0x99 ||
               op == 0xa8 || op == 0xa9 || op == 0xb8 || op == 0xb9) {
        handled = exec_move(bus, cpu, op, arg, ext);
    } else if (op == 0x0c || op == 0x0d || op == 0x1c || op == 0x1d ||
               op == 0x2c || op == 0x2d || op == 0x3c || op == 0x3d ||
               op == 0x4a || op == 0x4b || op == 0x4c || op == 0x4d ||
               op == 0x5c || op == 0x5d || op == 0xc8 || op == 0xc9 ||
               op == 0xca || op == 0xcb) {
        handled = exec_shift(bus, cpu, op, arg);
    } else if (op == 0x6c || op == 0x6d || op == 0x7c || op == 0x7d ||
               op == 0x8c || op == 0x8d) {
        handled = exec_bit(bus, cpu, op, arg, ext);
    } else if (op == 0x9a || op == 0x9b || op == 0x9c || op == 0x9d ||
               op == 0xac || op == 0xad || op == 0xbc || op == 0xbd ||
               op == 0xc6 || op == 0xcc || op == 0xcd || op == 0xce) {
        handled = exec_control(bus, cpu, op, arg, ext, start, next, word0);
    } else if (op == 0x1e && (arg >> 4) <= 7) {
        /* NORMAL/NORMF 归一化族（helper.c:20215）。 */
        handled = exec_exponent(bus, cpu, op, arg);
    } else if (op == 0xbe) {
        /* memory MAC（helper.c:20046）。校验失败归为未支持。 */
        handled = exec_mac(bus, cpu, op, arg, ext);
    } else if (op >= 0xc2 && op <= 0xc5) {
        /* DIV/DIVL/UDIV/UDIVL（helper.c:20174）。 */
        handled = exec_divide(cpu, op, arg);
    } else if (op == 0x4e || op == 0x4f || op == 0x5e || op == 0x5f ||
               op == 0x6e || op == 0x6f || op == 0x7e || op == 0x7f ||
               op == 0x8e || op == 0x8f || op == 0x9e || op == 0x9f) {
        /* CMPD/CMPI 比较并更新（helper.c:20006）。 */
        handled = exec_compare_update(bus, cpu, op, arg, ext);
    } else if (op == 0x0f) {
        /* TRAP（helper.c:23096）：同步陷阱，返回点为两字节指令之后的
         * next，PC 折叠在调用者的 CS 内。 */
        dmcu_software_trap(bus, cpu,
                           dmcu_code_address(dmcu_code_segment(start),
                                             dmcu_code_pc(next)), arg);
        handled = true;
    } else if ((op == 0xc0 || op == 0xc1) && exec_mul(cpu, op, arg)) {
        handled = true;
    } else if (op == 0x1f) {
        /* PUSH/POP 直接 SFR（helper.c:23015）。 */
        if (arg & 0x80) {
            dmcu_sfr_write(cpu, arg & 0x7f, dmcu_pop(bus, cpu), 16);
        } else {
            dmcu_push(bus, cpu, dmcu_sfr_read(cpu, arg & 0x7f, 16));
        }
        handled = true;
    } else if (op == 0x2e) {
        /* LINK（helper.c:23023）。 */
        dmcu_push(bus, cpu, cpu->gpr[DMCU_FP]);
        cpu->gpr[DMCU_FP] = cpu->gpr[DMCU_SP];
        cpu->gpr[DMCU_SP] = (cpu->gpr[DMCU_SP] - arg * 2) & MASK16;
        handled = true;
    } else if (op == 0x2f && arg == 0) {
        /* UNLINK（helper.c:23029）。 */
        cpu->gpr[DMCU_SP] = cpu->gpr[DMCU_FP];
        cpu->gpr[DMCU_FP] = dmcu_pop(bus, cpu);
        handled = true;
    } else if (op == 0x0e) {
        /* EXCH（helper.c:22970）：交换两个 16 位 GPR，不改标志。
         * arg 高半=目标寄存器、低半=源寄存器。固件存储容量扫描后的
         * 2A:AA24 0EB0 (EXCH R11,R0) 会执行到，缺失会导致 CPU halt。 */
        uint16_t tmp = (uint16_t)cpu->gpr[arg >> 4];
        cpu->gpr[arg >> 4] = (uint16_t)cpu->gpr[arg & 15];
        cpu->gpr[arg & 15] = tmp;
        handled = true;
    } else if (op == 0x3e && (arg >> 4) <= 1) {
        /* NEG（helper.c:22976）。 */
        int bits = (arg >> 4) ? 8 : 16;
        uint32_t value = dmcu_alu_op(cpu, 2, 0,
                                     dmcu_reg_read(cpu, arg, bits), bits);

        dmcu_reg_write(cpu, arg, value, bits);
        handled = true;
    } else if (op == 0x3f && arg <= 3) {
        /* CRIT #imm2：其后 imm2+1 条指令禁硬件中断。 */
        cpu->critical_count = (arg & 3) + 1;
        handled = true;
    } else if (op == 0x3f && arg >= 0x10 && arg <= 0x13) {
        /* IGNDS #imm2：其后 imm2+1 条指令用固定未分组视图。 */
        cpu->ignore_ds_count = (arg & 3) + 1;
        handled = true;
    } else if (op == 0xbf && arg == 0) {
        /* 单指令重复：立即数计数（helper.c:23079）。 */
        cpu->single_repeat_pc = next;
        cpu->single_repeat_count = (uint32_t)ext + 1;
        handled = true;
    } else if (op == 0xcf && (arg >> 4) == 0) {
        /* 单指令重复：寄存器计数（helper.c:23084）。 */
        cpu->single_repeat_pc = next;
        cpu->single_repeat_count = (cpu->gpr[arg & 15] & MASK16) + 1;
        handled = true;
    } else if (op == 0xae || (op == 0xaf && (arg >> 4) == 0)) {
        /* 块重复 REPB/REP（helper.c:23089）。 */
        cpu->repeat_pc = next;
        cpu->repeat_end = dmcu_near_pc(start, ext);
        cpu->repeat_count = (op == 0xae ? arg : cpu->gpr[arg & 15]) + 1;
        cpu->repeat_block = true;
        handled = true;
    }

    if (!handled) {
        dmcu_unsupported(bus, cpu, start, word0);
        return DMCU_EXEC_EXIT;
    }

    /* —— 完成收尾（helper.c 24255-24318 语义部分） —— */

    if (was_ignoring_ds && cpu->ignore_ds_count) {
        cpu->ignore_ds_count--;
    }
    if (was_critical && cpu->critical_count) {
        cpu->critical_count--;
    }
    address_space_changed =
        old_ds0 != (cpu->sfr[DMCU_SFR_DS0] & 0x1ff) ||
        old_ds1 != (cpu->sfr[DMCU_SFR_DS1] & 0x1ff) ||
        was_ignoring_ds != (cpu->ignore_ds_count != 0);

    /* 重复引擎回绕（helper.c 24275）。架构只暴露一台单指令引擎和
     * 一台块引擎，无嵌套栈。 */
    if (cpu->single_repeat_count && start == cpu->single_repeat_pc) {
        if (--cpu->single_repeat_count) {
            cpu->pc = cpu->single_repeat_pc;
        }
    }
    if (!cpu->single_repeat_count && cpu->repeat_count) {
        if (cpu->pc == cpu->repeat_end) {
            if (--cpu->repeat_count) {
                cpu->pc = cpu->repeat_pc;
            } else {
                cpu->repeat_block = false;
            }
        }
    }

    /* CRIT 只屏蔽其后的指令窗口；窗口到期或 IRQ 状态变化时交还
     * 主循环仲裁（helper.c 24300）。 */
    if ((was_ignoring_ds && !cpu->ignore_ds_count) ||
        (was_critical && !cpu->critical_count) ||
        cpu->irq_state_changed) {
        cpu->irq_state_changed = false;
        /* 指令边界仲裁（cpu.c dmcu_cpu_exec_interrupt）：CRIT/IGNDS
         * 窗口内保持 RQ 挂起、不进入服务；DTU 搬运不受限。 */
        if (!cpu->critical_count && !cpu->ignore_ds_count) {
            dmcu_irq_service(bus, cpu);
        }
        return DMCU_EXEC_EXIT;
    }
    /* DS0/DS1 或 IGNDS 转变是精确的执行边界。 */
    if (address_space_changed) {
        return DMCU_EXEC_LOOKUP;
    }
    return cpu->pc == next ? DMCU_EXEC_NEXT : DMCU_EXEC_LOOKUP;
}
