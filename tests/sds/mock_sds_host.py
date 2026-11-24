#!/usr/bin/env python3
"""Mock secure-desktop-stream host for the Moonlight client tests.

Implements the /sds/v1 endpoints from docs/DESIGN.md (sections 4-6) on an HTTPS server that
requires the paired client certificate (mTLS), plus fault injection for the client's error
handling. Standard library only; run it with `python3 -I`.

Control channel: one JSON object per line on stdin, one JSON reply per line on stdout. The
first stdout line is {"ready": true, "port": N}.
"""

import argparse
import base64
import hashlib
import hmac
import json
import os
import secrets
import socket
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class State:
    def __init__(self):
        self.cond = threading.Condition()
        self.feature_enabled = True
        self.stream_active = True
        self.version = 1
        self.features = ["clipboard", "files", "file_clipboard", "lazy_clipboard", "mic"]
        self.drop_delivered = "xdnd"
        self.permissions = ["clipboard_read", "clipboard_write", "file_upload", "file_download"]
        self.limits = {
            "clipboard_text_max": 1048576,
            "clipboard_image_max": 16777216,
            "file_max": 4294967296,
            "chunk_max": 4194304,
            "uploads_max_concurrent": 2,
        }
        self.events = []
        self.next_event_id = 1
        self.clip_seq = 0
        self.clip = {}
        self.file_clip_seq = 0
        self.file_clip_epoch = secrets.token_hex(16)
        self.file_clip = None
        self.file_clip_puts = []
        self.file_clip_block = False
        self.client_offers = {}
        self.completed_offers = {}
        self.host_offers = {}
        self.drops = {}
        self.completed_drops = []
        self.drop_counter = 0
        self.client_puts = []
        self.uploads = {}
        self.completed = []
        self.completed_names = set()
        self.outbox = {}
        self.outbox_key = secrets.token_bytes(32)
        self.outbox_counter = 0
        self.faults = []
        self.log = []
        self.handshake_failures = 0

    # Must hold self.cond
    def add_event(self, ev):
        ev = dict(ev)
        ev["id"] = self.next_event_id
        self.next_event_id += 1
        self.events.append(ev)
        self.cond.notify_all()
        return ev["id"]

    # Must hold self.cond
    def take_fault(self, method, path, kinds):
        for f in self.faults:
            if f.get("count", 1) <= 0 or f["kind"] not in kinds:
                continue
            if f.get("method") and f["method"] != method:
                continue
            if f.get("path_prefix") and not path.startswith(f["path_prefix"]):
                continue
            if f.get("path_suffix") and not path.endswith(f["path_suffix"]):
                continue
            if f.get("skip", 0) > 0:
                f["skip"] -= 1
                continue
            f["count"] = f.get("count", 1) - 1
            return f
        return None


def is_valid_utf8_text(data):
    if b"\x00" in data:
        return False
    try:
        data.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return False
    return True


