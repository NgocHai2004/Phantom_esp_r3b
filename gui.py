"""
Phantom Thinker - Quan ly file /uploads tren ESP32-S3 qua COM
Giao thuc Serial: LIST / STATUS / PING / DELETE / DOWNLOAD / UPLOAD
Chay: python gui.py
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
import serial
import serial.tools.list_ports
import threading
import json
import os
import time

TIMEOUT_S = 8
CHUNK     = 512


class PhantomThinker:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Phantom Thinker - ESP32 SD Manager")
        self.root.geometry("900x660")
        self.root.resizable(True, True)

        self.ser: serial.Serial | None = None
        self._lock = threading.Lock()

        self._build_ui()
        self._refresh_ports()

    # ------------------------------------------------------------------ UI --
    def _build_ui(self):
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

        tk.Button(top, text="Refresh Ports",
                  command=self._refresh_ports).pack(side="left", padx=4)

        self.btn_connect = tk.Button(
            top, text="Ket noi", bg="#4caf50", fg="white",
            width=11, command=self._toggle_connect
        )
        self.btn_connect.pack(side="left", padx=8)

        self.lbl_status = tk.Label(top, text="Chua ket noi", fg="gray")
        self.lbl_status.pack(side="left", padx=6)

        mid = tk.PanedWindow(self.root, orient="horizontal", sashrelief="raised")
        mid.pack(fill="both", expand=True, padx=10, pady=4)

        # --- panel trai: danh sach file ---
        left = tk.LabelFrame(mid, text="  Files /uploads  ", padx=4, pady=4)
        mid.add(left, minsize=300)

        btn_row = tk.Frame(left)
        btn_row.pack(fill="x", pady=(0, 4))
        tk.Button(btn_row, text="Lam moi",    width=9,  command=self._cmd_list).pack(side="left", padx=2)
        tk.Button(btn_row, text="Tai xuong",  width=9,  command=self._cmd_download).pack(side="left", padx=2)
        tk.Button(btn_row, text="Xoa file",   width=9,  command=self._cmd_delete).pack(side="left", padx=2)
        tk.Button(btn_row, text="Xoa tat ca", width=10,
                  bg="#f44336", fg="white",
                  command=self._cmd_delete_all).pack(side="left", padx=2)

        cols = ("Filename", "Size")
        self.tree = ttk.Treeview(left, columns=cols, show="headings", selectmode="browse")
        self.tree.heading("Filename", text="Ten file")
        self.tree.heading("Size",     text="Kich thuoc")
        self.tree.column("Filename", width=200, stretch=True)
        self.tree.column("Size",     width=90, anchor="e", stretch=False)
        vsb = ttk.Scrollbar(left, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<Double-1>", lambda _: self._cmd_download())

        # --- panel phai: upload + info + log ---
        right = tk.Frame(mid)
        mid.add(right, minsize=400)

        up_frame = tk.LabelFrame(right, text="  Upload file len ESP32  ", padx=6, pady=6)
        up_frame.pack(fill="x", pady=(0, 6))
        self.entry_upload = tk.Entry(up_frame)
        self.entry_upload.pack(side="left", fill="x", expand=True, padx=(0, 4))
        tk.Button(up_frame, text="Chon...",
                  command=self._browse_upload).pack(side="left", padx=2)
        tk.Button(up_frame, text="Upload", bg="#2196f3", fg="white",
                  width=8, command=self._cmd_upload).pack(side="left", padx=4)

        info_frame = tk.LabelFrame(right, text="  Thong tin thiet bi  ", padx=6, pady=4)
        info_frame.pack(fill="x", pady=(0, 6))
        btn_inf = tk.Frame(info_frame)
        btn_inf.pack(side="left")
        tk.Button(btn_inf, text="STATUS", width=8, command=self._cmd_status).pack(pady=2)
        tk.Button(btn_inf, text="PING",   width=8, command=self._cmd_ping).pack(pady=2)
        self.lbl_sdinfo = tk.Label(info_frame, text="(chua lay du lieu)",
                                   anchor="w", justify="left", fg="gray")
        self.lbl_sdinfo.pack(side="left", padx=12, fill="x", expand=True)

        log_frame = tk.LabelFrame(right, text="  Log  ", padx=4, pady=4)
        log_frame.pack(fill="both", expand=True)
        self.log_box = scrolledtext.ScrolledText(
            log_frame, height=14, state="disabled",
            font=("Consolas", 9), bg="#1e1e1e", fg="#d4d4d4"
        )
        self.log_box.pack(fill="both", expand=True)

        self.progress = ttk.Progressbar(self.root, mode="determinate")
        self.progress.pack(fill="x", padx=10, pady=(0, 6))

    # ------------------------------------------------------- port helpers ---
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
            self._log(f"[COM] Ket noi {port} @ {baud}")
            self.lbl_status.config(text=f"Da ket noi: {port}", fg="#4caf50")
            self.btn_connect.config(text="Ngat ket noi", bg="#f44336")
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
        self._log("[COM] Ngat ket noi")

    # ------------------------------------------------------ serial helpers --
    def _ok(self) -> bool:
        if not self.ser or not self.ser.is_open:
            messagebox.showerror("Loi", "Chua ket noi COM port")
            return False
        return True

    def _send(self, cmd: str):
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\n").encode())

    def _readline(self) -> str:
        return self.ser.readline().decode(errors="replace").strip()

    def _log(self, msg: str):
        def _do():
            self.log_box.config(state="normal")
            ts = time.strftime("%H:%M:%S")
            self.log_box.insert("end", f"[{ts}] {msg}\n")
            self.log_box.see("end")
            self.log_box.config(state="disabled")
        self.root.after(0, _do)

    def _thread(self, fn, *args):
        threading.Thread(target=fn, args=args, daemon=True).start()

    # ---------------------------------------------------------- commands ---
    def _cmd_ping(self):
        if not self._ok(): return
        self._thread(self._do_ping)

    def _do_ping(self):
        with self._lock:
            try:
                self._send("PING")
                self._log(f"[PING] {self._readline()}")
            except Exception as e:
                self._log(f"[PING] ERR: {e}")

    def _cmd_status(self):
        if not self._ok(): return
        self._thread(self._do_status)

    def _do_status(self):
        with self._lock:
            try:
                self._send("STATUS")
                raw  = self._readline()
                data = json.loads(raw)
                dd   = data.get("dinh_danh", {})
                info = (
                    f"SD: {data.get('sd_used_human','?')} / {data.get('sd_total_human','?')}"
                    f"  [{data.get('sd_card_type','?')}]\n"
                    f"ID: {dd.get('id_thiet_bi','?')}  |  {dd.get('ho_ten','?')}"
                )
                self.root.after(0, lambda: self.lbl_sdinfo.config(text=info, fg="black"))
                self._log("[STATUS] ok")
            except Exception as e:
                self._log(f"[STATUS] ERR: {e}")

    def _cmd_list(self):
        if not self._ok(): return
        self._thread(self._do_list)

    def _do_list(self):
        with self._lock:
            self._list_locked()

    def _list_locked(self):
        try:
            self._send("LIST")
            raw   = self._readline()
            data  = json.loads(raw)
            files = data.get("files", [])
            self.root.after(0, lambda: self._populate_tree(files))
            self._log(f"[LIST] {len(files)} file(s)")
        except Exception as e:
            self._log(f"[LIST] ERR: {e}")

    def _populate_tree(self, files):
        for r in self.tree.get_children():
            self.tree.delete(r)
        for f in files:
            self.tree.insert("", "end",
                             values=(f["name"], f.get("size_human", str(f.get("size", 0)))))

    def _sel_name(self) -> str | None:
        sel = self.tree.selection()
        if not sel:
            messagebox.showwarning("Chua chon", "Chon mot file truoc")
            return None
        return str(self.tree.item(sel[0])["values"][0])

    def _cmd_download(self):
        if not self._ok(): return
        name = self._sel_name()
        if not name: return
        dest = filedialog.asksaveasfilename(initialfile=name,
                                            title="Luu file ve may tinh")
        if not dest: return
        self._thread(self._do_download, name, dest)

    def _do_download(self, name: str, dest: str):
        with self._lock:
            try:
                self._send(f"DOWNLOAD:{name}")
                hdr = self._readline()
                if not hdr.startswith("SIZE:"):
                    self._log(f"[DOWNLOAD] ERR: {hdr}")
                    return
                total = int(hdr[5:])
                self._log(f"[DOWNLOAD] {name}  {total} bytes ...")
                self.root.after(0, lambda: self.progress.config(maximum=max(total, 1), value=0))

                received = 0
                with open(dest, "wb") as fout:
                    while received < total:
                        need  = min(CHUNK, total - received)
                        chunk = self.ser.read(need)
                        if not chunk:
                            self._log("[DOWNLOAD] ERR: timeout")
                            return
                        fout.write(chunk)
                        received += len(chunk)
                        v = received
                        self.root.after(0, lambda x=v: self.progress.config(value=x))

                done = self._readline()
                self.root.after(0, lambda: self.progress.config(value=0))
                self._log(f"[DOWNLOAD] OK  {os.path.basename(dest)}  ({done})")
            except Exception as e:
                self._log(f"[DOWNLOAD] ERR: {e}")

    def _cmd_delete(self):
        if not self._ok(): return
        name = self._sel_name()
        if not name: return
        if not messagebox.askyesno("Xac nhan", f"Xoa file:\n{name}"):
            return
        self._thread(self._do_delete, name)

    def _do_delete(self, name: str):
        with self._lock:
            try:
                self._send(f"DELETE:{name}")
                resp = self._readline()
                self._log(f"[DELETE] {resp}")
                if resp.startswith("OK"):
                    self._list_locked()
            except Exception as e:
                self._log(f"[DELETE] ERR: {e}")

    def _cmd_delete_all(self):
        if not self._ok(): return
        if not messagebox.askyesno("Xac nhan",
                                   "Xoa TAT CA file trong /uploads?\nKhong the hoan tac!"):
            return
        self._thread(self._do_delete_all)

    def _do_delete_all(self):
        with self._lock:
            try:
                self._send("DELETE_ALL")
                resp = self._readline()
                self._log(f"[DELETE_ALL] {resp}")
                self._list_locked()
            except Exception as e:
                self._log(f"[DELETE_ALL] ERR: {e}")

    def _browse_upload(self):
        path = filedialog.askopenfilename(title="Chon file can upload len ESP32")
        if path:
            self.entry_upload.delete(0, "end")
            self.entry_upload.insert(0, path)

    def _cmd_upload(self):
        if not self._ok(): return
        path = self.entry_upload.get().strip()
        if not path or not os.path.isfile(path):
            messagebox.showerror("Loi", f"File khong ton tai:\n{path}")
            return
        self._thread(self._do_upload, path)

    def _do_upload(self, path: str):
        with self._lock:
            name = os.path.basename(path)
            size = os.path.getsize(path)
            self._log(f"[UPLOAD] {name}  {size} bytes ...")
            try:
                self._send(f"UPLOAD:{name}:{size}")
                ack = self._readline()
                if ack != "READY":
                    self._log(f"[UPLOAD] ERR: {ack}")
                    return

                self.root.after(0, lambda: self.progress.config(maximum=max(size, 1), value=0))
                sent = 0
                with open(path, "rb") as fin:
                    while True:
                        chunk = fin.read(CHUNK)
                        if not chunk:
                            break
                        self.ser.write(chunk)
                        sent += len(chunk)
                        v = sent
                        self.root.after(0, lambda x=v: self.progress.config(value=x))

                resp = self._readline()
                self.root.after(0, lambda: self.progress.config(value=0))
                self._log(f"[UPLOAD] {resp}")
                if resp.startswith("OK"):
                    self._list_locked()
            except Exception as e:
                self._log(f"[UPLOAD] ERR: {e}")


if __name__ == "__main__":
    root = tk.Tk()
    PhantomThinker(root)
    root.mainloop()
