import os
import time
import sys
import struct
import ctypes
from datetime import datetime
import subprocess  # Import subprocess for PowerShell commands

KB = 1 << 10
MB = 1 << 20
GB = 1 << 30
BUFFER_SIZE = 4096
UPDATE_INTERVAL = 1  # In seconds

class ProgramOptions:
    def __init__(self):
        self.print_drive_list = False
        self.filename_in = None
        self.filename_out = None
        self.block_size = 0
        self.count = -1
        self.status = None

class ProgramState:
    def __init__(self):
        self.in_file = None
        self.out_file = None
        self.buffer_size = 0
        self.buffer = None
        self.out_file_is_device = False
        self.started_copying = False
        self.start_time = 0
        self.num_bytes_in = 0
        self.num_bytes_out = 0
        self.num_blocks_copied = 0

def print_usage():
    sys.stderr.write("Usage: wdd if=<in_file> of=<out_file> [bs=N] [count=N] [status=progress]\n")

def get_time_usec():
    return int(time.time() * 1_000_000)

def format_size(size):
    if size >= GB:
        return f"{size / GB:.1f} GB"
    elif size >= MB:
        return f"{size / MB:.1f} MB"
    elif size >= KB:
        return f"{size / KB:.1f} KB"
    else:
        return f"{size} bytes"

def format_speed(speed):
    if speed >= GB:
        return f"{speed / GB:.1f} GB/s"
    elif speed >= MB:
        return f"{speed / MB:.1f} MB/s"
    elif speed >= KB:
        return f"{speed / KB:.1f} KB/s"
    else:
        return f"{speed:.1f} bytes/s"

def print_progress(num_bytes_copied, last_bytes_copied, start_time, last_time):
    current_time = get_time_usec()
    elapsed_time = current_time - start_time
    if elapsed_time >= 1000000:
        speed = last_bytes_copied / ((current_time - last_time) / 1000000)
    else:
        speed = last_bytes_copied

    bytes_str = format_size(num_bytes_copied)
    speed_str = format_speed(speed)

    sys.stdout.write(f"{num_bytes_copied} bytes ({bytes_str}) copied, {elapsed_time / 1000000:.1f} s, {speed_str}\n")

def print_status(num_bytes_copied, start_time):
    print_progress(num_bytes_copied, num_bytes_copied, start_time, start_time)

def parse_size(size_str):
    if size_str.endswith('k') or size_str.endswith('K'):
        return int(size_str[:-1]) * KB
    elif size_str.endswith('m') or size_str.endswith('M'):
        return int(size_str[:-1]) * MB
    elif size_str.endswith('g') or size_str.endswith('G'):
        return int(size_str[:-1]) * GB
    else:
        return int(size_str)

def is_empty_string(s):
    return not s or s.strip() == ""

def parse_options(argv):
    options = ProgramOptions()
    for arg in argv[1:]:
        parts = arg.split("=")
        name = parts[0]
        value = parts[1] if len(parts) > 1 else None

        if name == "list":
            options.print_drive_list = True
            return options
        elif name == "if":
            options.filename_in = value
        elif name == "of":
            options.filename_out = value
        elif name == "bs":
            options.block_size = parse_size(value)
        elif name == "count":
            options.count = int(value)
        elif name == "status":
            options.status = value

    return options

def cleanup(state):
    if state.in_file:
        state.in_file.close()
    if state.out_file:
        state.out_file.close()

def exit_on_error(state, message):
    sys.stderr.write(message + "\n")
    if state.started_copying:
        print_status(state.num_bytes_out, state.start_time)
    cleanup(state)
    sys.exit(1)

def list_drives():
    try:
        # Use PowerShell to list drives
        powershell_path = r'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
        command = f'{powershell_path} Get-WmiObject -Class Win32_DiskDrive | Select-Object -Property DeviceID, Model, MediaType | Format-Table -AutoSize'
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        print(result.stdout)
    except subprocess.CalledProcessError as e:
        print(f"Error occurred while listing drives: {e}")
        sys.exit(1)

def main(argv):
    options = parse_options(argv)

    if options.print_drive_list:
        # Use the PowerShell method to list drives
        list_drives()
        return

    # Proceed with the rest of the logic if no "list" argument
    if not options.filename_in or not options.filename_out:
        print_usage()
        sys.exit(1)

    state = ProgramState()
    state.start_time = get_time_usec()

    try:
        state.in_file = open(options.filename_in, 'rb')
    except Exception as e:
        exit_on_error(state, f"Could not open input file {options.filename_in} for reading: {str(e)}")

    try:
        state.out_file = open(options.filename_out, 'wb')
    except Exception as e:
        exit_on_error(state, f"Could not open output file {options.filename_out} for writing: {str(e)}")

    state.buffer_size = BUFFER_SIZE
    state.buffer = bytearray(state.buffer_size)
    state.started_copying = True

    last_bytes_copied = 0
    last_time = 0

    while True:
        if options.count >= 0 and state.num_blocks_copied >= options.count:
            break

        if options.status == "progress":
            current_time = get_time_usec()
            if last_time == 0:
                last_time = current_time
            elif current_time - last_time >= UPDATE_INTERVAL * 1000000:
                sys.stdout.write("\r")
                print_progress(state.num_bytes_out, state.num_bytes_out - last_bytes_copied, state.start_time, last_time)
                last_time = current_time
                last_bytes_copied = state.num_bytes_out

        block = state.in_file.read(state.buffer_size)
        if not block:
            break

        state.num_bytes_in += len(block)
        state.out_file.write(block)
        state.num_bytes_out += len(block)
        state.num_blocks_copied += 1

    cleanup(state)
    print_status(state.num_bytes_out, state.start_time)


if __name__ == "__main__":
    main(sys.argv)
