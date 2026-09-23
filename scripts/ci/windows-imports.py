"""Reject Windows binaries that require unbundled compiler runtime DLLs."""
from pathlib import Path
import re
import subprocess
import sys

SYSTEM_DLLS = {"kernel32.dll", "ntdll.dll", "advapi32.dll", "user32.dll", "shell32.dll",
               "ws2_32.dll", "bcrypt.dll", "ole32.dll", "ucrtbase.dll", "msvcrt.dll"}
for tool in ("rev", "rem", "rvm"):
    path = Path(sys.argv[1]) / (tool + ".exe")
    result = subprocess.run(["llvm-readobj", "--coff-imports", str(path)], check=True,
                            capture_output=True, text=True)
    imports = re.findall(r"Name: (\S+\.dll)", result.stdout, re.IGNORECASE)
    if not imports:
        raise RuntimeError(f"无法检查导入依赖：{path}")
    for name in imports:
        name = name.lower()
        if name not in SYSTEM_DLLS and not name.startswith("api-ms-win-"):
            raise RuntimeError(f"安装包缺少非系统依赖：{path}: {name}")
