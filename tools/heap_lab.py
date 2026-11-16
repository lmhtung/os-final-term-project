"""Control the ESP32-S3 heap lab over UART."""

import argparse
import json
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk

import serial
from serial.tools import list_ports


PRESETS = {
    "Tải bình thường": (4, 4, 0, "A xin 4, B xin 4."),
    "Tăng tải của A": (20, 4, 0, "A xin 20, B xin 4."),
    "Áp lực bộ nhớ": (40, 4, 0, "A xin 40, B xin 4."),
    "Chèn lỗi OOM": (20, 4, 3, "A xin 20, B xin 4. Từ chối mỗi yêu cầu thứ 3."),
    "B vượt phần dự trữ": (40, 12, 0, "A xin 40, B xin 12."),
}
MODES = {
    "ADAPTIVE": ("A riêng", "B riêng", "Vùng chung"),
    "STATIC": ("A riêng", "B riêng"),
    "SHARED": ("Heap chung",),
    "SHARED_QUOTA": ("Heap chung có hạn mức",),
}
BG, PAPER, INK, MUTED, BORDER = "#eef2f8", "#ffffff", "#172741", "#455a75", "#d6deea"
COLORS = {"A": "#225fc6", "B": "#08846a", ".": "#dce5f0",
          "H": "#596d87", "G": "#b46c15", "X": "#bf4444"}
LABELS = {"A": "A đang giữ", "B": "B đang giữ", ".": "Trống",
          "H": "Header", "G": "Guard", "X": "Cách ly"}
ERRORS = {
    "BUSY": "Còn buffer đang giữ. Hãy giải phóng các buffer trước.",
    "NO_MEMORY": "Thiếu khối trống phù hợp.",
    "QUOTA": "Yêu cầu vượt hạn mức còn lại.",
    "INJECTED": "Yêu cầu bị từ chối do chèn lỗi.",
    "BAD_POINTER": "Buffer đã được giải phóng hoặc mã không còn hợp lệ.",
    "CORRUPT": "Allocator phát hiện vùng nhớ hỏng.",
    "INVALID": "Thông số không hợp lệ.",
}


class Uart:
    def __init__(self, port, baud):
        self.connection = serial.Serial(port=None, baudrate=baud, timeout=0.15, write_timeout=1)
        self.connection.dtr = False
        self.connection.rts = False
        self.connection.port = port
        self.buffer = bytearray()
        self.sequence = time.monotonic_ns() % 1_000_000_000

    def open(self):
        try:
            self.connection.open()
        except serial.SerialException as error:
            raise ConnectionError(
                f"Không mở được {self.connection.port}. Đóng IDF Monitor (Ctrl+]) "
                "và kiểm tra cổng COM, rồi kết nối lại."
            ) from error

    def close(self):
        self.connection.close()

    def exchange(self, command, timeout=3):
        self.sequence += 1
        prefix = f"LAB,{self.sequence},".encode("ascii")
        self.connection.write(f"\nLAB {self.sequence} {command}\n".encode("ascii"))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.buffer.extend(self.connection.read(min(4096, self.connection.in_waiting or 1)))
            while b"\n" in self.buffer:
                line, _, rest = self.buffer.partition(b"\n")
                self.buffer = rest
                if line.startswith(prefix):
                    state = json.loads(line[len(prefix):])
                    self.check_state(state)
                    return state
            if len(self.buffer) > 8192:
                self.buffer.clear()
        raise TimeoutError(
            "Đã mở cổng nhưng bo chưa trả lời. Kiểm tra log khởi động có HEAP_LAB_READY "
            "và tốc độ UART trong app."
        )

    @staticmethod
    def check_state(state):
        mode, alignment = state["mode"], state["align"]
        sizes, cells = state["regions"], state["cells"]
        if (mode not in MODES or alignment != 16 or state["arena"] != 8192
                or len(sizes) != len(MODES[mode])
                or any(size <= 0 or size % alignment for size in sizes)
                or sum(sizes) != state["arena"] or len(cells) * alignment != state["arena"]
                or set(cells).difference(COLORS)
                or len(state["groups"]) != 2 or len(state["blocks"]) != state["live"]):
            raise ValueError("Dữ liệu bản đồ bộ nhớ không nhất quán.")


