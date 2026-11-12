-- benchd.lua — Wireshark / tshark dissector for the benchd protocol (§6).
--
--   wireshark -X lua_script:tools/benchd.lua captures/session.pcap
--   tshark   -X lua_script:tools/benchd.lua -r captures/session.pcap -Y benchd
--
-- Every packet: magic 'BN' (0x424E) | ver | type | len (u32) | crc32 (u32) | payload.
-- One TCP segment may hold several packets and a packet may span segments: both handled
-- with Wireshark's TCP desegmentation. The CRC32 is recomputed and checked.

local benchd = Proto("benchd", "benchd remote serial console")

local types = {
    [0x01] = "HELLO", [0x02] = "INPUT", [0x03] = "TAKE", [0x04] = "RELEASE",
    [0x05] = "INFO_REQ", [0x06] = "BAUD", [0x07] = "PING", [0x08] = "BYE",
    [0x81] = "WELCOME", [0x82] = "DATA", [0x83] = "EVENT", [0x84] = "REPLAY_BEGIN",
    [0x85] = "REPLAY_END", [0x86] = "CTRL_RESULT", [0x87] = "INFO", [0x88] = "STATUS",
    [0x89] = "ERROR", [0x8A] = "PONG",
}
local errors = {
    [1] = "BAD_MAGIC", [2] = "BAD_VERSION", [3] = "BAD_LENGTH", [4] = "BAD_CRC", [5] = "BAD_TYPE",
    [6] = "NOT_CONTROLLER", [7] = "DEVICE_LOST", [8] = "SERVER_BUSY", [9] = "HANDSHAKE_TIMEOUT",
    [10] = "IDLE_TIMEOUT", [11] = "SERVER_SHUTDOWN", [12] = "BAD_REQUEST",
}
local events = {
    [1] = "DEVICE_LOST", [2] = "DEVICE_BACK", [3] = "CTRL_TAKEN", [4] = "CTRL_RELEASED",
    [5] = "BAUD_CHANGED", [6] = "DROPPED", [7] = "SERVER_SHUTDOWN",
}
local devstate = { [0] = "LOST", [1] = "UP" }

local f = benchd.fields
f.magic    = ProtoField.uint16("benchd.magic", "Magic", base.HEX)
f.ver      = ProtoField.uint8("benchd.ver", "Version")
f.type     = ProtoField.uint8("benchd.type", "Type", base.HEX, types)
f.len      = ProtoField.uint32("benchd.len", "Payload length")
f.crc      = ProtoField.uint32("benchd.crc32", "CRC32", base.HEX)
f.crc_ok   = ProtoField.bool("benchd.crc_ok", "CRC32 valid")
f.name     = ProtoField.string("benchd.name", "Name")
f.replay   = ProtoField.uint32("benchd.replay_bytes", "Replay bytes")
f.resume   = ProtoField.uint8("benchd.has_resume", "Has resume")
f.resoff   = ProtoField.uint64("benchd.resume_offset", "Resume offset")
f.laststart= ProtoField.uint64("benchd.last_server_start_ns", "Last server start (ns)")
f.data     = ProtoField.string("benchd.data", "Data")
f.baud     = ProtoField.uint32("benchd.baud", "Baud")
f.cts      = ProtoField.uint64("benchd.client_ts_ns", "Client timestamp (ns)")
f.sts      = ProtoField.uint64("benchd.server_ts_ns", "Server timestamp (ns)")
f.devstate = ProtoField.uint8("benchd.dev_state", "Device state", base.DEC, devstate)
f.viewers  = ProtoField.uint32("benchd.viewers", "Viewers")
f.histcap  = ProtoField.uint32("benchd.history_cap", "History capacity")
f.head     = ProtoField.uint64("benchd.head", "Ring head offset")
f.start    = ProtoField.uint64("benchd.server_start_ns", "Server start (ns)")
f.devpath  = ProtoField.string("benchd.dev_path", "Device path")
f.ctrlname = ProtoField.string("benchd.ctrl_name", "Controller")
f.sver     = ProtoField.string("benchd.server_version", "Server version")
f.offset   = ProtoField.uint64("benchd.offset", "Stream offset (end of record)")
f.ts       = ProtoField.absolute_time("benchd.ts", "Server timestamp", base.LOCAL)
f.evcode   = ProtoField.uint16("benchd.event", "Event", base.DEC, events)
f.text     = ProtoField.string("benchd.text", "Text")
f.from     = ProtoField.uint64("benchd.from_offset", "From offset")
f.dropped  = ProtoField.uint64("benchd.dropped_bytes", "Dropped bytes")
f.granted  = ProtoField.bool("benchd.granted", "Granted")
f.idle     = ProtoField.uint32("benchd.idle_sec", "Holder idle (s)")
f.autorel  = ProtoField.uint32("benchd.auto_release_in", "Auto-release in (s)")
f.holder   = ProtoField.string("benchd.holder", "Holder")
f.youctrl  = ProtoField.bool("benchd.you_are_ctrl", "You are controller")
f.rx       = ProtoField.uint64("benchd.rx_bytes", "Device rx bytes")
f.errcode  = ProtoField.uint16("benchd.error", "Error", base.DEC, errors)

