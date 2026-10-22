#!/usr/bin/env python3
"""按 profile 组合功能包并构建。

当前只切换功能包 Kconfig；板型等既有配置从根目录 sdkconfig 复制，避免
profile 构建意外改变板级配置。构建目录默认在 build/profiles/<name>。
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
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


def prepare_config(profile, features, sdkconfig, output):
    missing = [name for name in profile["features"] if name not in features]
    if missing:
        raise SystemExit(f"未知功能包: {', '.join(missing)}")

    text = sdkconfig.read_text(encoding="utf-8")
    for name, enabled in profile["features"].items():
        text = set_kconfig(text, features[name]["config"], enabled)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="english", help="profiles/<name>.json")
    parser.add_argument("--build-dir", help="构建目录，默认 build/profiles/<profile>")
    parser.add_argument("--sdkconfig", default=str(ROOT / "sdkconfig"), help="基础 sdkconfig")
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

    build_dir = Path(args.build_dir or ROOT / "build" / "profiles" / args.profile)
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    profile_sdkconfig = build_dir / "sdkconfig"
    prepare_config(profile, features, sdkconfig, profile_sdkconfig)
    print(f"profile: {args.profile}")
    print(f"config : {profile_sdkconfig}")
    for name, enabled in profile["features"].items():
        print(f"  {name}: {'on' if enabled else 'off'}")

    if args.no_build:
        return 0

    idf_py = shutil.which("idf.py")
    if idf_py is None:
        raise SystemExit("找不到 idf.py，请先 source ESP-IDF 的 export.sh")

    cmd = [idf_py, "-B", str(build_dir), "-D", f"SDKCONFIG={profile_sdkconfig}", "build"]
    print("+", " ".join(cmd))
    return subprocess.call(cmd, cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
