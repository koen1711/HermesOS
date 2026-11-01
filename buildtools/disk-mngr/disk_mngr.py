#!/usr/bin/env python3
import curses
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import image
from image import DiskConfig, ImageError, format_size, parse_size

EXIT_ABORTED = 130
MODES = ("continue", "keep", "fresh")


@dataclass(frozen=True)
class Choice:
    mode: str
    size: Optional[int] = None


class Aborted(Exception):
    pass


def settings_path(config: DiskConfig) -> Path:
    return config.binary_dir / "disk-mngr.json"


def load_last_size(config: DiskConfig) -> int:
    try:
        return int(json.loads(settings_path(config).read_text())["size"])
    except (OSError, ValueError, KeyError, TypeError):
        return config.default_size


def save_last_size(config: DiskConfig, size: int):
    settings_path(config).write_text(json.dumps({"size": size}) + "\n")


def image_summary(config: DiskConfig) -> Optional[str]:
    try:
        stat = config.core_img.stat()
    except FileNotFoundError:
        return None
    modified = time.strftime("%Y-%m-%d %H:%M", time.localtime(stat.st_mtime))
    used = format_size(stat.st_blocks * 512)
    return f"core.img: {format_size(stat.st_size)} ({used} on host), last modified {modified}"


def draw(screen, title, lines, footer):
    screen.erase()
    height, width = screen.getmaxyx()
    rows = [(title, curses.A_BOLD), ("", 0)] + lines + [("", 0), (footer, curses.A_DIM)]
    for row, (text, attr) in enumerate(rows[:height]):
        try:
            screen.addnstr(row, 2, text, max(width - 3, 0), attr)
        except curses.error:
            pass
    screen.refresh()


def prompt_size(screen, config: DiskConfig, title, initial: int) -> Optional[int]:
    text = format_size(initial)
    error = ""
    while True:
        lines = [(f"Size: {text}_", 0), (error, curses.A_BOLD)]
        draw(screen, title, lines, "Enter accept   Backspace edit   Esc cancel   e.g. 512M, 32G, 1T")
        key = screen.get_wch()
        if key == "\x1b":
            return None
        if key in ("\n", "\r", curses.KEY_ENTER):
            try:
                return parse_size(text, config)
            except ImageError as err:
                error = str(err)
        elif key in ("\x7f", "\b", curses.KEY_BACKSPACE):
            text = text[:-1]
        elif isinstance(key, str) and key.isalnum():
            text += key


def confirm(screen, title, message) -> bool:
    draw(screen, title, [(message, 0)], "y confirm   any other key cancel")
    return screen.get_wch() in ("y", "Y")


def menu(screen, config: DiskConfig, last_size: int) -> Choice:
    curses.curs_set(0)
    curses.use_default_colors()
    screen.keypad(True)
    curses.set_escdelay(25)

    summary = image_summary(config)
    if summary:
        entries = [
            ("continue", "Continue", "keep the disk, refresh build files"),
            ("keep", "Continue without refresh", "boot the image exactly as it is"),
            ("resize", "Resize", "change the size, keep the files, refresh build files"),
            ("fresh", "Create fresh", "replace the image with a new one"),
            ("quit", "Quit", "exit without booting"),
        ]
        current = config.core_img.stat().st_size
    else:
        entries = [
            ("fresh", "Create fresh", "build a new image"),
            ("quit", "Quit", "exit without booting"),
        ]
        current = None

    selected = 0
    title = "HermesOS disk manager"
    while True:
        lines = [(summary or "No disk image yet", 0), ("", 0)]
        for index, (_, label, detail) in enumerate(entries):
            marker = "> " if index == selected else "  "
            attr = curses.A_REVERSE if index == selected else 0
            lines.append((f"{marker}{label:<26}{detail}", attr))
        draw(screen, title, lines, "Up/Down or j/k move   Enter select   q quit")

        key = screen.get_wch()
        if key in (curses.KEY_UP, "k"):
            selected = (selected - 1) % len(entries)
        elif key in (curses.KEY_DOWN, "j"):
            selected = (selected + 1) % len(entries)
        elif key == "q":
            raise Aborted
        elif key in ("\n", "\r", curses.KEY_ENTER):
            action = entries[selected][0]
            if action == "quit":
                raise Aborted
            if action in ("continue", "keep"):
                return Choice(action)
            if action == "resize":
                size = prompt_size(screen, config, "Resize image", current)
                if size is not None:
                    return Choice("continue", size)
            if action == "fresh":
                size = prompt_size(screen, config, "Create fresh image", last_size)
                if size is None:
                    continue
                if not summary or confirm(
                    screen, "Create fresh image",
                    f"Replace core.img with an empty {format_size(size)} image? Its files will be lost.",
                ):
                    return Choice("fresh", size)


def from_environment(config: DiskConfig, interactive: bool) -> Optional[Choice]:
    mode = os.environ.get("DISK_MODE")
    size_text = os.environ.get("DISK_SIZE")
    size = parse_size(size_text, config) if size_text else None
    if mode and mode not in MODES:
        raise ImageError(f"DISK_MODE must be one of {', '.join(MODES)}, got '{mode}'")
    if mode:
        return Choice(mode, size)
    if interactive:
        return None
    default = "continue" if config.core_img.exists() else "fresh"
    print(f"disk-mngr: no terminal and no DISK_MODE, using '{default}'", flush=True)
    return Choice(default, size)


def apply(config: DiskConfig, choice: Choice, last_size: int):
    exists = config.core_img.exists()
    if choice.mode == "keep":
        if not exists:
            raise ImageError("DISK_MODE=keep needs an existing core.img")
        if choice.size is not None and choice.size != config.core_img.stat().st_size:
            raise ImageError("DISK_SIZE can't change the size with DISK_MODE=keep; use continue")
        print("Booting existing image unchanged.", flush=True)
        return
    if choice.mode == "fresh" or not exists:
        size = choice.size if choice.size is not None else last_size
        image.create(config, size)
        save_last_size(config, size)
        return
    if choice.size is not None and choice.size != config.core_img.stat().st_size:
        image.resize(config, choice.size)
        save_last_size(config, choice.size)
    image.refresh(config)


def main(argv) -> int:
    if len(argv) != 2:
        print(f"Usage: {argv[0]} <BINARY_DIR>", file=sys.stderr)
        return 2
    config = DiskConfig(binary_dir=Path(argv[1]).resolve())
    try:
        interactive = sys.stdin.isatty() and sys.stdout.isatty()
        choice = from_environment(config, interactive)
        last_size = load_last_size(config)
        if choice is None:
            try:
                choice = curses.wrapper(menu, config, last_size)
            except curses.error as error:
                print(f"disk-mngr: can't start the menu ({error})", flush=True)
                choice = from_environment(config, False)
        apply(config, choice, last_size)
    except (Aborted, KeyboardInterrupt):
        print("Aborted, not booting.", file=sys.stderr)
        return EXIT_ABORTED
    except ImageError as error:
        print(f"disk-mngr: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        print(f"disk-mngr: {error.cmd[0]} failed with exit code {error.returncode}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"disk-mngr: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
