#!/usr/bin/env python3
"""按 profile 组合功能包并构建。

当前只切换功能包 Kconfig；板型等既有配置从根目录 sdkconfig 复制，避免
profile 构建意外改变板级配置。构建目录默认 build/profiles/<name>，可用
环境变量 XIAOZHI_BUILD_ROOT 指到更快的磁盘（机械盘上构建极慢）。
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILES_DIR = ROOT / "profiles"
FEATURES_DIR = ROOT / "main" / "features"


def load_features():
    features = {}
    for manifest in sorted(FEATURES_DIR.glob("*/feature.json")):
        data = json.loads(manifest.read_text(encoding="utf-8"))
        features[data["id"]] = data
    return features


def load_profiles():
    profiles = {}
    for path in sorted(PROFILES_DIR.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        profiles[data["name"]] = data
    return profiles


def set_kconfig(text, symbol, enabled):
    value = f"{symbol}=y" if enabled else f"# {symbol} is not set"
    pattern = re.compile(rf"^{re.escape(symbol)}=.*$|^# {re.escape(symbol)} is not set$", re.M)
    if pattern.search(text):
        return pattern.sub(value, text, count=1)
    return text.rstrip() + "\n" + value + "\n"


# 所有 profile 都需要的定制项：板级 config.json 的 sdkconfig_append 只对
# scripts/build.py 生效，feature_build.py 复制的是根 sdkconfig，这里补齐。
ALWAYS_APPEND = {
    # 聆听状态下也保持唤醒词检测：否则一轮说完进入聆听后叫不醒，
    # 必须等超时回到待机，体感就是"要喊好几次"。
    "CONFIG_WAKE_WORD_DETECTION_IN_LISTENING": True,
    # 空闲省电：开启动态电源管理后 power_save_timer 的 esp_pm_configure
    # 才能真正降频（240 -> 40MHz）。不启用 light sleep，保连接、保唤醒词。
    "CONFIG_PM_ENABLE": True,
    "CONFIG_FREERTOS_USE_TICKLESS_IDLE": True,
    # 日志默认级别 WARN -> INFO。WARN 下所有 ESP_LOGI 在编译期被裁掉，
    # 排查省电/SD/Wi-Fi 只能靠屏幕，串口一片空白。
    # 注意：CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT=y 时最大级别跟随默认值，
    # 所以只改 choice 项即可，无需单独写 CONFIG_LOG_DEFAULT_LEVEL=3。
    "CONFIG_LOG_DEFAULT_LEVEL_WARN": False,
    "CONFIG_LOG_DEFAULT_LEVEL_INFO": True,
}


def prepare_config(profile, features, sdkconfig, output):
    missing = [name for name in profile["features"] if name not in features]
    if missing:
        raise SystemExit(f"未知功能包: {', '.join(missing)}")

    text = sdkconfig.read_text(encoding="utf-8")
    for name, enabled in profile["features"].items():
        text = set_kconfig(text, features[name]["config"], enabled)
    for symbol, enabled in ALWAYS_APPEND.items():
        text = set_kconfig(text, symbol, enabled)
    for symbol, enabled in profile.get("sdkconfig", {}).items():
        text = set_kconfig(text, symbol, enabled)

    # 这里不能拿 text 和磁盘上的 sdkconfig 逐字比对。
    # cmake 配置时 Kconfig 会把 sdkconfig 规范化：补 "# default:" 注释、
    # 重排、丢掉当前不可见的项，所以磁盘版本永远和"重新生成"的差几十行。
    # 若按内容比对就会每次都重写 -> mtime 变 -> reconfigure -> 整仓重编。
    # 因此改为记录"我们想要的目标配置"的指纹：目标没变就完全不碰文件。
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    stamp = output.parent / ".sdkconfig.stamp"
    if output.is_file() and stamp.is_file():
        try:
            if stamp.read_text(encoding="utf-8").strip() == digest:
                return False
        except OSError:
            pass

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    stamp.write_text(digest + "\n", encoding="utf-8")
    return True


def build_root():
    """构建根目录：环境变量 > Windows 用户级注册表 > 仓库内 build/profiles。

    读注册表是因为 `setx` / [Environment]::SetEnvironmentVariable 只对**之后**
    启动的进程生效，已经在跑的终端读不到，容易误落到机械盘的 build 目录。
    """
    value = os.environ.get("XIAOZHI_BUILD_ROOT")
    if value:
        return Path(value)
    if os.name == "nt":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as key:
                value, _ = winreg.QueryValueEx(key, "XIAOZHI_BUILD_ROOT")
            if value:
                return Path(os.path.expandvars(value))
        except (OSError, ImportError):
            pass
    return ROOT / "build" / "profiles"


def default_jobs():
    """Windows 上并发一高就容易僵死（gcc 进程 CPU 归零、不写文件），
    实测 -j6 稳定、-j8 及以上偶发；其他平台按逻辑核数走。"""
    cpu = os.cpu_count() or 4
    if os.name == "nt":
        return min(6, cpu)
    return cpu


def kill_tree(pid):
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        try:
            os.killpg(os.getpgid(pid), signal.SIGKILL)
        except OSError:
            pass


def ninja_progress(build_dir):
    """用 .ninja_log 的体积当进度信号：每完成一个目标 ninja 就追加一行，
    比递归扫几万个 .obj 便宜得多。"""
    try:
        return (build_dir / ".ninja_log").stat().st_size
    except OSError:
        return -1


def run_once(cmd, env, log_path, build_dir, stall_timeout, interval):
    """跑一次构建。

    输出直接写文件而不是交给父进程管道：ninja 的日志量很大（近两千个目标），
    一旦消费者跟不上形成背压，就会把 ninja -> ccache -> gcc 整条链堵死，
    表现为所有编译进程 CPU 归零、目录不再有新文件。stdin 也置空，
    避免 gcc 有从标准输入读取的路径时挂住。

    返回 (退出码, 是否判定卡死)。
    """
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with open(log_path, "wb") as log:
        proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT)
        last_size = ninja_progress(build_dir)
        last_change = time.time()
        while True:
            try:
                return proc.wait(timeout=interval), False
            except subprocess.TimeoutExpired:
                size = ninja_progress(build_dir)
                if size != last_size:
                    last_size = size
                    last_change = time.time()
                    print(f"  ... 构建中（.ninja_log {size / 1024:.0f} KB）", flush=True)
                elif time.time() - last_change > stall_timeout:
                    print(f"  !! {stall_timeout}s 无任何进展，判定卡死，终止后重试", flush=True)
                    kill_tree(proc.pid)
                    try:
                        proc.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        pass
                    return None, True


def clean_ninja_locks(build_dir):
    for lock in build_dir.rglob(".ninja_lock"):
        try:
            lock.unlink()
            print(f"  清理残留锁: {lock}")
        except OSError:
            pass


def build_with_retry(cmd, env, log_path, build_dir, jobs, stall_timeout, retries):
    """卡死就杀进程、清锁、用 ninja 直接续跑（已完成的目标不会重编）。"""
    current = cmd
    for attempt in range(retries + 1):
        if attempt:
            clean_ninja_locks(build_dir)
            ninja = shutil.which("ninja")
            if ninja and (build_dir / "build.ninja").is_file():
                # 配置阶段已完成，直连 ninja 省掉 idf.py 的检查开销
                current = [ninja] + (["-j", str(jobs)] if jobs else [])
            print(f"  第 {attempt} 次重试：{' '.join(current)}")
        code, stalled = run_once(current, env, log_path, build_dir, stall_timeout, 20)
        if not stalled:
            if code != 0:
                clean_ninja_locks(build_dir)
            return code
    print("  连续卡死，放弃。请检查是否有杀毒软件/同步盘在扫描 build 目录。")
    return 1


def print_tail(log_path, lines=12):
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return
    tail = [line for line in text if line.strip()][-lines:]
    if tail:
        print("--- 日志尾部 ---")
        for line in tail:
            print("  " + line.rstrip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="english", help="profiles/<name>.json")
    parser.add_argument("--build-dir", help="构建目录，默认 $XIAOZHI_BUILD_ROOT/<profile> 或 build/profiles/<profile>")
    parser.add_argument("--sdkconfig", default=str(ROOT / "sdkconfig"), help="基础 sdkconfig")
    parser.add_argument("--jobs", type=int, help="并行编译任务数，默认 Windows 6、其他平台按核数")
    parser.add_argument("--log", help="构建日志路径，默认 <build-dir>/build.log")
    parser.add_argument("--stall-timeout", type=int, default=300,
                        help="多少秒无进展判定卡死并重试（默认 300）")
    parser.add_argument("--retries", type=int, default=2, help="卡死后重试次数（默认 2）")
    parser.add_argument("--no-build", action="store_true", help="只生成 profile 配置")
    parser.add_argument("--list", action="store_true", help="列出 profile 和功能包")
    args = parser.parse_args()

    features = load_features()
    profiles = load_profiles()

    if args.list:
        print("工作区:", ROOT)
        print("功能包:")
        for name, data in features.items():
            print(f"  {name:24} {data['config']:36} {data.get('description', '')}")
        print("profiles:")
        for name, data in profiles.items():
            enabled = [key for key, value in data["features"].items() if value]
            print(f"  {name:24} {', '.join(enabled) or '(none)'}  {data.get('description', '')}")
        return 0

    profile = profiles.get(args.profile)
    if profile is None:
        raise SystemExit(f"未知 profile: {args.profile}（可用: {', '.join(profiles)}）")

    sdkconfig = Path(args.sdkconfig)
    if not sdkconfig.is_file():
        raise SystemExit(f"找不到基础配置: {sdkconfig}")

    # 构建目录优先级：--build-dir > XIAOZHI_BUILD_ROOT/<profile> > build/profiles/<profile>。
    # 仓库放在机械盘上时，把 build 指向 SSD。
    build_dir = Path(args.build_dir) if args.build_dir else build_root() / args.profile
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    profile_sdkconfig = build_dir / "sdkconfig"
    changed = prepare_config(profile, features, sdkconfig, profile_sdkconfig)
    print(f"profile: {args.profile}")
    print(f"config : {profile_sdkconfig}" + ("" if changed else "  (内容未变，跳过重写)"))
    for name, enabled in profile["features"].items():
        print(f"  {name}: {'on' if enabled else 'off'}")

    if args.no_build:
        return 0

    idf_py = shutil.which("idf.py")
    if idf_py is None:
        raise SystemExit("找不到 idf.py，请先 source ESP-IDF 的 export.sh / export.ps1")

    cmd = [idf_py, "-B", str(build_dir), "-D", f"SDKCONFIG={profile_sdkconfig}", "build"]
    print("+", " ".join(cmd))

    # idf.py 自己没有 -j，并行度靠 CMAKE_BUILD_PARALLEL_LEVEL 透传给 ninja。
    env = os.environ.copy()
    jobs = args.jobs or default_jobs()
    if jobs:
        env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(jobs)

    log_path = Path(args.log) if args.log else build_dir / "build.log"
    print(f"jobs   : {jobs if jobs else 'ninja 默认'}")
    print(f"log    : {log_path}")

    code = build_with_retry(cmd, env, log_path, build_dir, jobs,
                            args.stall_timeout, args.retries)

    print_tail(log_path)
    if code == 0:
        print("构建完成。")
    else:
        print(f"构建失败，退出码 {code}。完整日志：{log_path}")
    return code


if __name__ == "__main__":
    sys.exit(main())
