import sys

# Minimal 65816 disassembler, assumes M=0 (16-bit A) and X=0 (16-bit X/Y)
# throughout (matches the traced m16/x16 flags for this code region).
# addr_mode -> (operand_bytes, format)
OPS = {
    0x18: ('CLC', 0), 0x38: ('SEC', 0), 0x60: ('RTS', 0), 0x6B: ('RTL', 0),
    0x40: ('RTI', 0), 0x1B: ('TCS', 0), 0x3B: ('TSC', 0), 0x5B: ('TCD', 0),
    0x7B: ('TDC', 0), 0xAB: ('PLB', 0), 0x8B: ('PHB', 0), 0x0B: ('PHD', 0),
    0x2B: ('PLD', 0), 0x08: ('PHP', 0), 0x28: ('PLP', 0), 0x48: ('PHA', 0),
    0x68: ('PLA', 0), 0x5A: ('PHY', 0), 0x7A: ('PLY', 0), 0xDA: ('PHX', 0),
    0xFA: ('PLX', 0), 0xE8: ('INX', 0), 0xC8: ('INY', 0), 0xCA: ('DEX', 0),
    0x88: ('DEY', 0), 0x1A: ('INC A', 0), 0x3A: ('DEC A', 0),
    0x0A: ('ASL A', 0), 0x4A: ('LSR A', 0), 0x2A: ('ROL A', 0), 0x6A: ('ROR A', 0),
    0xAA: ('TAX', 0), 0xA8: ('TAY', 0), 0x8A: ('TXA', 0), 0x98: ('TYA', 0),
    0x9A: ('TXS', 0), 0xBA: ('TSX', 0), 0x9B: ('TXY', 0), 0xBB: ('TYX', 0),
    0xEA: ('NOP', 0), 0xFB: ('XCE', 0), 0xEB: ('XBA', 0),

    0xC2: ('REP #${:02x}', 1), 0xE2: ('SEP #${:02x}', 1),
    0xA9: ('LDA #${:04x}', 2), 0xA2: ('LDX #${:04x}', 2), 0xA0: ('LDY #${:04x}', 2),
    0x29: ('AND #${:04x}', 2), 0x09: ('ORA #${:04x}', 2), 0x49: ('EOR #${:04x}', 2),
    0xC9: ('CMP #${:04x}', 2), 0xE0: ('CPX #${:04x}', 2), 0xC0: ('CPY #${:04x}', 2),
    0x69: ('ADC #${:04x}', 2), 0xE9: ('SBC #${:04x}', 2),

    0xA5: ('LDA ${:02x}', 1), 0x85: ('STA ${:02x}', 1), 0xA6: ('LDX ${:02x}', 1),
    0x86: ('STX ${:02x}', 1), 0xA4: ('LDY ${:02x}', 1), 0x84: ('STY ${:02x}', 1),
    0x25: ('AND ${:02x}', 1), 0x05: ('ORA ${:02x}', 1), 0x45: ('EOR ${:02x}', 1),
    0xC5: ('CMP ${:02x}', 1), 0x65: ('ADC ${:02x}', 1), 0xE5: ('SBC ${:02x}', 1),
    0x24: ('BIT ${:02x}', 1), 0x64: ('STZ ${:02x}', 1), 0x06: ('ASL ${:02x}', 1),
    0xE6: ('INC ${:02x}', 1), 0xC6: ('DEC ${:02x}', 1),

    0x95: ('STA ${:02x},X', 1), 0xB5: ('LDA ${:02x},X', 1), 0xB4: ('LDY ${:02x},X', 1),

    0xAD: ('LDA ${:04x}', 2), 0x8D: ('STA ${:04x}', 2), 0xAE: ('LDX ${:04x}', 2),
    0x8E: ('STX ${:04x}', 2), 0xAC: ('LDY ${:04x}', 2), 0x8C: ('STY ${:04x}', 2),
    0x2D: ('AND ${:04x}', 2), 0x0D: ('ORA ${:04x}', 2), 0x4D: ('EOR ${:04x}', 2),
    0xCD: ('CMP ${:04x}', 2), 0x6D: ('ADC ${:04x}', 2), 0xED: ('SBC ${:04x}', 2),
    0x2C: ('BIT ${:04x}', 2), 0x9C: ('STZ ${:04x}', 2), 0xEE: ('INC ${:04x}', 2),
    0xCE: ('DEC ${:04x}', 2), 0x0E: ('ASL ${:04x}', 2), 0x4E: ('LSR ${:04x}', 2),

    0xBD: ('LDA ${:04x},X', 2), 0x9D: ('STA ${:04x},X', 2), 0xBC: ('LDY ${:04x},X', 2),
    0x1D: ('ORA ${:04x},X', 2), 0x3D: ('AND ${:04x},X', 2), 0x5D: ('EOR ${:04x},X', 2),
    0x7D: ('ADC ${:04x},X', 2), 0xFD: ('SBC ${:04x},X', 2), 0xDD: ('CMP ${:04x},X', 2),
    0xFE: ('INC ${:04x},X', 2), 0xDE: ('DEC ${:04x},X', 2), 0x9E: ('STZ ${:04x},X', 2),

    0xB9: ('LDA ${:04x},Y', 2), 0x99: ('STA ${:04x},Y', 2), 0xBE: ('LDX ${:04x},Y', 2),

    0x4C: ('JMP ${:04x}', 2), 0x20: ('JSR ${:04x}', 2), 0x5C: ('JML ${:06x}', 3),
    0x22: ('JSL ${:06x}', 3), 0x6C: ('JMP (${:04x})', 2), 0x7C: ('JMP (${:04x},X)', 2),

    # Long (24-bit) absolute addressing -- 4-byte instructions.
    0xAF: ('LDA ${:06x}', 3), 0xBF: ('LDA ${:06x},X', 3),
    0x8F: ('STA ${:06x}', 3), 0x9F: ('STA ${:06x},X', 3),
    0xCF: ('CMP ${:06x}', 3), 0xDF: ('CMP ${:06x},X', 3),
    0xEF: ('SBC ${:06x}', 3), 0xFF: ('SBC ${:06x},X', 3),
    0x0F: ('ORA ${:06x}', 3), 0x1F: ('ORA ${:06x},X', 3),
    0x2F: ('AND ${:06x}', 3), 0x3F: ('AND ${:06x},X', 3),
    0x4F: ('EOR ${:06x}', 3), 0x5F: ('EOR ${:06x},X', 3),
    0x6F: ('ADC ${:06x}', 3), 0x7F: ('ADC ${:06x},X', 3),

    # Misc addressing modes seen in this ROM.
    0x02: ('COP #${:02x}', 1), 0x89: ('BIT #${:04x}', 2),
    0xF4: ('PEA #${:04x}', 2), 0xD4: ('PEI (${:02x})', 1), 0x62: ('PER ${:04x}', 2),
    0xA3: ('LDA ${:02x},S', 1), 0x03: ('ORA ${:02x},S', 1), 0x83: ('STA ${:02x},S', 1),
    0xB2: ('LDA (${:02x})', 1), 0x92: ('STA (${:02x})', 1),
    0xA1: ('LDA (${:02x},X)', 1), 0x81: ('STA (${:02x},X)', 1),
    0xB1: ('LDA (${:02x}),Y', 1), 0x91: ('STA (${:02x}),Y', 1),
    0xA7: ('LDA [${:02x}]', 1), 0x87: ('STA [${:02x}]', 1),
    0xB7: ('LDA [${:02x}],Y', 1), 0x97: ('STA [${:02x}],Y', 1),

    0x10: ('BPL', 'rel'), 0x30: ('BMI', 'rel'), 0x50: ('BVC', 'rel'),
    0x70: ('BVS', 'rel'), 0x90: ('BCC', 'rel'), 0xB0: ('BCS', 'rel'),
    0xD0: ('BNE', 'rel'), 0xF0: ('BEQ', 'rel'), 0x80: ('BRA', 'rel'),
    0x82: ('BRL', 'rel16'),
}

