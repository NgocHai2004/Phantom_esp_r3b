"""
Phantom Thinker - Xem Transfer Log tu ESP32-S3 qua COM
Chay: python gui.py
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import serial
import serial.tools.list_ports
import threading
import json
import time


TIMEOUT_S = 10


class PhantomThinker:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Phantom Thinker - Transfer Log Viewer")
        self.root.geometry("860x520")
        self.root.resizable(True, True)

        self.ser: serial.Serial | None = None
        self._lock = threading.Lock()
        self._csv_raw = ""

        self._build_ui()
        self._refresh_ports()

    # ================================================================ UI ===
    def _build_ui(self):
        # --- thanh ket noi ---
        top = tk.Frame(self.root, pady=6)
        top.pack(fill="x", padx=10)

        tk.Label(top, text="COM Port:").pack(side="left")
        self.cb_port = ttk.Combobox(top, width=12, state="readonly")
        self.cb_port.pack(side="left", padx=4)

        tk.Label(top, text="Baud:").pack(side="left")
        self.cb_baud = ttk.Combobox(
            top, width=9,
            values=["115200", "230400", "460800", "921600"],
            state="readonly"
        )
        self.cb_baud.set("115200")
        self.cb_baud.pack(side="left", padx=4)

        tk.Button(top, text="Refresh", command=self._refresh_ports).pack(side="left", padx=4)

        self.btn_connect = tk.Button(
            top, text="Ket noi", bg="#4caf50", fg="white",
            width=11, command=self._toggle_connect
        )
        self.btn_connect.pack(side="left", padx=8)

        self.lbl_status = tk.Label(top, text="Chua ket noi", fg="gray")
        self.lbl_status.pack(side="left", padx=6)

        # --- thanh cong cu log ---
        bar = tk.Frame(self.root, pady=4)
        bar.pack(fill="x", padx=10)

        tk.Button(bar, text="Tai lai log", width=12,
                  command=self._cmd_read_log).pack(side="left", padx=4)
        tk.Button(bar, text="Xoa log tren ESP32", bg="#f44336", fg="white",
                  width=20, command=self._cmd_clear_log).pack(side="left", padx=4)
        tk.Button(bar, text="Luu CSV ra may tinh",
                  width=20, command=self._export_csv).pack(side="left", padx=4)

        self.lbl_count = tk.Label(bar, text="", fg="gray")
        self.lbl_count.pack(side="left", padx=8)

        # --- bang log ---
        cols    = ("uptime_ms", "type", "filename", "bytes", "duration_s", "speed_kbs", "status")
        headers = ("Uptime (ms)", "Loai", "Ten file", "Bytes", "Thoi gian (s)", "Toc do (KB/s)", "Trang thai")
        widths  = [100, 110, 220, 90, 110, 110, 70]

        frame = tk.Frame(self.root)
        frame.pack(fill="both", expand=True, padx=10, pady=(0, 8))

        self.tree = ttk.Treeview(frame, columns=cols, show="headings")
        for c, h, w in zip(cols, headers, widths):
            self.tree.heading(c, text=h)
            self.tree.column(c, width=w,
                             anchor="w" if c == "filename" else "e",
                             stretch=(c == "filename"))

        vsb = ttk.Scrollbar(frame, orient="vertical",   command=self.tree.yview)
        hsb = ttk.Scrollbar(frame, orient="horizontal",  command=self.tree.xview)
        self.tree.configure(yscrollcommand=vsb.set, xscrollcommand=hsb.set)
        vsb.pack(side="right", fill="y")
        hsb.pack(side="bottom", fill="x")
        self.tree.pack(fill="both", expand=True)

    # ============================================================ helpers ===
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.cb_port["values"] = ports
        if ports:
            self.cb_port.set(ports[0])

    def _toggle_connect(self):
        if self.ser and self.ser.is_open:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.cb_port.get()
        baud = int(self.cb_baud.get())
        if not port:
            messagebox.showerror("Loi", "Chua chon COM port")
            return
        try:
            self.ser = serial.Serial(port, baud, timeout=TIMEOUT_S)
            time.sleep(1.5)
            self.lbl_status.config(text=f"Da ket noi: {port}", fg="#4caf50")
            self.btn_connect.config(text="Ngat ket noi", bg="#f44336")
            # Tu dong tai log khi ket noi
            threading.Thread(target=self._do_read_log, daemon=True).start()
        except Exception as e:
            messagebox.showerror("Loi ket noi", str(e))

    def _disconnect(self):
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None
        self.lbl_status.config(text="Chua ket noi", fg="gray")
        self.btn_connect.config(text="Ket noi", bg="#4caf50")

    def _ok(self) -> bool:
        if not self.ser or not self.ser.is_open:
            messagebox.showerror("Loi", "Chua ket noi COM port")
            return False
        return True

    def _send_json(self, obj: dict):
        self.ser.reset_input_buffer()
        self.ser.write((json.dumps(obj, separators=(",", ":")) + "\n").encode())

    def _recv_json(self) -> dict:
        deadline = time.time() + TIMEOUT_S
        while time.time() < deadline:
            raw = self.ser.readline().decode(errors="replace").strip()
            if raw.startswith("{"):
                return json.loads(raw)
        raise TimeoutError("Khong nhan duoc phan hoi")

    # ===================================================== Transfer Log ===
    def _cmd_read_log(self):
        if not self._ok(): return
        threading.Thread(target=self._do_read_log, daemon=True).start()

    def _do_read_log(self):
        with self._lock:
            try:
                self._send_json({"cmd": "read_log"})
                r = self._recv_json()
                if not r.get("ok"):
                    err = r.get("error", "")
                    self.root.after(0, lambda: self.lbl_count.config(
                        text=f"ERR: {err}", fg="red"))
                    return
                csv_text = r.get("csv", "").replace("\\n", "\n").replace("\\\"", "\"")
                self._csv_raw = csv_text
                self.root.after(0, lambda: self._populate(csv_text))
            except Exception as e:
                self.root.after(0, lambda: self.lbl_count.config(
                    text=f"ERR: {e}", fg="red"))

    def _populate(self, csv_text: str):
        for r in self.tree.get_children():
            self.tree.delete(r)

        lines = [l for l in csv_text.splitlines() if l.strip()]
        if not lines:
            self.lbl_count.config(text="(chua co du lieu)", fg="gray")
            return

        data_lines = lines[1:] if lines[0].startswith("uptime") else lines
        for line in data_lines:
            parts = line.split(",")
            if len(parts) >= 7:
                self.tree.insert("", "end", values=tuple(parts[:7]))

        self.lbl_count.config(text=f"{len(data_lines)} ban ghi", fg="black")

    def _cmd_clear_log(self):
        if not self._ok(): return
        if not messagebox.askyesno("Xac nhan", "Xoa log tren ESP32?\nKhong the hoan tac!"): return
        threading.Thread(target=self._do_clear_log, daemon=True).start()

    def _do_clear_log(self):
        with self._lock:
            try:
                self._send_json({"cmd": "clear_log"})
                r = self._recv_json()
                if r.get("ok"):
                    self._csv_raw = ""
                    self.root.after(0, lambda: self._populate(""))
            except Exception as e:
                self.root.after(0, lambda: self.lbl_count.config(
                    text=f"ERR: {e}", fg="red"))

    def _export_csv(self):
        if not self._csv_raw:
            messagebox.showinfo("Thong bao", "Chua co du lieu. Ket noi va tai log truoc.")
            return
        dest = filedialog.asksaveasfilename(
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
            initialfile="transfer_log.csv",
            title="Luu CSV ra may tinh"
        )
        if not dest: return
        with open(dest, "w", encoding="utf-8") as f:
            f.write(self._csv_raw)


if __name__ == "__main__":
    root = tk.Tk()
    PhantomThinker(root)
    root.mainloop()