-- CRC32 (IEEE) for validation
local crc_table = {}
for i = 0, 255 do
    local c = i
    for _ = 1, 8 do
        if c % 2 == 1 then c = (c >> 1) ~ 0xEDB88320 else c = c >> 1 end
    end
    crc_table[i] = c
end
local function crc32(tvbr)
    local c = 0xFFFFFFFF
    local bytes = tvbr:bytes()
    for i = 0, bytes:len() - 1 do
        c = crc_table[(c ~ bytes:get_index(i)) & 0xFF] ~ (c >> 8)
    end
    return c ~ 0xFFFFFFFF
end

local function ns_time(tvbr)
    local ns = tvbr:uint64()
    local secs = (ns / 1000000000):tonumber()
    local nsec = (ns % 1000000000):tonumber()
    return NSTime.new(secs, nsec)
end

local function printable(s, max)
    s = s:gsub("\r", "\\r"):gsub("\n", "\\n"):gsub("[%c]", ".")
    if #s > max then s = s:sub(1, max) .. "…" end
    return s
end

-- decode one payload; returns a short summary for the Info column
local function payload(t, buf, off, len, tree)
    local p = function(o, n) return buf(off + o, n) end
    if t == 0x01 and len >= 56 then
        tree:add(f.name, p(0, 32)); tree:add(f.replay, p(32, 4)); tree:add(f.resume, p(36, 1))
        tree:add(f.resoff, p(40, 8)); tree:add(f.laststart, p(48, 8))
        return "name=" .. p(0, 32):stringz()
    elseif t == 0x02 then
        if len > 0 then tree:add(f.data, p(0, len)) end
        return len > 0 and ('"' .. printable(p(0, len):string(), 30) .. '"') or ""
    elseif t == 0x06 and len >= 4 then
        tree:add(f.baud, p(0, 4)); return tostring(p(0, 4):uint())
    elseif t == 0x07 and len >= 8 then
        tree:add(f.cts, p(0, 8))
    elseif t == 0x81 and len >= 208 then
        tree:add(f.ver, p(0, 1)); tree:add(f.devstate, p(1, 1)); tree:add(f.baud, p(4, 4))
        tree:add(f.viewers, p(8, 4)); tree:add(f.histcap, p(12, 4)); tree:add(f.head, p(16, 8))
        tree:add(f.start, p(24, 8)); tree:add(f.devpath, p(32, 128)); tree:add(f.ctrlname, p(160, 32))
        tree:add(f.sver, p(192, 16))
        return "dev=" .. p(32, 128):stringz() .. " viewers=" .. p(8, 4):uint()
    elseif t == 0x82 and len >= 16 then
        tree:add(f.offset, p(0, 8)); tree:add(f.ts, p(8, 8), ns_time(p(8, 8)))
        if len > 16 then tree:add(f.data, p(16, len - 16)) end
        return len > 16 and ('"' .. printable(p(16, len - 16):string(), 40) .. '"') or ""
    elseif t == 0x83 and len >= 20 then
        tree:add(f.offset, p(0, 8)); tree:add(f.ts, p(8, 8), ns_time(p(8, 8)))
        tree:add(f.evcode, p(16, 2))
        if len > 20 then tree:add(f.text, p(20, len - 20)) end
        return (events[p(16, 2):uint()] or "?") .. (len > 20 and (": " .. p(20, len - 20):string()) or "")
    elseif t == 0x84 and len >= 16 then
        tree:add(f.from, p(0, 8)); tree:add(f.dropped, p(8, 8))
    elseif t == 0x85 and len >= 8 then
        tree:add(f.head, p(0, 8))
    elseif t == 0x86 and len >= 44 then
        tree:add(f.granted, p(0, 1)); tree:add(f.idle, p(4, 4)); tree:add(f.autorel, p(8, 4))
        tree:add(f.holder, p(12, 32))
        return (p(0, 1):uint() == 1 and "granted" or "denied") .. " holder=" .. p(12, 32):stringz()
    elseif t == 0x87 then
        if len > 0 then tree:add(f.text, p(0, len)) end
        return len .. " bytes of key=value"
    elseif t == 0x88 and len >= 56 then
        tree:add(f.devstate, p(0, 1)); tree:add(f.youctrl, p(1, 1)); tree:add(f.baud, p(4, 4))
        tree:add(f.viewers, p(8, 4)); tree:add(f.idle, p(12, 4)); tree:add(f.rx, p(16, 8))
        tree:add(f.ctrlname, p(24, 32))
        return "viewers=" .. p(8, 4):uint() .. " ctrl=" .. p(24, 32):stringz()
    elseif t == 0x89 and len >= 4 then
        tree:add(f.errcode, p(0, 2))
        if len > 4 then tree:add(f.text, p(4, len - 4)) end
        return errors[p(0, 2):uint()] or "?"
    elseif t == 0x8A and len >= 16 then
        tree:add(f.cts, p(0, 8)); tree:add(f.sts, p(8, 8))
    end
    return ""
