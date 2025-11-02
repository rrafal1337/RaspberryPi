#!/usr/bin/env python3
from luma.core.interface.serial import i2c
from luma.oled.device import ssd1306
from PIL import Image, ImageDraw, ImageFont
import time, subprocess, threading, sys
import argparse

# -------------------------------------------------
# Font setup
# -------------------------------------------------
font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
font = ImageFont.truetype(font_path, 10)
font_small = ImageFont.truetype(font_path, 9)

# -------------------------------------------------
# Ping Monitor Class
# -------------------------------------------------
class PingMonitor:
    def __init__(self, hosts):
        # hosts: list of tuples (ip, friendly_name)
        self.hosts = hosts
        # status keyed by ip
        self.status = {ip: {'alive': True, 'fail_count': 0, 'last_check': 0} for ip, _ in hosts}
        self.lock = threading.Lock()
        self.running = True
        
    def ping_host(self, ip):
        """Ping a single ip and return True if successful"""
        try:
            result = subprocess.run(
                ['ping', '-c', '1', '-W', '1', ip],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=2
            )
            return result.returncode == 0
        except Exception:
            return False
    
    def monitor_loop(self):
        """Continuous monitoring loop in background thread"""
        while self.running:
            for ip, _ in self.hosts:
                if not self.running:
                    break
                    
                success = self.ping_host(ip)
                
                with self.lock:
                    if success:
                        # Host responded - reset fail count and mark as alive
                        self.status[ip]['fail_count'] = 0
                        self.status[ip]['alive'] = True
                    else:
                        # Host didn't respond - increment fail count
                        self.status[ip]['fail_count'] += 1
                        # Mark as dead if failed twice in a row
                        if self.status[ip]['fail_count'] >= 2:
                            self.status[ip]['alive'] = False
                    
                    self.status[ip]['last_check'] = time.time()
            
            # Wait 3 seconds before next round
            time.sleep(3)
    
    def get_status(self, ip):
        """Get current status of a host by ip"""
        with self.lock:
            return self.status.get(ip, {}).get('alive', False)
    
    def start(self):
        """Start monitoring in background thread"""
        self.thread = threading.Thread(target=self.monitor_loop, daemon=True)
        self.thread.start()
    
    def stop(self):
        """Stop monitoring"""
        self.running = False
        if hasattr(self, 'thread'):
            self.thread.join(timeout=5)


# -------------------------------------------------
# Base Screen Class
# -------------------------------------------------
class Screen:
    def __init__(self, device):
        self.device = device
        self.image = Image.new("1", (device.width, device.height))
        self.draw = ImageDraw.Draw(self.image)

    def draw_header(self, title):
        self.draw.rectangle((0, 0, 127, 11), outline=0, fill=0)
        self.draw.text((0, 0), title, font=font, fill=255)
        # Draw line under header
        self.draw.line((0, 11, 127, 11), fill=255)

    def clear_content(self):
        self.draw.rectangle((0, 12, 127, 63), outline=0, fill=0)

    def update(self):
        """Override this in subclasses"""
        pass

    def show(self):
        self.device.display(self.image)


# -------------------------------------------------
# Ping Status Screen
# -------------------------------------------------
class PingScreen(Screen):
    def __init__(self, device, monitor, hosts_subset, page_num, total_pages):
        super().__init__(device)
        self.monitor = monitor
        # hosts_subset: list of (ip, friendly_name)
        self.hosts = hosts_subset
        self.page_num = page_num
        self.total_pages = total_pages
    
    def update(self):
        self.clear_content()
        
        # Header with page info
        if self.total_pages > 1:
            title = f"Ping Monitor [{self.page_num}/{self.total_pages}]"
        else:
            title = "Ping Monitor"
        self.draw_header(title)
        
        # Display hosts: single column, 4 rows
        row_height = 13
        col_width = 128
        
        for idx, (ip, friendly) in enumerate(self.hosts):
            row = idx  # 0..3
            col = 0
            
            x = col * col_width
            y = 14 + row * row_height
            
            # Get status by ip
            alive = self.monitor.get_status(ip)
            
            # Display "IP friendly" (IP, space, friendly name)
            display_name = f"{ip} {friendly}"
            # Draw status indicator (filled circle for UP, empty for DOWN)
            if alive:
                # Filled circle for UP
                self.draw.ellipse((x+1, y+2, x+6, y+7), outline=255, fill=255)
                self.draw.text((x+9, y), display_name, font=font_small, fill=255)
            else:
                # Empty circle for DOWN
                self.draw.ellipse((x+1, y+2, x+6, y+7), outline=255, fill=0)
                self.draw.text((x+9, y), display_name, font=font_small, fill=255)


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
def main():
    # Parse arguments
    parser = argparse.ArgumentParser(description="OLED ping monitor dashboard")
    parser.add_argument("hosts", nargs="+", help="List of hosts to ping in format: ip,friendlyname")
    parser.add_argument("--switch-interval", type=int, default=10, 
                       help="Seconds between screen switches (default: 10)")
    parser.add_argument("--refresh-interval", type=float, default=1, 
                       help="Screen refresh interval in seconds (default: 1)")
    args = parser.parse_args()

    # Parse host entries (expect ip,friendlyname)
    hosts_entries = []
    for entry in args.hosts:
        if ',' not in entry:
            print(f"Invalid host entry '{entry}'. Expected format: 192.168.55.55,friendlyname")
            sys.exit(1)
        ip, friendly = entry.split(',', 1)
        ip = ip.strip()
        friendly = friendly.strip()
        if not ip or not friendly:
            print(f"Invalid host entry '{entry}'. Both ip and friendly name required.")
            sys.exit(1)
        hosts_entries.append((ip, friendly))

    if not hosts_entries:
        print("No hosts provided.")
        sys.exit(1)

    # Setup device
    serial = i2c(port=1, address=0x3C)
    device = ssd1306(serial, width=128, height=64)
    device.clear()

    # Create ping monitor
    monitor = PingMonitor(hosts_entries)
    monitor.start()
    
    # Give monitor a moment to do initial pings
    print("Starting ping monitor...")
    time.sleep(2)

    # Create screens (4 hosts per screen)
    hosts_per_screen = 4
    screens = []
    total_pages = (len(hosts_entries) + hosts_per_screen - 1) // hosts_per_screen
    
    for i in range(0, len(hosts_entries), hosts_per_screen):
        hosts_subset = hosts_entries[i:i+hosts_per_screen]
        page_num = (i // hosts_per_screen) + 1
        screens.append(PingScreen(device, monitor, hosts_subset, page_num, total_pages))
    
    print(f"Monitoring {len(hosts_entries)} hosts on {len(screens)} screen(s)")
    print("Press Ctrl+C to exit")

    # Run dashboard
    try:
        dashboard = Dashboard(device, screens, 
                            switch_interval=args.switch_interval,
                            refresh_interval=args.refresh_interval)
        dashboard.run()
    except KeyboardInterrupt:
        print("\nStopping monitor...")
        monitor.stop()
        device.clear()
        sys.exit(0)

if __name__ == "__main__":
    main()