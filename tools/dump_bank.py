import sys

rom_path, bank_s, start_s, end_s = sys.argv[1:5]
bank = int(bank_s, 16)
start_addr = int(start_s, 16)
end_addr = int(end_s, 16)

with open(rom_path, 'rb') as f:
    data = f.read()

file_start = bank * 0x8000 + (start_addr - 0x8000)
file_end = bank * 0x8000 + (end_addr - 0x8000)
chunk = data[file_start:file_end]
for i in range(0, len(chunk), 16):
    row = chunk[i:i+16]
    addr = start_addr + i
    print(f'{bank:02x}:{addr:04x}: ' + ' '.join(f'{b:02x}' for b in row))
