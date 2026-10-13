"""Sample where a running 32-bit game's threads are (for hangs).

usage: hang_sample.py <pid> [samples] [stack_dwords]
Each sample briefly suspends every thread, reads its WOW64 context and the
top of its stack, and resumes it. Prints CPU time per thread, EIP and the
return-address candidates (module + offset). Read-only.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

TH32CS_SNAPTHREAD = 0x4
PROCESS_ALL_READ = 0x0410 | 0x0008  # QUERY_INFORMATION | VM_READ | VM_OPERATION
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040  # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]


class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [("ContextFlags", wt.DWORD), ("Dr", wt.DWORD * 6),
                ("FloatSave", ctypes.c_byte * 112), ("SegGs", wt.DWORD), ("SegFs", wt.DWORD),
                ("SegEs", wt.DWORD), ("SegDs", wt.DWORD), ("Edi", wt.DWORD), ("Esi", wt.DWORD),
                ("Ebx", wt.DWORD), ("Edx", wt.DWORD), ("Ecx", wt.DWORD), ("Eax", wt.DWORD),
                ("Ebp", wt.DWORD), ("Eip", wt.DWORD), ("SegCs", wt.DWORD), ("EFlags", wt.DWORD),
                ("Esp", wt.DWORD), ("SegSs", wt.DWORD), ("ExtendedRegisters", ctypes.c_byte * 512)]


def modules(process):
    handles = (wt.HMODULE * 1024)()
    needed = wt.DWORD()
    # LIST_MODULES_32BIT = 1
    psapi.EnumProcessModulesEx(process, handles, ctypes.sizeof(handles), ctypes.byref(needed), 1)
    out = []
    for h in handles[:needed.value // ctypes.sizeof(wt.HMODULE)]:
        if not h:
            continue
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(process, wt.HMODULE(h), name, 260)

        class MODINFO(ctypes.Structure):
            _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]
        info = MODINFO()
        psapi.GetModuleInformation(process, wt.HMODULE(h), ctypes.byref(info), ctypes.sizeof(info))
        out.append((info.base or 0, info.size, name.value))
    return out


def where(mods, addr):
    for base, size, name in mods:
        if base <= addr < base + size:
            return "%s+0x%X" % (name, addr - base)
    return None


def read(process, addr, n):
    buf = (ctypes.c_ubyte * n)()
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(process, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return b""
    return bytes(buf[:got.value])


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            out.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return out


def cpu_ms(thread):
    c, e, kt, ut = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
    k32.GetThreadTimes(thread, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    return ((ut.dwHighDateTime << 32 | ut.dwLowDateTime) +
            (kt.dwHighDateTime << 32 | kt.dwLowDateTime)) / 10000.0


def main():
    pid = int(sys.argv[1])
    samples = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 256
    process = k32.OpenProcess(PROCESS_ALL_READ, False, pid)
    mods = modules(process)
    for base, size, name in mods:
        if name.lower() in ("cnc3game.dat", "cnc3ep1.dat", "dinput8.dll"):
            print("module %s base 0x%X size 0x%X" % (name, base, size))
    before = {}
    for s in range(samples):
        print("=== sample %d" % s)
        for tid in threads(pid):
            th = k32.OpenThread(THREAD_ACCESS, False, tid)
            if not th:
                continue
            ms = cpu_ms(th)
            delta = ms - before.get(tid, ms)
            before[tid] = ms
            ctx = WOW64_CONTEXT()
            ctx.ContextFlags = 0x10000 | 0x1 | 0x2  # CONTROL | INTEGER
            k32.SuspendThread(th)
            ok = k32.Wow64GetThreadContext(th, ctypes.byref(ctx))
            stack = read(process, ctx.Esp, depth * 4) if ok else b""
            k32.ResumeThread(th)
            k32.CloseHandle(th)
            if not ok:
                continue
            loc = where(mods, ctx.Eip) or "0x%X" % ctx.Eip
            print("tid %d cpu %.0f ms (+%.0f) eip %s esp %08X ebp %08X eax %08X ebx %08X ecx %08X "
                  "edx %08X esi %08X edi %08X" % (tid, ms, delta, loc, ctx.Esp, ctx.Ebp, ctx.Eax,
                                                  ctx.Ebx, ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi))
            if s == samples - 1:
                rets = []
                for i in range(0, len(stack) - 3, 4):
                    v = int.from_bytes(stack[i:i + 4], "little")
                    w = where(mods, v)
                    if w and (w.lower().startswith("cnc3") or w.lower().startswith("dinput8")):
                        rets.append("[esp+0x%X] %s" % (i, w))
                print("   " + "\n   ".join(rets[:40]))
        time.sleep(0.5)


if __name__ == "__main__":
    main()
