from pathlib import Path

Import("env")


def patch_esp32_io_expander_tags() -> None:
    libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "ESP32_IO_Expander" / "src" / "port"
    if not libdeps_dir.exists():
        return

    replacements = {
        'static char *TAG = "io_expander";': 'static const char *TAG = "io_expander";',
        'static char *TAG = "ht8574";': 'static const char *TAG = "ht8574";',
        'static char *TAG = "tca9554";': 'static const char *TAG = "tca9554";',
        'static char *TAG = "tca95xx_16";': 'static const char *TAG = "tca95xx_16";',
    }

    for path in libdeps_dir.glob("*.c"):
        content = path.read_text(encoding="utf-8")
        updated = content
        for source, target in replacements.items():
            updated = updated.replace(source, target)
        if updated != content:
            path.write_text(updated, encoding="utf-8")


patch_esp32_io_expander_tags()
