"""Build the four seek-bar layouts from the original 35-pixel test skin."""

from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parent
KEY = bytes.fromhex("62 4a 99 64 ad 1a 4d ec b3 a0 10 7c 47 4c bc d2")
NAME = "SpectrogramProbe.acs6"


def crypt(data):
    mask = (KEY * ((len(data) + len(KEY) - 1) // len(KEY)))[:len(data)]
    return (int.from_bytes(data, "little") ^ int.from_bytes(mask, "little")).to_bytes(len(data), "little")


def field(text):
    return bytes([len(text)]) + text.encode("utf-16-le")


def layouts():
    original = (ROOT / "SpectrogramProbe.35px.acs6").read_bytes()
    payload = crypt(original[10:])
    marker = struct.pack("<H", len("AIMPMainForm")) + "AIMPMainForm".encode("utf-16-le") + b"\x03"
    assert original.startswith(b"ACS6") and payload.count(marker) == 1
    entry = payload.index(marker) + len(marker)
    length, compression, packed = struct.unpack_from("<IBI", payload, entry)
    assert compression == 1
    start = entry + 9
    form = zlib.decompress(payload[start:start + packed])
    assert len(form) == length
    target = b"\x02" + field("mwSeekBar")
    index = form.index(target) + len(target)
    size = b"\x02" + field("SizeMinimal") + b"\x0b" + field("0,35")
    index = form.index(size, index, index + 100)
    for factor in range(1, 5):
        value = b"\x02" + field("SizeMinimal") + b"\x0b" + field(f"0,{35 * factor}")
        new_form = bytearray(form[:index] + value + form[index + len(size):])
        section_length, = struct.unpack_from("<I", form, 4)
        struct.pack_into("<I", new_form, 4, section_length + len(new_form) - len(form))
        block = zlib.compress(new_form, 9)
        header = struct.pack("<IBI", len(new_form), 1, len(block))
        yield original[:10] + crypt(payload[:entry] + header + block + payload[start + packed:])


def replace_entry(payload, name, kind, modify):
    marker = struct.pack("<H", len(name)) + name.encode("utf-16le") + bytes([kind])
    assert payload.count(marker) == 1
    offset = payload.index(marker) + len(marker)
    length, compression, packed = struct.unpack_from("<IBI", payload, offset)
    start = offset + 9
    data = zlib.decompress(payload[start:start+packed])
    assert compression == 1 and len(data) == length
    updated = modify(data)
    block = zlib.compress(updated, 9)
    return payload[:offset] + struct.pack("<IBI", len(updated), 1, len(block)) + block + payload[start+packed:]

handler = "FollowPlayingTrack"

def bind(data):
    prefix = bytes.fromhex("1c 02 20 1d 03 02 0c") + field("Notificator")
    assert data.count(prefix) == 1
    start = data.index(prefix)
    link = bytes.fromhex("0b 03 01 0c") + field("Bindings") + bytes.fromhex("01 0e 01 03 1f")
    link += field("prvPlayerState") + bytes([32]) + field("OnTrackStarted") + bytes([33]) + field("Handler")
    provider = bytes.fromhex("1d 03 02 0c") + field(handler) + bytes([30]) + field("ScriptableEventHandler")
    provider += bytes.fromhex("01 08 02 02") + link + bytes.fromhex("0b 01 03 0c") + field("Script")
    provider += bytes([13]) + field(handler) + bytes.fromhex("1a 01 31 00")
    updated = bytearray(data[:start+2] + bytes([33]) + provider + data[start+3:])
    struct.pack_into("<I", updated, 4, struct.unpack_from("<I", data, 4)[0] + len(updated) - len(data))
    return updated

def node(tag, attrs="", *children):
    def string(value):
        return struct.pack("<H", len(value)) + value.encode("utf-16le")
    return string(tag) + string(attrs) + struct.pack("<I",len(children)) + b"".join(children)

def script(data):
    # procedure Execute(Sender, Arguments: TComponent); begin
    #   FindObject('acPlayableFileFindInML').Execute;
    # end;
    assert data.startswith(b"FSXMLBIN")
    pos = 'pos="3:3"'
    call = node("callstmt", pos, node("dsgn", pos,
        node("node", 'text="FindObject" pos="3:13"'),
        node("expr", 'pos="3:14" pos1="3:38"',
            node("string", 'text="\'acPlayableFileFindInML\'" pos="3:38"')),
        node("node", 'text="Execute" pos="3:47"')))
    proc = node("procedure", 'pos="1:1"',
        node("name", 'text="Execute" pos="1:18"'),
        node("parameters", 'pos="1:18"', node("var", 'pos="1:19"',
            node("ident", 'text="Sender" pos="1:25"'),
            node("ident", 'text="Arguments" pos="1:36"'),
            node("type", 'text="TComponent" pos="1:48"'))),
        node("compoundstmt", 'pos="2:1"', call))
    return b"FSXMLBIN" + node("program", "", proc, node("compoundstmt",'pos="5:1"'),node("types"))


def with_follow(data):
    payload = bytearray(replace_entry(crypt(data[10:]), "Schema", 3, bind))
    metadata_size, = struct.unpack_from("<I", payload, 17)
    count_offset = 21 + metadata_size
    entries, = struct.unpack_from("<I", payload, count_offset)
    assert payload[count_offset + 4:count_offset + 6] == struct.pack("<H", len("FloatArrow"))
    struct.pack_into("<I", payload, count_offset, entries + 1)
    body = script(b"FSXMLBIN")
    block = zlib.compress(body, 9)
    entry = struct.pack("<H", len(handler)) + handler.encode("utf-16le") + bytes([7])
    return data[:10] + crypt(payload + entry + struct.pack("<IBI", len(body), 1, len(block)) + block)

if __name__ == "__main__":
    for factor, data in enumerate(layouts(), start=1):
        follow = with_follow(data)
        for directory in sys.argv[1:]:
            folder = Path(directory) / "layouts" / str(factor)
            folder.mkdir(parents=True, exist_ok=True)
            (folder / NAME).write_bytes(data)
            (folder / "follow").mkdir(exist_ok=True)
            (folder / "follow" / NAME).write_bytes(follow)
