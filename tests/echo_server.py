"""Minimal WebSocket test server (RFC 6455), standard library only (+ cryptography for the
self-signed wss certificate). Used by ws_selftest.exe.

    python echo_server.py [--port 8765] [--tls-port 8766]

Text commands: "close-me" (server closes 4001 "bye"), "big:N" (N-byte text in fragments),
"flood:N" (N small messages), "headers?" (replies "<X-Test>|<protocol>").
Path /reject answers HTTP 403. Anything else is echoed with the same opcode.
"""
import argparse
import asyncio
import base64
import datetime
import hashlib
import os
import ssl
import struct

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
HERE = os.path.dirname(os.path.abspath(__file__))
FRAGMENT = 256 * 1024


def make_cert(cert_path, key_path):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name).issuer_name(name).public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=365))
        .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), critical=False)
        .sign(key, hashes.SHA256())
    )
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))


def frame(opcode, payload, fin=True):
    head = bytearray([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126:
        head.append(n)
    elif n < 65536:
        head.append(126)
        head += struct.pack("!H", n)
    else:
        head.append(127)
        head += struct.pack("!Q", n)
    return bytes(head) + payload


async def read_frame(reader):
    b0, b1 = await reader.readexactly(2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack("!H", await reader.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", await reader.readexactly(8))[0]
    mask = await reader.readexactly(4) if b1 & 0x80 else None
    data = bytearray(await reader.readexactly(n))
    if mask:
        for i in range(n):
            data[i] ^= mask[i & 3]
    return bool(b0 & 0x80), b0 & 0x0F, bytes(data)


async def handle(reader, writer):
    try:
        request = await reader.readuntil(b"\r\n\r\n")
        lines = request.decode("latin-1").split("\r\n")
        path = lines[0].split(" ")[1] if len(lines[0].split(" ")) > 1 else "/"
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        if path.startswith("/reject") or "sec-websocket-key" not in headers:
            writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            await writer.drain()
            return
        accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + GUID).digest())
        protocol = headers.get("sec-websocket-protocol", "").split(",")[0].strip()
        response = (b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    b"Sec-WebSocket-Accept: " + accept + b"\r\n")
        if protocol:
            response += b"Sec-WebSocket-Protocol: " + protocol.encode() + b"\r\n"
        writer.write(response + b"\r\n")
        await writer.drain()

        message, message_op = bytearray(), 0
        while True:
            fin, op, data = await read_frame(reader)
            if op == 0x8:
                writer.write(frame(0x8, data[:2] if len(data) >= 2 else b""))
                await writer.drain()
                return
            if op == 0x9:
                writer.write(frame(0xA, data))
                continue
            if op == 0xA:
                continue
            if op in (0x1, 0x2):
                message, message_op = bytearray(data), op
            else:
                message += data
            if not fin:
                continue
            payload = bytes(message)
            text = payload.decode("utf-8", "replace") if message_op == 0x1 else None
            if text == "close-me":
                writer.write(frame(0x8, struct.pack("!H", 4001) + b"bye"))
                await writer.drain()
                await asyncio.sleep(0.5)
                return
            if text and text.startswith("big:"):
                body = b"a" * int(text[4:])
                chunks = [body[i:i + FRAGMENT] for i in range(0, len(body), FRAGMENT)] or [b""]
                for i, chunk in enumerate(chunks):
                    writer.write(frame(0x1 if i == 0 else 0x0, chunk, fin=i == len(chunks) - 1))
                    await writer.drain()
                continue
            if text and text.startswith("flood:"):
                for i in range(int(text[6:])):
                    writer.write(frame(0x1, ("f%d" % i).encode()))
                await writer.drain()
                continue
            if text == "headers?":
                writer.write(frame(0x1, (headers.get("x-test", "") + "|" + protocol).encode()))
                await writer.drain()
                continue
            writer.write(frame(message_op, payload))
            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        writer.close()


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--tls-port", type=int, default=8766)
    args = parser.parse_args()

    cert_dir = os.path.join(HERE, "certs")
    os.makedirs(cert_dir, exist_ok=True)
    cert, key = os.path.join(cert_dir, "localhost.pem"), os.path.join(cert_dir, "localhost.key")
    if not os.path.exists(cert):
        make_cert(cert, key)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, key)

    plain = await asyncio.start_server(handle, "127.0.0.1", args.port, limit=8 * 1024 * 1024)
    secure = await asyncio.start_server(handle, "127.0.0.1", args.tls_port, ssl=tls, limit=8 * 1024 * 1024)
    print("listening ws://127.0.0.1:%d wss://localhost:%d" % (args.port, args.tls_port), flush=True)
    async with plain, secure:
        await asyncio.gather(plain.serve_forever(), secure.serve_forever())


if __name__ == "__main__":
    asyncio.run(main())
