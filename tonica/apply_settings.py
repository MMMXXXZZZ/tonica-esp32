"""Apply tonica/settings.conf to tonica/sdkconfig (run by tonica/build.ps1).

    python -I tonica/apply_settings.py [--settings tonica/settings.conf] [--sdkconfig tonica/sdkconfig]

sdkconfig.defaults only fills values missing from sdkconfig, so editing it does
nothing on an existing build; this writes the settings straight into sdkconfig.
"""
import argparse, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHOICES = {  # setting -> (value -> Kconfig member), all members listed so the others get unset
    "TAKEOVER": {"open": "TONICA_TAKEOVER_OPEN", "owner_priority": "TONICA_TAKEOVER_OWNER_PRIORITY",
                 "owner_only": "TONICA_TAKEOVER_OWNER_ONLY"},
    "AUDIO_OUT": {"pdm": "TONICA_AUDIO_OUT_PDM", "i2s_dac": "TONICA_AUDIO_OUT_I2S_DAC"},
}
STRINGS = {"DEVICE_NAME", "PRESET"}
INTS = {"BITRATE", "GRACE_SEC", "RESUME_WINDOW_MIN", "GUEST_IDLE_MIN", "GUEST_MAX_MIN", "WATCHDOG_MIN", "AUTO_UPDATE", "PDM_LEFT_GPIO", "PDM_RIGHT_GPIO",
        "PDM_CLK_GPIO", "I2S_BCK_GPIO", "I2S_WS_GPIO", "I2S_DOUT_GPIO", "I2S_MCLK_GPIO"}


def parse(path):
    out = {}
    for n, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r'([A-Z0-9_]+)\s*=\s*("([^"]*)"|[^#\s]+)', line)
        if not m:
            sys.exit(f"{path}:{n}: cannot parse: {line}")
        key, val = m.group(1), (m.group(3) if m.group(3) is not None else m.group(2))
        if key not in CHOICES and key not in STRINGS and key not in INTS:
            sys.exit(f"{path}:{n}: unknown setting {key}")
        if key in CHOICES and val not in CHOICES[key]:
            sys.exit(f"{path}:{n}: {key} must be one of {', '.join(CHOICES[key])}")
        if key in INTS and not re.fullmatch(r"-?\d+", val):
            sys.exit(f"{path}:{n}: {key} must be an integer")
        out[key] = val
    return out


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--settings", default=os.path.join(ROOT, "tonica", "settings.conf"))
    a.add_argument("--sdkconfig", default=os.path.join(ROOT, "tonica", "sdkconfig"))
    args = a.parse_args()
    settings = parse(args.settings)
    lines = {}  # CONFIG_NAME -> full line
    for key, val in settings.items():
        if key in CHOICES:
            for v, member in CHOICES[key].items():
                lines["CONFIG_" + member] = f"CONFIG_{member}=y" if v == val else f"# CONFIG_{member} is not set"
        elif key in STRINGS:
            lines[f"CONFIG_TONICA_{key}"] = f'CONFIG_TONICA_{key}="{val}"'
        else:
            lines[f"CONFIG_TONICA_{key}"] = f"CONFIG_TONICA_{key}={val}"
    text = open(args.sdkconfig, encoding="utf-8").read()
    changed = []
    for name, line in lines.items():
        pat = re.compile(r"^(# )?" + re.escape(name) + r"(=.*| is not set)$", re.M)
        cur = pat.search(text)
        if cur and cur.group(0) == line:
            continue
        text = pat.sub(line, text) if cur else text + line + "\n"
        changed.append(line)
    if changed:
        open(args.sdkconfig, "w", encoding="utf-8").write(text)
    print(f"settings.conf: {len(settings)} settings, {len(changed)} changed in sdkconfig")
    for c in changed:
        print("  " + c)


if __name__ == "__main__":
    main()
