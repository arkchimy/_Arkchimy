# 덤프(.dmp) 요약 - 디버거(cdb/WinDbg) 없이 Windows 기본 dbghelp.dll로 읽는다
# 사용: python tools\dump_info.py <덤프 경로>
#  1. 예외 코드·주소·스레드, 우리 모듈(exe, NetworkLib.dll)과 링크 시각
#  2. 예외 주소의 함수·소스 줄 - 덤프에 적힌 경로의 exe·pdb가 그때 빌드 그대로 남아 있어야 한다(링크 시각을 비교한다)
#  3. gAssertMessage(마지막 Assert 설명) - Assert로 멈췄으면 설명이 보인다
#  4. 예외 스레드 스택에서 라이브러리 문자열을 가리키는 값 + 뒤 4바이트 - Start 실패의 NetError{Step, Code}를 찾는다
# 전체 메모리 덤프(MiniDumpWithFullMemory)를 가정한다 - 스레드 스택도 Memory64 목록에서 읽는다
import ctypes
import datetime
import os
import struct
import sys
from ctypes import wintypes

sys.stdout.reconfigure(encoding="utf-8")
data = open(sys.argv[1], "rb").read()
_, _, stream_cnt, dir_rva, _, dump_ts, _ = struct.unpack_from("<IIIIIIQ", data, 0)


def find_stream(stream_type):
    for i in range(stream_cnt):
        t, size, rva = struct.unpack_from("<III", data, dir_rva + i * 12)
        if t == stream_type:
            return size, rva
    return None


def read_wstr(rva):
    n = struct.unpack_from("<I", data, rva)[0]
    return data[rva + 4: rva + 4 + n].decode("utf-16-le")


ranges = []
if find_stream(9):  # Memory64ListStream
    _, rva = find_stream(9)
    cnt, cur = struct.unpack_from("<QQ", data, rva)
    for i in range(cnt):
        start, size = struct.unpack_from("<QQ", data, rva + 16 + i * 16)
        ranges.append((start, size, cur))
        cur += size


def read_mem(addr, n):
    for start, size, off in ranges:
        if start <= addr and addr + n <= start + size:
            return data[off + addr - start: off + addr - start + n]
    return None


print(f"덤프 시각 {datetime.datetime.fromtimestamp(dump_ts)}")
modules = []
_, rva = find_stream(4)  # ModuleListStream
for i in range(struct.unpack_from("<I", data, rva)[0]):
    base, size, _, link_ts, name_rva = struct.unpack_from("<QIIII", data, rva + 4 + i * 108)
    modules.append((base, size, read_wstr(name_rva), link_ts))
ours = [m for m in modules if m[2].lower().endswith(".exe") or os.path.basename(m[2]).lower() == "networklib.dll"]
for base, size, name, link_ts in ours:
    now = ""
    if os.path.exists(name):
        same = abs(os.path.getmtime(name) - link_ts) < 5
        now = "지금 파일과 같은 빌드" if same else "지금 파일은 다른 빌드 - 심볼이 맞지 않는다"
    print(f"  {os.path.basename(name):18s} base {base:#x} 링크 {datetime.datetime.fromtimestamp(link_ts)}  {now}")

_, rva = find_stream(6)  # ExceptionStream
thread_id = struct.unpack_from("<I", data, rva)[0]
code, _, _, addr = struct.unpack_from("<IIQQ", data, rva + 8)
hit = next((m for m in modules if m[0] <= addr < m[0] + m[1]), None)
print(f"예외 코드 {code:#010x}{' (__debugbreak - Assert 또는 일부러 멈춤)' if code == 0x80000003 else ''}, 스레드 {thread_id}")
print(f"  주소 {addr:#x} = {os.path.basename(hit[2]) if hit else '?'} + {addr - hit[0]:#x}" if hit else f"  주소 {addr:#x}")

