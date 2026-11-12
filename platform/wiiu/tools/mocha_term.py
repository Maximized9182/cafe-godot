"""Interactive terminal for the shared Mocha TCP shell.

tools/mocha_logger.py owns the single connection to the Wii U (only one
program can listen on port 7965). It appends everything the console sends to
mocha.log and sends each line written to the FIFO mocha.cmd as a command.
This shows mocha.log live and gives a "> " prompt whose lines are sent as
commands, so several people (or Claude and you) can watch and send commands
at the same time.

Usage: tools/mocha_term.sh   (Ctrl+C or Ctrl+D quits; the logger keeps running)
Examples: iosu reboot, iosu shutdown, cos help, cos threads
"""
import os
import readline  # noqa: F401  (line editing and history for input())
import subprocess
import sys
import threading
import time

PROMPT = "> "
BACKLOG_LINES = 40

root = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
log_path = os.path.join(root, "mocha.log")
fifo_path = os.path.join(root, "mocha.cmd")
history_path = os.path.join(root, ".mocha_term_history")

output_lock = threading.Lock()


def hidden(line):
    # The logger notes every command it sends; the prompt already shows them.
    return line.startswith("[mocha_logger ") and "] sent: " in line


def show(line):
    """Prints a log line above the prompt, then redraws the prompt and
    whatever has been typed so far."""
    with output_lock:
        sys.stdout.write("\r\033[K" + line + "\n" + PROMPT + readline.get_line_buffer())
        sys.stdout.flush()


def follow_log():
    # Start with the last BACKLOG_LINES lines, then follow new output.
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()[-BACKLOG_LINES:]
        for line in lines:
            line = line.rstrip("\n")
            if not hidden(line):
                show(line)
        while True:
            line = f.readline()
            if not line:
                time.sleep(0.1)
                continue
            line = line.rstrip("\n")
            if not hidden(line):
                show(line)


def logger_running():
    return subprocess.run(["pgrep", "-f", "mocha_logger.py"], stdout=subprocess.DEVNULL).returncode == 0


def main():
    if not logger_running():
        print("mocha_logger.py isn't running. Start it with:")
        print(f"  python3 -I {root}/tools/mocha_logger.py {log_path} {fifo_path}")
        return 1
    if not os.path.exists(fifo_path):
        print(f"{fifo_path} is missing; restart mocha_logger.py.")
        return 1
    try:
        readline.read_history_file(history_path)
    except OSError:
        pass

    print("-- shared Mocha shell: type a command and press Enter (Ctrl+D quits) --")
    threading.Thread(target=follow_log, daemon=True).start()
    try:
        while True:
            cmd = input(PROMPT).strip()
            if not cmd:
                continue
            # Lines under PIPE_BUF (4 KB) are written atomically, so concurrent
            # senders don't interleave.
            with open(fifo_path, "w") as fifo:
                fifo.write(cmd + "\n")
    except (EOFError, KeyboardInterrupt):
        print()
    finally:
        try:
            readline.write_history_file(history_path)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
