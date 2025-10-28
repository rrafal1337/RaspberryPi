#!/usr/bin/env python3
from luma.core.interface.serial import i2c
from luma.oled.device import ssd1306
from PIL import Image, ImageDraw, ImageFont
import psutil, time, datetime, subprocess
import argparse, sys

# -------------------------------------------------
# Font setup
# -------------------------------------------------
font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
font_big = ImageFont.truetype(font_path, 18)
font = ImageFont.truetype(font_path, 12)
font_small = ImageFont.truetype(font_path, 10)

# -------------------------------------------------
# Base Screen Class
# -------------------------------------------------
class Screen:
    def __init__(self, device):
        self.device = device
        self.image = Image.new("1", (device.width, device.height))
        self.draw = ImageDraw.Draw(self.image)

    def draw_header(self, title):
        self.draw.rectangle((0, 0, 127, 15), outline=0, fill=0)
        self.draw.text((0, 0), title, font=font, fill=255)

    def clear_content(self):
        self.draw.rectangle((0, 16, 127, 63), outline=0, fill=0)

    def update(self):
        """Override this in subclasses"""
        pass

    def show(self):
        self.device.display(self.image)


# -------------------------------------------------
# Individual Screens
# -------------------------------------------------
class SystemScreen(Screen):
    def update(self):
        self.clear_content()
        cpu = psutil.cpu_percent()
        mem = psutil.virtual_memory().percent
        disk_usage = psutil.disk_usage(path="/").percent
        temp = None
        try:
            with open("/sys/class/thermal/thermal_zone0/temp") as f:
                temp = int(f.read().strip()) / 1000.0
        except FileNotFoundError:
            pass

        self.draw_header("⚙ System Stats")
        self.draw.text((0, 16), f"CPU: {cpu:>4.1f}%", font=font_small, fill=255)
        self.draw.text((0, 28), f"MEM: {mem:>4.1f}%", font=font_small, fill=255)
        if temp:
            self.draw.text((0, 40), f"TEMP: {temp:>4.1f}°C", font=font_small, fill=255)
        else:
            self.draw.text((0, 40), "TEMP: n/a", font=font_small, fill=255)
        self.draw.text((0, 52), f"DISK: {disk_usage:>4.1f}%", font=font_small, fill=255)

class NetworkScreen(Screen):
    def get_ip(self):
        try:
            return subprocess.check_output(["hostname", "-I"]).decode().split()[0]
        except Exception:
            return "No network"

    def update(self):
        self.clear_content()
        ip = self.get_ip()
        net = psutil.net_io_counters()
        # count established inet connections (may require privileges on some systems)
        try:
            conns = psutil.net_connections(kind='inet')
            established = sum(1 for c in conns if getattr(c, "status", "").upper() == "ESTABLISHED")
        except Exception:
            established = 0

        self.draw_header("⇄ Network")
        self.draw.text((2, 16), f"IP: {ip}", font=font_small, fill=255)
        self.draw.text((2, 28), f"▲ SENT: {net.bytes_sent//1024} KB", font=font_small, fill=255)
        self.draw.text((2, 40), f"▼ RECV: {net.bytes_recv//1024} KB", font=font_small, fill=255)
        self.draw.text((2, 52), f"⇶ ESTAB: {established}", font=font_small, fill=255)

class ClockScreen(Screen):
    def update(self):
        self.clear_content()
        now = datetime.datetime.now()
        self.draw_header("▷ Clock")
        self.draw.text((8, 16), now.strftime("%Y-%m-%d"), font=font_big, fill=255)
        self.draw.text((16, 38), now.strftime("%H:%M:%S"), font=font_big, fill=255)


# -------------------------------------------------
# Dashboard Controller
# -------------------------------------------------
class Dashboard:
    def __init__(self, device, screens, switch_interval=5, refresh_interval=1):
        self.device = device
        self.screens = screens
        self.current = 0
        self.switch_interval = switch_interval
        self.refresh_interval = refresh_interval

    def run(self):
        last_switch = time.time()
        while True:
            now = time.time()
            # Update current screen
            screen = self.screens[self.current]
            screen.update()
            screen.show()
            time.sleep(self.refresh_interval)

            # Change screen every switch_interval seconds
            if now - last_switch >= self.switch_interval:
                self.current = (self.current + 1) % len(self.screens)
                last_switch = now


# -------------------------------------------------
# Setup display and run
# -------------------------------------------------
serial = i2c(port=1, address=0x3C)
device = ssd1306(serial, width=128, height=64)

# Parse arguments
parser = argparse.ArgumentParser(description="OLED dashboard")
parser.add_argument("--once", action="store_true", help="Show selected screen once and exit without clearing the display")
parser.add_argument("--screen", type=str, help="Select screen: 'system', 'network', 'clock' or index (0-based)")
args = parser.parse_args()

# Create screen instances
screens = [SystemScreen(device), NetworkScreen(device), ClockScreen(device)]

# Do not clear display if --once (to avoid erasing content that was there)
if not args.once:
    device.clear()

# Select screen
selected_index = 0
if args.screen:
    s = args.screen.lower()
    name_map = {"system": 0, "network": 1, "clock": 2}
    if s.isdigit():
        idx = int(s)
        if idx < 0 or idx >= len(screens):
            print(f"Invalid screen index: {idx}")
            sys.exit(1)
        selected_index = idx
    elif s in name_map:
        selected_index = name_map[s]
    else:
        print(f"Invalid screen name: {args.screen}")
        sys.exit(1)

# Once mode
if args.once:
    screen = screens[selected_index]
    screen.update()
    screen.show()
    # Prevent automatic cleanup/clearing on interpreter exit (some device implementations clear on exit)
    try:
        if hasattr(device, "clear_on_exit"):
            device.clear_on_exit = False
    except Exception:
        pass
    if hasattr(device, "cleanup"):
        # override cleanup to no-op so atexit handlers won't clear the display
        device.cleanup = lambda *a, **k: None
    # short delay to allow the display buffer to be sent before exiting
    time.sleep(0.5)
    sys.exit(0)

# Default dashboard mode
dashboard = Dashboard(device, screens, switch_interval=10, refresh_interval=1)
dashboard.current = selected_index
dashboard.run()