class HeapLab:
    def __init__(self, root, port, baud):
        self.root = root
        self.baud = baud
        self.port = tk.StringVar(value=port)
        self.mode = tk.StringVar(value="ADAPTIVE")
        self.preset = tk.StringVar(value="Áp lực bộ nhớ")
        self.group = tk.StringVar(value="A")
        self.size = tk.StringVar(value="256")
        self.count = tk.StringVar(value="1")
        self.connection = None
        self.snapshot = None
        self.pending = False
        self.events = queue.Queue()
        self.actions = []
        self.command_text = tk.StringVar(value="Chọn cổng COM và kết nối để đọc bộ nhớ trên bo.")
        self.status_text = tk.StringVar(value="Chưa kết nối")
        self.preset_text = tk.StringVar()
        self.cards = []

        root.title("Heap Lab · ESP32-S3")
        root.geometry("1180x805")
        root.minsize(1050, 760)
        root.configure(bg=BG)
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.style()
        self.layout()
        self.scan_ports()
        self.update_preset()
        self.update_controls()
        self.root.after(60, self.receive)

    @staticmethod
    def style():
        style = ttk.Style()
        style.theme_use("clam")
        style.configure(".", font=("Segoe UI", 10))
        style.configure("TButton", padding=(10, 5), foreground=INK)
        style.configure("Primary.TButton", background="#225fc6", foreground="white")
        style.map("Primary.TButton", background=[("disabled", "#d6deea"), ("active", "#174fae")],
                  foreground=[("disabled", "#5e7190")])
        style.configure("Treeview", rowheight=25, fieldbackground=PAPER, background=PAPER, foreground=INK)
        style.configure("Treeview.Heading", padding=(6, 5), background="#e7edf6", foreground=INK)
        style.map("Treeview", background=[("selected", "#d6e6ff")], foreground=[("selected", INK)])

    @staticmethod
    def label(parent, text="", variable=None, size=10, bold=False, fg=INK, bg=PAPER, **kwargs):
        return tk.Label(parent, text=text, textvariable=variable, font=("Segoe UI", size, "bold" if bold else "normal"),
                        bg=bg, fg=fg, anchor="w", justify="left", **kwargs)

    def panel(self, parent):
        return tk.Frame(parent, bg=PAPER, highlightbackground=BORDER, highlightthickness=1, padx=14, pady=10)

    def button(self, parent, text, command, primary=False, dependent=True):
        button = ttk.Button(parent, text=text, command=command, style="Primary.TButton" if primary else "TButton")
        if dependent:
            self.actions.append(button)
        return button

    def layout(self):
        header = tk.Frame(self.root, bg="#172741", padx=20, pady=10)
        header.pack(fill="x")
        self.label(header, "Heap Lab", size=24, bold=True, fg="white", bg="#172741").pack(anchor="w")
        self.label(header, "Bộ nhớ thích nghi  ·  ESP32-S3 / FreeRTOS", fg="#d1e0f6", bg="#172741").pack(anchor="w")

        body = tk.Frame(self.root, bg=BG, padx=14, pady=12)
        body.pack(fill="both", expand=True)
        body.grid_columnconfigure(1, weight=1)
        body.grid_rowconfigure(0, weight=1)
        side = self.panel(body)
        side.grid(row=0, column=0, padx=(0, 12), sticky="ns")
        main = tk.Frame(body, bg=BG)
        main.grid(row=0, column=1, sticky="nsew")
        self.sidebar(side)
        self.dashboard(main)

        footer = tk.Frame(self.root, bg="#dee7f4", padx=16, pady=6)
        footer.pack(fill="x")
        self.label(footer, variable=self.command_text, bg="#dee7f4", fg=INK).pack(fill="x")

    def sidebar(self, side):
        self.label(side, "Kết nối bo mạch", size=13, bold=True).pack(anchor="w")
        port_row = tk.Frame(side, bg=PAPER)
        port_row.pack(fill="x", pady=(7, 4))
        self.ports = ttk.Combobox(port_row, textvariable=self.port, width=14)
        self.ports.pack(side="left", expand=True, fill="x")
        self.button(port_row, "Quét COM", self.scan_ports, dependent=False).pack(side="left", padx=(6, 0))
        self.connect_button = self.button(side, "Kết nối", self.connect, primary=True, dependent=False)
        self.connect_button.pack(fill="x", pady=3)
        self.label(side, variable=self.status_text, fg=MUTED, wraplength=245).pack(anchor="w")

        ttk.Separator(side).pack(fill="x", pady=10)
        self.label(side, "Chính sách bộ nhớ", size=13, bold=True).pack(anchor="w")
        ttk.Combobox(side, textvariable=self.mode, values=list(MODES), state="readonly", width=24).pack(fill="x", pady=(6, 3))
        self.button(side, "Áp dụng chính sách", lambda: self.send(f"MODE {self.mode.get()}", self.mode.get())).pack(fill="x")

        ttk.Separator(side).pack(fill="x", pady=10)
        self.label(side, "Kịch bản có sẵn", size=13, bold=True).pack(anchor="w")
        presets = ttk.Combobox(side, textvariable=self.preset, values=list(PRESETS), state="readonly", width=24)
        presets.pack(fill="x", pady=(6, 3))
        presets.bind("<<ComboboxSelected>>", lambda _event: self.update_preset())
        self.label(side, variable=self.preset_text, fg=MUTED, wraplength=244).pack(anchor="w")
        self.button(side, "Chạy và giữ buffer", self.run_preset, primary=True).pack(fill="x", pady=(5, 0))

        ttk.Separator(side).pack(fill="x", pady=10)
        self.label(side, "Cấp phát thủ công", size=13, bold=True).pack(anchor="w")
        fields = tk.Frame(side, bg=PAPER)
        fields.pack(fill="x", pady=5)
        for column, name in enumerate(("Nhóm", "Byte / buffer", "Số buffer")):
            self.label(fields, name).grid(row=0, column=column, sticky="w", padx=(0, 5))
        ttk.Combobox(fields, textvariable=self.group, values=("A", "B"), state="readonly", width=3).grid(row=1, column=0, sticky="w")
        ttk.Entry(fields, textvariable=self.size, width=9).grid(row=1, column=1, sticky="w")
        ttk.Entry(fields, textvariable=self.count, width=7).grid(row=1, column=2, sticky="w")
        self.button(side, "Cấp phát", self.allocate, primary=True).pack(fill="x")
        self.button(side, "Giải phóng hết", lambda: self.send("CLEAR", "Giải phóng hết")).pack(fill="x", pady=(5, 0))
        self.label(side, "Cần arena trống để đổi chính sách\nhoặc bắt đầu kịch bản.", fg=MUTED).pack(anchor="w", pady=(4, 0))

    def dashboard(self, main):
        self.dashboard_title = tk.StringVar(value="Bản đồ bộ nhớ")
        self.cell_text = tk.StringVar(value="Mỗi ô biểu diễn 16 byte. Di chuột lên sơ đồ để xem địa chỉ tương đối.")
        heap = self.panel(main)
        heap.pack(fill="x")
        self.label(heap, variable=self.dashboard_title, size=16, bold=True).pack(anchor="w")
        self.canvas = tk.Canvas(heap, height=94, bg=PAPER, highlightthickness=0)
        self.canvas.pack(fill="x", pady=(4, 0))
        self.canvas.bind("<Configure>", lambda _event: self.draw_map())
        self.canvas.bind("<Motion>", self.inspect_cell)
        self.canvas.bind("<Leave>", lambda _event: self.cell_text.set("Mỗi ô biểu diễn 16 byte. Di chuột lên sơ đồ để xem địa chỉ tương đối."))
        legend = tk.Frame(heap, bg=PAPER)
        legend.pack(fill="x")
        for cell, text in LABELS.items():
            self.label(legend, "■", fg=COLORS[cell], size=13).pack(side="left")
            self.label(legend, text, fg=MUTED).pack(side="left", padx=(3, 9))
        self.label(heap, variable=self.cell_text, size=9, fg=MUTED).pack(anchor="w", pady=(3, 0))

        metrics = tk.Frame(main, bg=BG)
        metrics.pack(fill="x", pady=9)
        for column, title in enumerate(("Dữ liệu trống", "Khối trống lớn nhất", "Buffer đang giữ")):
            card = self.panel(metrics)
            card.grid(row=0, column=column, sticky="ew", padx=(0, 7 if column < 2 else 0))
            metrics.grid_columnconfigure(column, weight=1, uniform="metric")
            self.label(card, title, fg=MUTED, bold=True).pack(anchor="w")
            value = tk.StringVar(value="—")
            self.cards.append(value)
            self.label(card, variable=value, size=17, bold=True).pack(anchor="w")

        usage = self.panel(main)
        usage.pack(fill="x")
        self.group_labels = []
        for idx, color in enumerate(("A", "B")):
            text = tk.StringVar(value="—")
            self.group_labels.append(text)
            self.label(usage, f"Nhóm {color}", fg=COLORS[color], bold=True).grid(row=idx, column=0, sticky="w", padx=(0, 12))
            self.label(usage, variable=text, fg=INK, wraplength=640).grid(row=idx, column=1, sticky="w")

        live = self.panel(main)
        live.pack(fill="both", expand=True, pady=(9, 0))
        title = tk.Frame(live, bg=PAPER)
        title.pack(fill="x", pady=(0, 5))
        self.label(title, "Buffer đang giữ", size=13, bold=True).pack(side="left")
        self.free_button = self.button(title, "Giải phóng mục đã chọn", self.release)
        self.free_button.pack(side="right")
        self.button(title, "Làm mới", lambda: self.send("GET", "Làm mới")).pack(side="right", padx=6)
        table = tk.Frame(live, bg=PAPER)
        table.pack(fill="both", expand=True)
        self.blocks = ttk.Treeview(table, columns=("id", "group", "size"), show="headings", height=4, selectmode="browse")
        for name, heading, width in (("id", "Mã buffer", 120), ("group", "Chủ sở hữu", 120), ("size", "Payload yêu cầu (byte)", 180)):
            self.blocks.heading(name, text=heading)
            self.blocks.column(name, width=width, anchor="center")
        scrollbar = ttk.Scrollbar(table, orient="vertical", command=self.blocks.yview)
        self.blocks.configure(yscrollcommand=scrollbar.set)
        self.blocks.tag_configure("A", foreground=COLORS["A"])
        self.blocks.tag_configure("B", foreground=COLORS["B"])
        self.blocks.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")
        self.blocks.bind("<<TreeviewSelect>>", lambda _event: self.update_controls())

        log = self.panel(main)
        log.pack(fill="x", pady=(9, 0))
        self.label(log, "Nhật ký thao tác", size=11, bold=True).pack(anchor="w")
        self.log = tk.Listbox(log, height=3, relief="flat", bg=PAPER, fg=MUTED,
                              font=("Segoe UI", 9), highlightthickness=0, activestyle="none")
        self.log.pack(fill="x")

    def scan_ports(self):
        self.ports.configure(values=[item.device for item in list_ports.comports()])

    def update_preset(self):
        self.preset_text.set(PRESETS[self.preset.get()][3] + " Mỗi buffer 256 byte.")

    def draw_map(self):
        self.canvas.delete("all")
        state = self.snapshot
        if not state:
            self.canvas.create_text(2, 35, text="Kết nối ESP32-S3 để hiển thị sơ đồ arena.", anchor="w",
                                    font=("Segoe UI", 11), fill=MUTED)
            return
        width = max(1, self.canvas.winfo_width() - 4)
        step = width / len(state["cells"])
        start = 0
        for region_size, name in zip(state["regions"], MODES[state["mode"]]):
            end = start + region_size // state["align"]
            left = 2 + start * step
            right = 2 + end * step
            self.canvas.create_text(left + 6, 15, text=f"{name}  ·  {region_size} B", anchor="w",
                                    font=("Segoe UI", 9, "bold"), fill=INK)
            run = start
            while run < end:
                color = state["cells"][run]
                stop = run + 1
                while stop < end and state["cells"][stop] == color:
                    stop += 1
                self.canvas.create_rectangle(2 + run * step, 28, 2 + stop * step, 67, fill=COLORS[color], width=0)
                run = stop
            self.canvas.create_rectangle(left, 28, right, 67, outline="#496581")
            self.canvas.create_text(left + 2, 77, text=f"{start * state['align']} B", anchor="nw",
                                    font=("Segoe UI", 8), fill=MUTED)
            start = end
        self.canvas.create_text(2 + width, 77, text=f"{state['arena']} B", anchor="ne",
                                font=("Segoe UI", 8), fill=MUTED)

    def inspect_cell(self, event):
        state = self.snapshot
        if not state or not 28 <= event.y <= 67:
            return
        width = max(1, self.canvas.winfo_width() - 4)
        if not 2 <= event.x < 2 + width:
            return
        index = (event.x - 2) * len(state["cells"]) // width
        offset = index * state["align"]
        self.cell_text.set(f"Byte {offset}–{offset + state['align'] - 1}  ·  {LABELS[state['cells'][index]]}")

    def render(self, snapshot):
        self.snapshot = snapshot
        self.dashboard_title.set(f"Bản đồ bộ nhớ  ·  {snapshot['mode']}  ·  8 KiB" if snapshot else "Bản đồ bộ nhớ")
        self.draw_map()
        for var, key in zip(self.cards, ("free", "largest", "live")):
            var.set((f"{snapshot[key]} B" if key != "live" else str(snapshot[key])) if snapshot else "—")
        for index, var in enumerate(self.group_labels):
            if not snapshot:
                var.set("—")
                continue
            group = snapshot["groups"][index]
            var.set(f"Dùng {group['live']}/{group['limit']} B  ·  Mượn {group['borrowed']} B  ·  "
                    f"OK {group['ok']}  ·  Thiếu {group['no_memory']}  ·  Hạn mức {group['quota']}  ·  Chèn lỗi {group['injected']}")
        selected = self.blocks.selection()
        children = self.blocks.get_children()
        if children:
            self.blocks.delete(*children)
        if snapshot:
            for handle, group, size in sorted(snapshot["blocks"]):
                self.blocks.insert("", "end", iid=str(handle), values=(handle, group, size), tags=(group,))
            if selected and self.blocks.exists(selected[0]):
                self.blocks.selection_set(selected[0])

    def update_controls(self):
        active = self.connection is not None and not self.pending
        for widget in self.actions:
            widget.configure(state="normal" if active else "disabled")
        self.free_button.configure(state="normal" if active and self.blocks.selection() else "disabled")
        self.connect_button.configure(text="Ngắt kết nối" if self.connection else "Kết nối",
                                      state="disabled" if self.pending else "normal")
        self.ports.configure(state="disabled" if self.connection or self.pending else "normal")

    def connect(self):
        if self.connection:
            self.connection.close()
            self.connection = None
            self.status_text.set("Đã ngắt kết nối")
            self.render(None)
            self.update_controls()
            self.note("Đã đóng cổng UART.")
            return
        port = self.port.get().strip()
        if not port:
            self.note("Chọn hoặc nhập cổng COM.")
            return
        self.run_async("Kết nối", "GET", port)

    def allocate(self):
        try:
            size, count = int(self.size.get()), int(self.count.get())
        except ValueError:
            self.note("Kích thước và số buffer phải là số nguyên.")
            return
        if not (1 <= size <= 8192 and 1 <= count <= 40):
            self.note("Nhập từ 1 đến 8192 byte và từ 1 đến 40 buffer.")
            return
        group = self.group.get()
        self.send(f"ALLOC {group} {size} {count}", f"Cấp phát {group} · {count} × {size} B")

    def run_preset(self):
        a, b, nth, _ = PRESETS[self.preset.get()]
        self.send(f"PRESET {a} {b} {nth}", self.preset.get())

    def release(self):
        selected = self.blocks.selection()
        if selected:
            self.send(f"FREE {selected[0]}", f"Giải phóng buffer #{selected[0]}")

    def send(self, command, label):
        if self.connection and not self.pending:
            self.run_async(label, command)

    def run_async(self, label, command, port=None):
        self.pending = True
        self.command_text.set(f"{label} · Đang chờ ESP32-S3…")
        self.update_controls()
        connection = self.connection

        def work():
            link = connection
            try:
                if port:
                    link = Uart(port, self.baud)
                    link.open()
                try:
                    response = link.exchange(command)
                except TimeoutError:
                    if not port:
                        raise
                    response = link.exchange("GET")
                self.events.put((label, link, response, None))
            except (OSError, TimeoutError, ValueError, KeyError, TypeError) as error:
                if link:
                    link.close()
                self.events.put((label, None, None, str(error)))

        threading.Thread(target=work, daemon=True).start()

    def receive(self):
        try:
            label, self.connection, state, error = self.events.get_nowait()
        except queue.Empty:
            self.root.after(60, self.receive)
            return
        self.pending = False
        if error:
            self.render(None)
            self.status_text.set("Mất kết nối / không có phản hồi")
            self.note(error)
        else:
            self.status_text.set(f"{self.connection.connection.port} · {self.baud} baud")
            self.render(state)
            if state["code"] == "OK" and label in MODES:
                self.mode.set(state["mode"])
            if label == "Kết nối":
                self.mode.set(state["mode"])
            detail = f"{state['completed']}/{state['requested']} cấp phát thành công" if state["requested"] else "OK"
            if state["code"] != "OK":
                detail = (detail + " · " if state["requested"] else "") + ERRORS.get(state["code"], state["code"])
            if state["health"] != "OK":
                detail += " · Trạng thái heap: " + state["health"]
            self.note(f"{label} · {detail}")
        self.update_controls()
        self.root.after(60, self.receive)

    def note(self, message):
        self.command_text.set(message)
        self.log.insert(0, message)
        if self.log.size() > 30:
            self.log.delete(30, "end")

    def close(self):
        if self.connection:
            self.connection.close()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    root = tk.Tk()
    HeapLab(root, args.port, args.baud)
    root.mainloop()


if __name__ == "__main__":
    main()