end

function benchd.dissector(buf, pinfo, root)
    local off, total, info = 0, buf:len(), {}
    while off < total do
        local avail = total - off
        if avail < 12 then
            pinfo.desegment_offset = off
            pinfo.desegment_len = DESEGMENT_ONE_MORE_SEGMENT
            break
        end
        if buf(off, 2):uint() ~= 0x424E then
            root:add(benchd, buf(off)):append_text(" [not benchd framing]")
            break
        end
        local len = buf(off + 4, 4):uint()
        if len > 65536 then
            root:add(benchd, buf(off, 12)):append_text(" [length " .. len .. " exceeds limit]")
            break
        end
        if avail < 12 + len then
            pinfo.desegment_offset = off
            pinfo.desegment_len = 12 + len - avail
            break
        end
        local t = buf(off + 3, 1):uint()
        local name = types[t] or string.format("0x%02x", t)
        local tree = root:add(benchd, buf(off, 12 + len), "benchd " .. name)
        tree:add(f.magic, buf(off, 2)); tree:add(f.ver, buf(off + 2, 1)); tree:add(f.type, buf(off + 3, 1))
        tree:add(f.len, buf(off + 4, 4))
        local crc_item = tree:add(f.crc, buf(off + 8, 4))
        local ok = (len == 0 and buf(off + 8, 4):uint() == 0) or
                   (len > 0 and crc32(buf(off + 12, len)) == buf(off + 8, 4):uint())
        tree:add(f.crc_ok, ok):set_generated()
        if not ok then crc_item:add_expert_info(PI_CHECKSUM, PI_ERROR, "CRC32 mismatch") end
        local summary = len > 0 and payload(t, buf, off + 12, len, tree) or ""
        info[#info + 1] = name .. (summary ~= "" and (" " .. summary) or "")
        off = off + 12 + len
    end
    if #info > 0 then
        pinfo.cols.protocol = "BENCHD"
        pinfo.cols.info = table.concat(info, " | ")
    end
    return off
end

DissectorTable.get("tcp.port"):add(9000, benchd)