def dis(data, bank, start, end):
    i = start
    out = []
    while i < end:
        op = data[i]
        entry = OPS.get(op)
        if entry is None:
            out.append((bank, i, f'{op:02x}', '.byte $%02x' % op))
            i += 1
            continue
        mnem, size = entry
        if size == 'rel':
            rel = data[i+1]
            signed = rel if rel < 0x80 else rel - 0x100
            target = i + 2 + signed
            out.append((bank, i, f'{data[i]:02x} {data[i+1]:02x}', f'{mnem} ${target:04x}'))
            i += 2
        elif size == 'rel16':
            rel = data[i+1] | (data[i+2] << 8)
            signed = rel if rel < 0x8000 else rel - 0x10000
            target = i + 3 + signed
            out.append((bank, i, f'{data[i]:02x} {data[i+1]:02x} {data[i+2]:02x}', f'{mnem} ${target:04x}'))
            i += 3
        elif size == 0:
            out.append((bank, i, f'{data[i]:02x}', mnem))
            i += 1
        elif size == 1:
            operand = data[i+1]
            out.append((bank, i, f'{data[i]:02x} {data[i+1]:02x}', mnem.format(operand)))
            i += 2
        elif size == 2:
            operand = data[i+1] | (data[i+2] << 8)
            out.append((bank, i, f'{data[i]:02x} {data[i+1]:02x} {data[i+2]:02x}', mnem.format(operand)))
            i += 3
        elif size == 3:
            operand = data[i+1] | (data[i+2] << 8) | (data[i+3] << 16)
            out.append((bank, i, f'{data[i]:02x} {data[i+1]:02x} {data[i+2]:02x} {data[i+3]:02x}', mnem.format(operand)))
            i += 4
    return out

if __name__ == '__main__':
    rom_path = sys.argv[1]
    bank = int(sys.argv[2], 16)
    start_addr = int(sys.argv[3], 16)
    end_addr = int(sys.argv[4], 16)
    with open(rom_path, 'rb') as f:
        data = f.read()
    file_start = bank * 0x8000 + (start_addr - 0x8000)
    file_end = bank * 0x8000 + (end_addr - 0x8000)
    for b, off, hexs, text in dis(data, bank, file_start, file_end):
        addr = 0x8000 + (off - bank * 0x8000)
        print(f'{bank:02x}:{addr:04x}  {hexs:<12}  {text}')
