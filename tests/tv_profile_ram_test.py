#!/usr/bin/env python3
"""Keep Android TV's shipped and generated profiles at the documented RAM baseline."""
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / "ios/GTVEmu/Resources/default-manifest.json").read_text())
profile = next(p for p in manifest["profiles"] if p["id"] == "android-tv")
assert profile["ramMB"] == 2048, f"default Android TV profile RAM is {profile['ramMB']} MB"

assets = (root / "tools/mkios-assets.sh").read_text()
block = assets.split('"id": "android-tv"', 1)[1].split('"files": [', 1)[0]
match = re.search(r'"ramMB"\s*:\s*(\d+)', block)
assert match and int(match.group(1)) == 2048, "generated Android TV asset profile must use 2048 MB"

settings = (root / "ios/GTVEmu/Views/SettingsView.swift").read_text()
assert "Android TV يحتاج عمليًا 2048 ميغابايت وJIT" in settings, "settings help must state Android TV requirements"
print("نجح: ملف Android TV الافتراضي والمولّد يضبطان 2048 ميغابايت، والإعدادات تذكر الحاجة إلى JIT")