# dbghelp로 함수·줄, gAssertMessage
dbghelp = ctypes.WinDLL("dbghelp.dll")
proc = wintypes.HANDLE(0x1234)  # 실제 프로세스가 아니어도 되는 임의 값
dbghelp.SymSetOptions(0x2 | 0x10)  # UNDNAME | LOAD_LINES
dbghelp.SymInitializeW(proc, None, False)
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
for base, size, name, _ in ours:
    if os.path.exists(name):
        dbghelp.SymLoadModuleExW(proc, None, name, None, ctypes.c_uint64(base), size, None, 0)


class SymbolInfo(ctypes.Structure):
    _fields_ = [("SizeOfStruct", ctypes.c_ulong), ("TypeIndex", ctypes.c_ulong), ("Reserved", ctypes.c_uint64 * 2), ("Index", ctypes.c_ulong),
                ("Size", ctypes.c_ulong), ("ModBase", ctypes.c_uint64), ("Flags", ctypes.c_ulong), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", ctypes.c_ulong), ("Scope", ctypes.c_ulong), ("Tag", ctypes.c_ulong),
                ("NameLen", ctypes.c_ulong), ("MaxNameLen", ctypes.c_ulong), ("Name", ctypes.c_wchar * 512)]


class LineInfo(ctypes.Structure):
    _fields_ = [("SizeOfStruct", ctypes.c_ulong), ("Key", ctypes.c_void_p), ("LineNumber", ctypes.c_ulong), ("FileName", ctypes.c_wchar_p),
                ("Address", ctypes.c_uint64)]


def new_symbol():
    s = SymbolInfo()
    s.SizeOfStruct = ctypes.sizeof(SymbolInfo) - ctypes.sizeof(ctypes.c_wchar * 512) + 2
    s.MaxNameLen = 511
    return s


sym, disp = new_symbol(), ctypes.c_uint64(0)
if dbghelp.SymFromAddrW(proc, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(sym)):
    print(f"  함수 {sym.Name} + {disp.value:#x}")
line, d32 = LineInfo(), ctypes.c_ulong(0)
line.SizeOfStruct = ctypes.sizeof(LineInfo)
if dbghelp.SymGetLineFromAddrW64(proc, ctypes.c_uint64(addr), ctypes.byref(d32), ctypes.byref(line)):
    print(f"  소스 {line.FileName}:{line.LineNumber}")
assert_sym = new_symbol()
if dbghelp.SymFromNameW(proc, "gAssertMessage", ctypes.byref(assert_sym)):
    raw = read_mem(assert_sym.Address, 8)
    ptr = struct.unpack("<Q", raw)[0] if raw else 0
    text = (read_mem(ptr, 300) or b"").split(b"\0")[0].decode("utf-8", "replace") if ptr else ""
    print(f"gAssertMessage: {text if text else '(비어 있음 - Assert로 멈춘 것이 아니다)'}")

# 예외 스레드 스택에서 라이브러리 안의 문자열을 가리키는 값(NetError.Step 후보)
lib = next((m for m in modules if os.path.basename(m[2]).lower() == "networklib.dll"), None)
_, rva = find_stream(3)  # ThreadListStream
for i in range(struct.unpack_from("<I", data, rva)[0]):
    tid, _, _, _, _, stack_start, stack_size, stack_rva = struct.unpack_from("<IIIIQQII", data, rva + 4 + i * 48)
    if tid != thread_id or lib is None:
        continue
    stack = read_mem(stack_start, stack_size) or data[stack_rva: stack_rva + stack_size]
    seen = set()
    for j in range(0, len(stack) - 12, 8):
        val = struct.unpack_from("<Q", stack, j)[0]
        if not (lib[0] <= val < lib[0] + lib[1]):
            continue
        raw = (read_mem(val, 64) or b"").split(b"\0")[0]
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        nxt = struct.unpack_from("<i", stack, j + 8)[0]
        if 2 <= len(text) <= 40 and text.isprintable() and (text, nxt) not in seen:
            seen.add((text, nxt))
            print(f"스택의 라이브러리 문자열 \"{text}\" 다음 4바이트 {nxt}  (NetError면 Step·Code)")
