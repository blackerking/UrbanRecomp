#!/usr/bin/env python3
"""Sampling profiler for the host on Windows -- no Visual Studio needed.

Starts the executable, samples its main thread's instruction pointer about a
thousand times a second (SuspendThread / GetThreadContext), and prints where
the time went, per function, resolved through the PDB with DbgHelp. Self time
only: a sample counts for the function the thread was executing.

    python tools/sample_profile.py build-prof/Release/UrbanRecomp.exe --qualify 1800

The executable needs a PDB next to it (compile /Zi, link /DEBUG). A plain
Release build has none; configure a separate directory for this:

    cmake -S . -B build-prof -DCMAKE_TOOLCHAIN_FILE=... \\
        "-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi" \\
        "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF"

Options before the executable: --interval <ms> (default 1), --top <n>
(default 40), --lines (group by source line instead of function).
"""
import collections
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
winmm = ctypes.WinDLL("winmm")

TH32CS_SNAPTHREAD = 0x4
TH32CS_SNAPMODULE = 0x8
THREAD_ALL = 0x0002 | 0x0008 | 0x0010 | 0x0040   # suspend/resume, get context, query
CONTEXT_CONTROL = 0x00100001
CONTEXT_SIZE = 0x4D0
RIP_OFFSET = 0xF8
MEM_COMMIT_RESERVE = 0x3000
PAGE_READWRITE = 0x04


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("GlblcntUsage", wt.DWORD),
                ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE),
                ("szModule", wt.WCHAR * 256), ("szExePath", wt.WCHAR * 260)]


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG),
                ("Reserved", ctypes.c_uint64 * 2), ("Index", wt.ULONG),
                ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64),
                ("Flags", wt.ULONG), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG),
                ("MaxNameLen", wt.ULONG), ("Name", wt.WCHAR * 512)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p),
                ("LineNumber", wt.DWORD), ("FileName", wt.LPWSTR),
                ("Address", ctypes.c_uint64)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.VirtualAlloc.restype = ctypes.c_void_p
k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
dbghelp.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR,
                                     ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64,
                                 ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
dbghelp.SymGetLineFromAddrW64.argtypes = [wt.HANDLE, ctypes.c_uint64,
                                          ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINEW64)]


def first_thread(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ok = k32.Thread32First(snap, ctypes.byref(te))
    tid = None
    while ok:
        if te.th32OwnerProcessID == pid:
            tid = te.th32ThreadID
            break
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return tid


def modules(pid):
    """[(base, size, path)] of the process, the executable first."""
    out = []
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
    if not snap or snap == wt.HANDLE(-1).value:
        return out
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(me)
    ok = k32.Module32FirstW(snap, ctypes.byref(me))
    while ok:
        out.append((me.modBaseAddr, me.modBaseSize, me.szExePath))
        ok = k32.Module32NextW(snap, ctypes.byref(me))
    k32.CloseHandle(snap)
    return out


def main_module(pid):
    for _ in range(200):
        mods = modules(pid)
        if mods:
            return mods[0][0], mods[0][1]
        time.sleep(0.005)
    sys.exit("cannot read the module list of %d" % pid)


def main():
    args = sys.argv[1:]
    interval, top, by_line = 1.0, 40, False
    while args and args[0].startswith("--"):
        opt = args.pop(0)
        if opt == "--interval":
            interval = float(args.pop(0))
        elif opt == "--top":
            top = int(args.pop(0))
        elif opt == "--lines":
            by_line = True
        else:
            sys.exit("unknown option " + opt)
    if not args:
        sys.exit(__doc__)
    exe = os.path.abspath(args[0])

    winmm.timeBeginPeriod(1)
    proc = subprocess.Popen([exe] + args[1:], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    tid = None
    while tid is None and proc.poll() is None:
        tid = first_thread(proc.pid)
    base, size = main_module(proc.pid)
    mods = {}
    th = k32.OpenThread(THREAD_ALL, False, tid)
    ctx = k32.VirtualAlloc(None, 0x1000, MEM_COMMIT_RESERVE, PAGE_READWRITE)
    ctypes.c_uint32.from_address(ctx + 0x30).value = CONTEXT_CONTROL
    rip = ctypes.c_uint64.from_address(ctx + RIP_OFFSET)

    hits = collections.Counter()
    total = 0
    t0 = time.perf_counter()
    while proc.poll() is None:
        if k32.SuspendThread(th) != 0xFFFFFFFF:
            if k32.GetThreadContext(th, ctx):
                hits[rip.value] += 1
                total += 1
                if total % 500 == 1:
                    for m in modules(proc.pid):
                        mods[m[0]] = m
            k32.ResumeThread(th)
        time.sleep(interval / 1000.0)
    wall = time.perf_counter() - t0
    winmm.timeEndPeriod(1)

    sym = wt.HANDLE(0x5C5C)
    dbghelp.SymSetOptions(0x2 | 0x10)   # undecorated names, load lines
    dbghelp.SymInitializeW(sym, os.path.dirname(exe), False)
    dbghelp.SymLoadModuleExW(sym, None, exe, None, base, size, None, 0)
    others = sorted(m for m in mods.values() if m[0] != base)
    for mb, ms, mp in others:
        dbghelp.SymLoadModuleExW(sym, None, mp, None, mb, ms, None, 0)

    def module_of(addr):
        for mb, ms, mp in others:
            if mb <= addr < mb + ms:
                return os.path.basename(mp)
        return None

    info = SYMBOL_INFOW()
    line = IMAGEHLP_LINEW64()
    disp64, disp32 = ctypes.c_uint64(), wt.DWORD()
    groups = collections.Counter()
    outside = 0
    for addr, n in hits.items():
        info.SizeOfStruct = 88          # sizeof(SYMBOL_INFOW) without the name
        info.MaxNameLen = 511
        if base <= addr < base + size:
            name = "?%x" % (addr - base)
        else:
            outside += n
            name = "%s!?" % (module_of(addr) or "?")
        if dbghelp.SymFromAddrW(sym, addr, ctypes.byref(disp64), ctypes.byref(info)):
            name = info.Name if base <= addr < base + size else                 "%s!%s" % (module_of(addr), info.Name)
        if by_line:
            line.SizeOfStruct = ctypes.sizeof(line)
            if dbghelp.SymGetLineFromAddrW64(sym, addr, ctypes.byref(disp32), ctypes.byref(line)):
                name = "%s  %s:%d" % (name, os.path.basename(line.FileName), line.LineNumber)
        groups[name] += n
    dbghelp.SymCleanup(sym)

    print("%d samples over %.1f s; %d outside the executable (DLLs)"
          % (total, wall, outside))
    for name, n in groups.most_common(top):
        print("%6.2f%%  %6d  %s" % (100.0 * n / max(total, 1), n, name))


if __name__ == "__main__":
    main()
