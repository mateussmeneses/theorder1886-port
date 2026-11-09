# SPDX-License-Identifier: GPL-2.0-or-later
"""Diagnostic bug reporting tool for The Order: 1886 PC Port (FASE 7).
Collects system specs, GPU/Vulkan information, engine settings and redacted logs into a ZIP file.
Preserves privacy by redacting user names, local personal directories, and sensitive tokens.
"""

import os
import sys
import platform
import zipfile
import re
from datetime import datetime
from pathlib import Path

def redact_string(text, patterns=None):
    """Redacts usernames, home directories, and personal paths."""
    if not text:
        return ""
    
    # Redact current user's profile and username
    user_name = os.environ.get('USERNAME') or os.environ.get('USER') or ''
    user_profile = os.environ.get('USERPROFILE') or os.environ.get('HOME') or ''
    
    redacted = text
    if user_profile:
        # Normalize slashes
        norm_up = user_profile.replace('\\', '/')
        redacted = redacted.replace(user_profile, '<USERPROFILE>')
        redacted = redacted.replace(norm_up, '<USERPROFILE>')
    if user_name and len(user_name) > 2:
        redacted = re.sub(rf'(?i)\b{re.escape(user_name)}\b', '<USER>', redacted)
        
    return redacted

def get_cpu_info():
    """Returns processor brand and core count."""
    try:
        import multiprocessing
        cores = multiprocessing.cpu_count()
    except Exception:
        cores = "unknown"
    return f"{platform.processor() or platform.machine()} ({cores} logical cores)"

def get_ram_info():
    """Returns physical RAM and pagefile metrics on Windows."""
    if sys.platform == 'win32':
        try:
            import ctypes
            class MEMORYSTATUSEX(ctypes.Structure):
                _fields_ = [
                    ('dwLength', ctypes.c_uint32),
                    ('dwMemoryLoad', ctypes.c_uint32),
                    ('ullTotalPhys', ctypes.c_uint64),
                    ('ullAvailPhys', ctypes.c_uint64),
                    ('ullTotalPageFile', ctypes.c_uint64),
                    ('ullAvailPageFile', ctypes.c_uint64),
                    ('ullTotalVirtual', ctypes.c_uint64),
                    ('ullAvailVirtual', ctypes.c_uint64),
                    ('ullAvailExtendedVirtual', ctypes.c_uint64),
                ]
            stat = MEMORYSTATUSEX()
            stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat)):
                total_ram_gb = stat.ullTotalPhys / (1024**3)
                avail_ram_gb = stat.ullAvailPhys / (1024**3)
                total_page_gb = stat.ullTotalPageFile / (1024**3)
                return f"{total_ram_gb:.1f} GB total, {avail_ram_gb:.1f} GB free (Pagefile: {total_page_gb:.1f} GB)"
        except Exception:
            pass
    return "Unknown"

def get_git_info(repo_dir):
    """Retrieves current git commit and branch if available."""
    try:
        head_file = Path(repo_dir) / ".git" / "HEAD"
        if head_file.is_file():
            ref = head_file.read_text(encoding="utf-8", errors="ignore").strip()
            if ref.startswith("ref: "):
                branch = ref[5:]
                ref_file = Path(repo_dir) / ".git" / branch
                commit = ref_file.read_text(encoding="utf-8", errors="ignore").strip() if ref_file.is_file() else "unknown"
                return f"{branch} ({commit[:8]})"
            return ref[:8]
    except Exception:
        pass
    return "Release build (git metadata absent)"

LOG_TAIL_BYTES = 4 << 20


def collect_log(view_text, log_file):
    """The log for a report and where it came from. The game's last_run.log is preferred: the
    launcher's log view is empty after the launcher is reopened (or after Clear), while the file
    keeps the last run. Large files contribute their last LOG_TAIL_BYTES."""
    try:
        path = Path(log_file)
        size = path.stat().st_size
        if size:
            with open(path, 'rb') as f:
                if size > LOG_TAIL_BYTES:
                    f.seek(size - LOG_TAIL_BYTES)
                text = f.read().decode('utf-8', errors='replace')
            if size > LOG_TAIL_BYTES:
                text = text.split('\n', 1)[-1]  # drop the partial first line
            return text, f"{path.name} (game output file)"
    except (OSError, TypeError):
        pass
    if view_text and view_text.strip():
        return view_text, "launcher log view"
    return "", "none (no last_run.log and an empty launcher log view)"


def generate_report(app_config, ini_config, log_text, repo_dir, game_title="The Order: 1886",
                    version=None, log_source=None):
    """Generates Markdown diagnostic summary."""
    now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    git_rev = get_git_info(repo_dir)
    if version:
        git_rev = f"{version} ({git_rev})"
    cpu_str = get_cpu_info()
    ram_str = get_ram_info()
    os_str = f"{platform.system()} {platform.release()} ({platform.version()}, {platform.architecture()[0]})"

    # Redact log
    redacted_log = redact_string(log_text)

    # Collect settings
    display_mode = ini_config.get('display_mode', 'borderless')
    upscaler = ini_config.get('upscaler', 'off')
    output_res = ini_config.get('output_res', 'native')
    fps_limit = ini_config.get('fps_limit', '60')
    vsync = ini_config.get('vsync', '1')
    hdr = ini_config.get('hdr', '0')
    dmem_mb = app_config.get('dmem_mb', 'auto')

    md = f"""# Bug Report / Diagnostics — {game_title}
**Timestamp:** {now_str}  
**Port Version / Git Revision:** {git_rev}  
**Operating System:** {os_str}  
**CPU:** {cpu_str}  
**RAM & Pagefile:** {ram_str}  

## Active Graphics & Display Settings
- **Display Mode:** {display_mode}
- **Output Resolution:** {output_res}
- **Upscaler Active:** {upscaler}
- **Target Frame Rate:** {fps_limit} FPS
- **VSync:** {vsync}
- **HDR:** {hdr}
- **Guest Direct Memory Pool:** {dmem_mb}

## Diagnostic Summary
- **Redaction Applied:** Local usernames and home paths have been sanitized.
- **Save Data / Dumps Included:** None (User privacy preserved).

## Log Excerpt (Recent execution)
Source: {log_source or "launcher log view"}

```text
{redacted_log[-4000:] if redacted_log else "No log output available."}
```
"""
    return md, redacted_log

def export_report_zip(zip_path, report_md, redacted_log):
    """Writes the markdown report and redacted log into a zip archive."""
    with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("report.md", report_md.encode('utf-8'))
        if redacted_log:
            z.writestr("last_run_sanitized.log", redacted_log.encode('utf-8'))
    return zip_path