def final_name(state, name):
    stem, dot, ext = name.rpartition(".")
    if not dot or not stem:
        stem, ext = name, ""
    candidate = name
    n = 0
    while candidate in state.completed_names:
        n += 1
        candidate = f"{stem} ({n}).{ext}" if ext else f"{stem} ({n})"
    state.completed_names.add(candidate)
    return candidate


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockSunshine/1"

    def log_message(self, fmt, *args):
        pass

    def setup(self):
        self.request.settimeout(60)
        try:
            self.request.do_handshake()
        except (ssl.SSLError, OSError):
            with self.server.state.cond:
                self.server.state.handshake_failures += 1
            raise
        super().setup()

    # ---- responses ---------------------------------------------------------------------

    def send(self, status, body=b"", ctype="application/json", headers=None, close=False):
        self.send_response(status)
        if status != 204:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, str(v))
        if close:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        if body:
            self.wfile.write(body)
        self.log_entry["status"] = status

    def json_resp(self, status, obj, headers=None):
        self.send(status, json.dumps(obj).encode(), headers=headers)

    def error(self, status, code, message="", extra=None, headers=None, close=False):
        obj = {"error": code, "message": message}
        obj.update(extra or {})
        self.send(status, json.dumps(obj).encode(), headers=headers, close=close)

    # ---- dispatch ----------------------------------------------------------------------

    def do_GET(self):
        self.dispatch("GET")

    def do_PUT(self):
        self.dispatch("PUT")

    def do_POST(self):
        self.dispatch("POST")

    def do_DELETE(self):
        self.dispatch("DELETE")

    def dispatch(self, method):
        st = self.server.state
        url = urlsplit(self.path)
        path = url.path
        self.query = {k: v[0] for k, v in parse_qs(url.query).items()}
        length = int(self.headers.get("Content-Length") or 0)
        self.log_entry = {
            "method": method,
            "path": path,
            "query": self.query,
            "length": length,
            "content_type": self.headers.get("Content-Type"),
            "client_seq": self.headers.get("X-SDS-Client-Seq"),
            "file_clip_seq": self.headers.get("X-SDS-Clipboard-Seq"),
            "file_clip_epoch": self.headers.get("X-SDS-Clipboard-Epoch"),
            "transfer_encoding": self.headers.get("Transfer-Encoding"),
            "status": None,
        }
        with st.cond:
            st.log.append(self.log_entry)
            limits = dict(st.limits)

        # 4.1 step 1-2: only the paired client certificate
        if self.connection.getpeercert(binary_form=True) != self.server.client_der:
            return self.error(401, "forbidden", "unknown client", close=True)
        if not path.startswith("/sds/v1/"):
            return self.error(404, "not_found")
        if self.headers.get("Transfer-Encoding"):
            return self.error(400, "bad_request", "chunked bodies are not accepted", close=True)
        # 4.3: body cap before buffering
        if length > (12 * 1024 * 1024 if path == "/sds/v1/file-clipboard" else limits["chunk_max"] + 65536):
            return self.error(413, "too_large", close=True)
        body = self.rfile.read(length) if length else b""

        with st.cond:
            fault = st.take_fault(method, path, ("status",))
            delay_fault = st.take_fault(method, path, ("delay",))
            feature_enabled = st.feature_enabled
            stream_active = st.stream_active
        if delay_fault:
            time.sleep(delay_fault.get("ms", 100) / 1000.0)
        if fault:
            headers = {"Retry-After": fault["retry_after"]} if "retry_after" in fault else None
            return self.error(fault["status"], fault.get("error", "internal"), headers=headers)

        if not feature_enabled:
            return self.error(404, "not_found")
        if not stream_active:
            return self.error(409, "no_active_stream")

        parts = path.split("/")[3:]  # after /sds/v1
        try:
            if parts == ["capabilities"] and method == "GET":
                return self.capabilities()
            if parts == ["events"] and method == "GET":
                return self.events()
            if parts == ["clipboard"] and method == "PUT":
                return self.clipboard_put(body)
            if parts == ["clipboard"] and method == "GET":
                return self.clipboard_get()
            if parts == ["file-clipboard"] and method in ("GET", "PUT"):
                return self.file_clipboard(method, body)
            if parts == ["clipboard-offer"] and method == "PUT":
                return self.lazy_offer_put(body)
            if len(parts) == 4 and parts[0] == "clipboard-offer" and parts[2] == "entries" and method == "POST":
                return self.lazy_offer_entry_start(parts[1], parts[3])
            if len(parts) == 4 and parts[0] == "clipboard-offer" and parts[2] == "entries" and method == "PUT":
                return self.lazy_offer_entry_chunk(parts[1], parts[3], body)
            if len(parts) == 4 and parts[0] == "clipboard-offer" and parts[2] == "entries" and method == "GET":
                return self.lazy_offer_entry_read(parts[1], parts[3])
            if len(parts) == 3 and parts[0] == "clipboard-offer" and parts[2] == "complete" and method == "POST":
                return self.lazy_offer_complete(parts[1], body)
            if len(parts) == 3 and parts[0] == "clipboard-offer" and parts[2] == "cancel" and method == "POST":
                return self.lazy_offer_cancel(parts[1])
            if parts == ["drops"] and method == "POST":
                return self.drop_create(body)
            if len(parts) == 2 and parts[0] == "drops" and method == "PUT":
                return self.drop_chunk(parts[1], body)
            if len(parts) == 3 and parts[0] == "drops" and parts[2] == "complete" and method == "POST":
                return self.drop_complete(parts[1], body)
            if len(parts) == 2 and parts[0] == "drops" and method == "DELETE":
                return self.drop_cancel(parts[1])
            if parts == ["files", "uploads"] and method == "POST":
                return self.upload_create(body)
            if len(parts) == 3 and parts[:2] == ["files", "uploads"] and method == "PUT":
                return self.upload_chunk(parts[2], body)
            if len(parts) == 4 and parts[:2] == ["files", "uploads"] and parts[3] == "complete" and method == "POST":
                return self.upload_complete(parts[2])
            if len(parts) == 3 and parts[:2] == ["files", "uploads"] and method == "DELETE":
                return self.upload_delete(parts[2])
            if parts == ["files", "outbox"] and method == "GET":
                return self.outbox_list()
            if len(parts) == 3 and parts[:2] == ["files", "outbox"] and method == "GET":
                return self.outbox_read(parts[2])
        except (ValueError, KeyError) as e:
            return self.error(400, "bad_request", str(e))
        return self.error(404, "not_found")

    def require(self, permission):
        with self.server.state.cond:
            ok = permission in self.server.state.permissions
        if not ok:
            self.error(403, "forbidden", f"missing permission {permission}")
        return ok

    # ---- endpoints ---------------------------------------------------------------------

    def file_clipboard(self, method, body):
        st = self.server.state
        with st.cond:
            while method == "GET" and st.file_clip_block:
                st.cond.wait(0.2)
            read = "clipboard_read" in st.permissions and "file_download" in st.permissions
            write = "clipboard_write" in st.permissions and "file_upload" in st.permissions
            headers = {"X-SDS-Clipboard-Seq": st.file_clip_seq, "X-SDS-Clipboard-Epoch": st.file_clip_epoch}
            baseline = method == "GET" and "after" not in self.query
            if not (read or write) or (method == "PUT" and not write):
                return self.error(403, "forbidden")
            if not baseline and self.headers.get("X-SDS-Clipboard-Epoch") != st.file_clip_epoch:
                return self.error(409, "conflict", headers=headers)
            if method == "PUT":
                if self.headers.get("X-SDS-Clipboard-Seq") != str(st.file_clip_seq):
                    return self.error(409, "conflict", headers=headers)
                st.file_clip_puts.append(json.loads(body))
                st.file_clip_seq += 1
                st.file_clip = None
                headers["X-SDS-Clipboard-Seq"] = st.file_clip_seq
                return self.send(204, headers=headers)
            if baseline or not read or not st.file_clip or int(self.query["after"]) >= st.file_clip_seq:
                stale_200 = (not baseline) and st.take_fault("GET", "/sds/v1/file-clipboard", ("stale_file_clip_200",))
                if stale_200:
                    payload = st.file_clip or {"files": [{"name": "stale.txt", "data": "c3RhbGU="}]}
                    return self.json_resp(200, payload, headers=headers)
                return self.send(204, headers=headers)
            return self.json_resp(200, st.file_clip, headers=headers)

    def capabilities(self):
        st = self.server.state
        with st.cond:
            obj = {
                "version": st.version,
                "features": list(st.features),
                "permissions": list(st.permissions),
                "limits": dict(st.limits),
                "stream_active": st.stream_active,
            }
        self.json_resp(200, obj)

    def lazy_offer_put(self, body):
        if not self.require("clipboard_write") or not self.require("file_upload"):
            return
        req = json.loads(body)
        offer_id = req["offer_id"]
        st = self.server.state
        with st.cond:
            st.client_offers[offer_id] = {
                "kind": req["kind"],
                "mime": req.get("mime", ""),
                "entries": {e["id"]: {"name": e["name"], "size": int(e["size"]), "started": False, "data": bytearray()} for e in req["entries"]},
            }
        self.json_resp(200, {"offer_id": offer_id, "generation": 1})

    def lazy_offer_entry_start(self, offer_id, entry_id):
        if not self.require("clipboard_write") or not self.require("file_upload"):
            return
        st = self.server.state
        with st.cond:
            off = st.client_offers.get(offer_id)
            if not off or entry_id not in off["entries"]:
                return self.error(404, "not_found")
            ent = off["entries"][entry_id]
            already = ent["started"]
            ent["started"] = True
            chunk_max = st.limits["chunk_max"]
            offset = len(ent["data"])
        self.json_resp(200 if already else 201, {"offset": offset, "chunk_max": chunk_max})

    def lazy_offer_entry_chunk(self, offer_id, entry_id, body):
        if not self.require("clipboard_write") or not self.require("file_upload"):
            return
        offset = int(self.query["offset"])
        st = self.server.state
        with st.cond:
            off = st.client_offers.get(offer_id)
            if not off or entry_id not in off["entries"]:
                return self.error(404, "not_found")
            ent = off["entries"][entry_id]
            if not ent["started"] or offset != len(ent["data"]):
                return self.json_resp(409, {"error": "conflict", "expected_offset": len(ent["data"])})
            if len(ent["data"]) + len(body) > ent["size"]:
                return self.error(413, "too_large")
            ent["data"].extend(body)
            new_off = len(ent["data"])
        self.json_resp(200, {"offset": new_off})

    def lazy_offer_complete(self, offer_id, body):
        if not self.require("clipboard_write") or not self.require("file_upload"):
            return
        req = json.loads(body) if body else {}
        hashes = {e["id"]: e["sha256"].lower() for e in req.get("entries", [])}
        st = self.server.state
        with st.cond:
            off = st.client_offers.pop(offer_id, None)
            if not off:
                return self.error(404, "not_found")
            saved = {}
            for eid, ent in off["entries"].items():
                if len(ent["data"]) != ent["size"]:
                    return self.error(422, "mismatch")
                digest = hashlib.sha256(ent["data"]).hexdigest()
                if eid in hashes and hashes[eid] != digest:
                    return self.error(422, "mismatch")
                saved[ent["name"]] = bytes(ent["data"])
            st.completed_offers[offer_id] = {k: base64.b64encode(v).decode() for k, v in saved.items()}
        self.json_resp(200, {"offer_id": offer_id, "ready": True})

    def lazy_offer_cancel(self, offer_id):
        st = self.server.state
        with st.cond:
            st.client_offers.pop(offer_id, None)
        self.json_resp(200, {"cancelled": True})

    def lazy_offer_entry_read(self, offer_id, entry_id):
        if not self.require("clipboard_read") or not self.require("file_download"):
            return
        offset = int(self.query.get("offset", "0"))
        length = int(self.query.get("length", str(self.server.state.limits["chunk_max"])))
        st = self.server.state
        with st.cond:
            off = st.host_offers.get(offer_id)
            chunk_max = st.limits["chunk_max"]
        if not off or entry_id not in off["entries"]:
            return self.error(404, "not_found")
        data_all = off["entries"][entry_id]["data"]
        if length < 0 or length > chunk_max or offset > len(data_all):
            return self.error(400, "bad_request")
        chunk = data_all[offset:offset + min(length, len(data_all) - offset)]
        headers = {
            "X-SDS-Chunk-Sha256": hashlib.sha256(chunk).hexdigest(),
            "X-SDS-File-Size": str(len(data_all)),
        }
        if offset + len(chunk) == len(data_all):
            headers["X-SDS-File-Sha256"] = hashlib.sha256(data_all).hexdigest()
        self.send(200, chunk, ctype="application/octet-stream", headers=headers)

    def drop_create(self, body):
        if not self.require("file_upload"):
            return
        req = json.loads(body)
        nx, ny = float(req["nx"]), float(req["ny"])
        st = self.server.state
        with st.cond:
            st.drop_counter += 1
            did = f"drop_{st.drop_counter}"
            entries = []
            for i, f in enumerate(req["files"]):
                entries.append({"index": i, "name": f["name"], "size": int(f["size"]), "data": bytearray()})
            st.drops[did] = {"nx": nx, "ny": ny, "entries": entries}
            chunk_max = st.limits["chunk_max"]
        self.json_resp(201, {"id": did, "chunk_max": chunk_max, "files": req["files"]})

    def drop_chunk(self, drop_id, body):
        if not self.require("file_upload"):
            return
        offset = int(self.query["offset"])
        index = int(self.query.get("index", "0"))
        st = self.server.state
        with st.cond:
            d = st.drops.get(drop_id)
            if not d or index < 0 or index >= len(d["entries"]):
                return self.error(404, "not_found")
            ent = d["entries"][index]
            if offset != len(ent["data"]):
                return self.json_resp(409, {"error": "conflict", "expected_offset": len(ent["data"])})
            if len(ent["data"]) + len(body) > ent["size"]:
                return self.error(413, "too_large")
            ent["data"].extend(body)
            new_off = len(ent["data"])
        self.json_resp(200, {"offset": new_off})

    def drop_complete(self, drop_id, body):
        if not self.require("file_upload"):
            return
        req = json.loads(body) if body else {}
        hashes = {int(e["index"]): e["sha256"].lower() for e in req.get("entries", [])}
        st = self.server.state
        with st.cond:
            d = st.drops.pop(drop_id, None)
            if not d:
                return self.error(404, "not_found")
            files_out = []
            for i, ent in enumerate(d["entries"]):
                if len(ent["data"]) != ent["size"]:
                    return self.error(422, "mismatch")
                if i in hashes and hashes[i] != hashlib.sha256(ent["data"]).hexdigest():
                    return self.error(422, "mismatch")
                files_out.append({"name": ent["name"], "size": ent["size"], "data_b64": base64.b64encode(ent["data"]).decode()})
            delivered = st.drop_delivered
            st.completed_drops.append({"nx": d["nx"], "ny": d["ny"], "delivered": delivered, "files": files_out})
        self.json_resp(200, {"delivered": delivered, "files": [f["name"] for f in files_out]})

    def drop_cancel(self, drop_id):
        st = self.server.state
        with st.cond:
            st.drops.pop(drop_id, None)
        self.json_resp(200, {"cancelled": True})

    def events(self):
        st = self.server.state
        after = int(self.query.get("after", "0"))
        timeout = min(30, max(0, int(self.query.get("timeout", "25"))))
        deadline = time.monotonic() + timeout
        with st.cond:
            spin = st.take_fault("GET", "/sds/v1/events", ("spin_event",))
            if spin:
                return self.json_resp(200, {"events": [{"id": after + 1, "type": "future_feature"}], "last_id": 0})
            while True:
                evs = [e for e in st.events if e["id"] > after]
                remaining = deadline - time.monotonic()
                if evs or remaining <= 0:
                    break
                st.cond.wait(remaining)
            last_id = st.next_event_id - 1
        self.json_resp(200, {"events": evs, "last_id": last_id})

    def clipboard_put(self, body):
        if not self.require("clipboard_write"):
            return
        st = self.server.state
        ctype = (self.headers.get("Content-Type") or "").replace(" ", "").lower()
        with st.cond:
            limits = dict(st.limits)
        if ctype == "text/plain;charset=utf-8":
            if len(body) > limits["clipboard_text_max"]:
                return self.error(413, "too_large")
            if not is_valid_utf8_text(body):
                return self.error(400, "bad_request", "invalid text")
            kind = "text/plain"
        elif ctype == "image/png":
            if len(body) > limits["clipboard_image_max"]:
                return self.error(413, "too_large")
            if not body.startswith(PNG_SIGNATURE):
                return self.error(400, "bad_request", "invalid png")
            kind = "image/png"
        else:
            return self.error(415, "unsupported_type")
        with st.cond:
            st.clip_seq += 1
            st.clip = {kind: body}
            st.client_puts.append({
                "type": kind,
                "client_seq": self.headers.get("X-SDS-Client-Seq"),
                "data_b64": base64.b64encode(body).decode(),
                "content_length": int(self.headers.get("Content-Length") or 0),
            })
            seq = st.clip_seq
        self.send(204, headers={"X-SDS-Host-Seq": seq})

    def clipboard_get(self):
        if not self.require("clipboard_read"):
            return
        st = self.server.state
        seq = int(self.query.get("seq", "0"))
        kind = self.query.get("type", "")
        with st.cond:
            fault = st.take_fault("GET", "/sds/v1/clipboard", ("clip_410",))
            current = st.clip_seq
            data = st.clip.get(kind)
        if fault or seq != current:
            return self.error(410, "conflict", "clipboard changed")
        if data is None:
            return self.error(404, "not_found")
        self.send(200, data, ctype=kind, headers={"X-SDS-Host-Seq": current})

    def upload_create(self, body):
        if not self.require("file_upload"):
            return
        st = self.server.state
        req = json.loads(body)
        name, size, sha = req["name"], int(req["size"]), req["sha256"]
        with st.cond:
            if size > st.limits["file_max"]:
                return self.error(413, "too_large")
            if len(st.uploads) >= st.limits["uploads_max_concurrent"]:
                return self.error(429, "rate_limited", headers={"Retry-After": 1})
            upload_id = secrets.token_hex(16)
            st.uploads[upload_id] = {"name": name, "size": size, "sha256": sha, "data": bytearray(), "last_chunk": 0}
            chunk = st.limits["chunk_max"]
        self.json_resp(201, {"id": upload_id, "chunk_max": chunk, "offset": 0})

    def upload_chunk(self, upload_id, body):
        if not self.require("file_upload"):
            return
        st = self.server.state
        offset = int(self.query["offset"])
        with st.cond:
            up = st.uploads.get(upload_id)
            if up is None:
                return self.error(404, "not_found")
            rewind = st.take_fault("PUT", "/sds/v1/files/uploads/", ("rewind",))
            if rewind and up["last_chunk"]:
                # Pretend the previous chunk was lost
                del up["data"][len(up["data"]) - up["last_chunk"]:]
                up["last_chunk"] = 0
                return self.error(409, "conflict", extra={"offset": len(up["data"])})
            if offset != len(up["data"]):
                return self.error(409, "conflict", extra={"offset": len(up["data"])})
            if len(body) > st.limits["chunk_max"]:
                return self.error(413, "too_large")
            if len(up["data"]) + len(body) > up["size"]:
                return self.error(400, "bad_request", "beyond declared size")
            up["data"] += body
            up["last_chunk"] = len(body)
            new_offset = len(up["data"])
            drop = st.take_fault("PUT", "/sds/v1/files/uploads/", ("drop_after_accept",))
        if drop:
            # Data accepted, response lost: the client must resynchronize via 409
            self.log_entry["status"] = "dropped"
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            return
        self.json_resp(200, {"offset": new_offset})

    def upload_complete(self, upload_id):
        if not self.require("file_upload"):
            return
        st = self.server.state
        with st.cond:
            up = st.uploads.pop(upload_id, None)
            if up is None:
                return self.error(404, "not_found")
            fault = st.take_fault("POST", "/sds/v1/files/uploads/", ("verify_fail",))
            data = bytes(up["data"])
            if fault or len(data) != up["size"] or hashlib.sha256(data).hexdigest() != up["sha256"]:
                return self.error(422, "bad_request", "verification failed")
            name = final_name(st, up["name"])
            st.completed.append({
                "name": name,
                "requested_name": up["name"],
                "size": len(data),
                "sha256": up["sha256"],
                "data_b64": base64.b64encode(data).decode(),
            })
        self.json_resp(201, {"name": name, "size": len(data)})

    def upload_delete(self, upload_id):
        st = self.server.state
        with st.cond:
            st.uploads.pop(upload_id, None)
        self.send(204)

    def outbox_list(self):
        if not self.require("file_download"):
            return
        st = self.server.state
        with st.cond:
            files = [{"id": fid, "name": f["name"], "size": f["size"], "mtime": f["mtime"]}
                     for fid, f in st.outbox.items()]
        self.json_resp(200, {"files": files})

    def outbox_read(self, file_id):
        if not self.require("file_download"):
            return
        st = self.server.state
        offset = int(self.query.get("offset", "0"))
        length = int(self.query.get("length", "0"))
        with st.cond:
            f = st.outbox.get(file_id)
            chunk_max = st.limits["chunk_max"]
            drop = st.take_fault("GET", "/sds/v1/files/outbox/", ("drop_range",)) if f else None
        if f is None:
            return self.error(404, "not_found")
        if length > chunk_max or length < 0:
            return self.error(413, "too_large")
        if offset > len(f["data"]):
            return self.error(416, "bad_request")
        data = f["data"][offset:offset + length]
        if f.get("extra_bytes"):
            data += b"X" * f["extra_bytes"]
        headers = {"X-SDS-File-Size": f.get("header_size", f["size"])}
        if offset == 0 and not f.get("no_sha"):
            sha = hashlib.sha256(f["data"]).hexdigest()
            if f.get("bad_sha"):
                sha = hashlib.sha256(b"not the file").hexdigest()
            headers["X-SDS-File-Sha256"] = sha
        if drop:
            # Promise the whole range, send half of it, then drop the connection
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            for k, v in headers.items():
                self.send_header(k, str(v))
            self.end_headers()
            self.wfile.write(data[:len(data) // 2])
            self.wfile.flush()
            self.log_entry["status"] = "dropped"
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            return
        self.send(200, data, ctype="application/octet-stream", headers=headers)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, ctx, state, client_der):
        super().__init__(addr, Handler)
        self.ctx = ctx
        self.state = state
        self.client_der = client_der

    def get_request(self):
        sock, addr = self.socket.accept()
        # Handshake happens in the handler thread (Handler.setup)
        return self.ctx.wrap_socket(sock, server_side=True, do_handshake_on_connect=False), addr

    def handle_error(self, request, client_address):
        # Handshake failures and client aborts are expected in tests
        pass


def handle_command(state, cmd):
    c = cmd.get("cmd")
    with state.cond:
        if c == "config":
            for key in ("permissions", "stream_active", "feature_enabled", "version", "file_clip_block", "features", "drop_delivered"):
                if key in cmd:
                    setattr(state, key, cmd[key])
            if "limits" in cmd:
                state.limits.update(cmd["limits"])
            state.cond.notify_all()
            return {"ok": True}
        if c == "host_lazy_offer":
            offer_id = cmd["offer_id"]
            kind = cmd["kind"]
            mime = cmd.get("mime", "")
            entries = {}
            entries_ev = []
            for e in cmd["entries"]:
                raw = base64.b64decode(e.get("data_b64", ""))
                entries[e["id"]] = {"name": e["name"], "size": len(raw), "data": raw}
                entries_ev.append({"id": e["id"], "name": e["name"], "size": len(raw)})
            state.host_offers[offer_id] = {"kind": kind, "mime": mime, "entries": entries}
            ev = {"type": "clipboard_offer", "offer_id": offer_id, "kind": kind, "entries": entries_ev}
            if mime:
                ev["mime"] = mime
            eid = state.add_event(ev)
            return {"ok": True, "event_id": eid}
        if c == "emit_pull":
            eid = state.add_event({"type": "clipboard_pull", "offer_id": cmd["offer_id"], "targets": cmd.get("targets", [])})
            return {"ok": True, "event_id": eid}
        if c == "host_clipboard":
            clip = {}
            if "text" in cmd:
                clip["text/plain"] = cmd["text"].encode("utf-8")
            if "text_b64" in cmd:
                clip["text/plain"] = base64.b64decode(cmd["text_b64"])
            if "png_b64" in cmd:
                clip["image/png"] = base64.b64decode(cmd["png_b64"])
            state.clip_seq += 1
            state.clip = clip
            sizes = {k: len(v) for k, v in clip.items()}
            sizes.update(cmd.get("announce_sizes", {}))
            eid = state.add_event({"type": "clipboard", "seq": state.clip_seq,
                                   "types": sorted(clip.keys()), "sizes": sizes})
            return {"ok": True, "seq": state.clip_seq, "event_id": eid}
        if c == "file_clipboard":
            state.file_clip_seq += 1
            state.file_clip = cmd["bundle"]
            if cmd.get("new_epoch"):
                state.file_clip_epoch = secrets.token_hex(16)
            return {"ok": True, "seq": state.file_clip_seq}
        if c == "event":
            if "events" in cmd:
                ids = [state.add_event(e) for e in cmd["events"]]
                return {"ok": True, "event_ids": ids}
            return {"ok": True, "event_id": state.add_event(cmd["event"])}
        if c == "outbox_add":
            data = base64.b64decode(cmd.get("data_b64", ""))
            state.outbox_counter += 1
            mtime = int(time.time())
            msg = f"{cmd['name']}|{len(data)}|{mtime}|{state.outbox_counter}".encode()
            fid = cmd.get("id") or hmac.new(state.outbox_key, msg, hashlib.sha256).hexdigest()[:32]
            state.outbox[fid] = {
                "name": cmd["name"], "data": data, "size": cmd.get("size_override", len(data)),
                "mtime": mtime, "bad_sha": cmd.get("bad_sha", False), "no_sha": cmd.get("no_sha", False),
                "extra_bytes": cmd.get("extra_bytes", 0),
            }
            if "header_size" in cmd:
                state.outbox[fid]["header_size"] = cmd["header_size"]
            if cmd.get("announce", True):
                state.add_event({"type": "outbox", "count": len(state.outbox)})
            return {"ok": True, "id": fid}
        if c == "outbox_clear":
            state.outbox.clear()
            return {"ok": True}
        if c == "reset_events":
            # Simulates a host that restarted its event numbering
            state.events.clear()
            state.next_event_id = 1
            return {"ok": True}
        if c == "end_stream":
            state.stream_active = False
            state.add_event({"type": "stream_ended"})
            return {"ok": True}
        if c == "fault":
            f = dict(cmd)
            f.pop("cmd")
            state.faults.append(f)
            return {"ok": True}
        if c == "log":
            return {"ok": True, "log": list(state.log), "handshake_failures": state.handshake_failures}
        if c == "clear_log":
            state.log.clear()
            return {"ok": True}
        if c == "state":
            return {
                "ok": True,
                "client_puts": list(state.client_puts),
                "completed": list(state.completed),
                "uploads": {k: len(v["data"]) for k, v in state.uploads.items()},
                "clip_seq": state.clip_seq,
                "file_clip_puts": list(state.file_clip_puts),
                "client_offers": list(state.client_offers.keys()),
                "completed_offers": dict(state.completed_offers),
                "completed_drops": list(state.completed_drops),
            }
    if c == "quit":
        return {"ok": True, "quit": True}
    return {"ok": False, "error": f"unknown command {c}"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cert", required=True, help="server certificate (PEM)")
    parser.add_argument("--key", required=True, help="server private key (PEM)")
    parser.add_argument("--client-cert", required=True, help="the paired client certificate (PEM)")
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(args.cert, args.key)
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_verify_locations(cafile=args.client_cert)
    if hasattr(ssl, "VERIFY_X509_STRICT"):
        ctx.verify_flags &= ~ssl.VERIFY_X509_STRICT

    with open(args.client_cert, "rb") as f:
        client_der = ssl.PEM_cert_to_DER_cert(f.read().decode())

    state = State()
    server = Server(("127.0.0.1", args.port), ctx, state, client_der)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    print(json.dumps({"ready": True, "port": server.server_address[1]}), flush=True)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            reply = handle_command(state, json.loads(line))
        except Exception as e:  # report and keep serving
            reply = {"ok": False, "error": repr(e)}
        print(json.dumps(reply), flush=True)
        if reply.get("quit"):
            break

    server.shutdown()


if __name__ == "__main__":
    main()
