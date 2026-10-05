-- ============================================================================
-- optiscaler_capture · CET 控制端
--
-- 与 OptiScaler 的 DLSS 输入捕获层通过「文件轮询」通信。
-- CET 是 Lua 环境，OptiScaler 是 C++ DX12 层，两者无法直接调用，
-- 因此在固定目录里用两个文件握手。
--
-- 通信目录（相对游戏 bin\x64）：
--   plugins/cyber_engine_tweaks/mods/optiscaler_capture/
--     ├── command.txt   ← 本脚本写入 START / STOP / STATUS，OptiScaler 读完即删
--     └── status.json   ← OptiScaler 写入，本脚本读取
--
-- 捕获输出目录：
--   plugins/cyber_engine_tweaks/mods/optiscaler_capture/session_<时间戳>/
--
-- 前置条件：
--   1. OptiScaler.ini 的 [Capture] Enabled 设为 true
--   2. [Hotfix] 的 ColorResourceBarrier / MotionVectorResourceBarrier 已按本游戏设好
--
-- ── 关于「控制台命令」（重要）──────────────────────────────────────────────
-- CET 并没有 registerConsoleCommand 这类 API。控制台本质上只是一个 Lua REPL：
--   · Console.cpp      —— 没有命令表，把输入原样交给 ExecuteLua()
--   · ScriptContext.cpp —— 只给 mod 注入 registerForEvent / registerHotkey /
--                          registerInput，且 init.lua 跑完就置 nil
--   · LuaSandbox.cpp   —— 每个 mod 独立沙箱，控制台是独立的 sandbox 0
-- 所以「裸敲 OptiCaptureStatus」在 CET 里永远是语法错误，必须带括号。
--
-- 本 mod 提供三种入口：
--   A) 热键（推荐、可靠）
--      已注册 4 个 id，去 CET 覆盖层 → Bindings 页绑定按键：
--        OptiCaptureStart / OptiCaptureStop / OptiCaptureStatus / OptiCaptureWatch
--   B) 控制台（尽力而为，取决于 CET 版本）
--      尝试把函数注入各沙箱共用的回退表，成功则控制台可直接调用：
--        OptiCaptureStatus()      ← 注意括号
--   C) 完全不用 CET
--      OptiScaler 侧只是轮询 command.txt。直接在该目录新建 command.txt，
--      内容写 START / STOP / STATUS 即可（OptiScaler 读完会删掉）。
-- ============================================================================

local captureDir  = "plugins/cyber_engine_tweaks/mods/optiscaler_capture/"
local commandFile = captureDir .. "command.txt"
local statusFile  = captureDir .. "status.json"

local watchEnabled = false
local watchTimer = 0.0
local WATCH_INTERVAL = 5.0

-- ---------------------------------------------------------------------------
-- 底层：发命令 / 读状态
-- ---------------------------------------------------------------------------
local function sendCommand(cmd)
    local f = io.open(commandFile, "w")
    if not f then
        print("[OptiScaler Capture] 无法写入 " .. commandFile ..
              " — 请确认 OptiScaler.ini 的 [Capture] Enabled=true")
        return false
    end
    f:write(cmd)
    f:close()
    return true
end

local function readStatus()
    local f = io.open(statusFile, "r")
    if not f then
        return nil
    end
    local content = f:read("*a")
    f:close()
    return content
end

-- ---------------------------------------------------------------------------
-- 极简 JSON 取值（只处理本模块自己写出的扁平结构，避免引入依赖）
-- ---------------------------------------------------------------------------
local function jsonField(raw, name)
    if not raw then
        return nil
    end
    local str = raw:match('"' .. name .. '"%s*:%s*"([^"]*)"')
    if str then
        return str
    end
    local num = raw:match('"' .. name .. '"%s*:%s*([%-%d%.]+)')
    if num then
        return tonumber(num)
    end
    local boolean = raw:match('"' .. name .. '"%s*:%s*(%a+)')
    if boolean then
        return boolean
    end
    return nil
end

local function formatBytes(value)
    local n = tonumber(value) or 0
    local units = { "B", "KiB", "MiB", "GiB", "TiB" }
    local index = 1
    while n >= 1024 and index < #units do
        n = n / 1024
        index = index + 1
    end
    return string.format("%.2f %s", n, units[index])
end

local function printStatus()
    local raw = readStatus()
    if not raw then
        print("[OptiScaler Capture] 未找到 status.json — OptiScaler 可能未启用 [Capture]")
        return
    end

    local state = jsonField(raw, "state") or "?"
    local captured = jsonField(raw, "captured_frames") or 0
    local dropped = jsonField(raw, "dropped_frames") or 0
    local inflight = jsonField(raw, "in_flight") or 0
    local bytes = jsonField(raw, "bytes_written") or 0
    local width = jsonField(raw, "width") or 0
    local height = jsonField(raw, "height") or 0
    local session = jsonField(raw, "session_dir")
    local message = jsonField(raw, "message")

    print(string.format(
        "[OptiScaler Capture] state=%s | frames=%s dropped=%s inflight=%s | %s | %sx%s",
        tostring(state), tostring(captured), tostring(dropped), tostring(inflight),
        formatBytes(bytes), tostring(width), tostring(height)))

    if session and session ~= "" then
        print("[OptiScaler Capture] 输出目录: " .. session)
    end
    if message and message ~= "" then
        print("[OptiScaler Capture] message: " .. message)
    end
end

-- ---------------------------------------------------------------------------
-- 动作实现
-- ---------------------------------------------------------------------------
local function cmdStart()
    if sendCommand("START") then
        print("[OptiScaler Capture] 已发送 START，将在下一个 DLSS 求值帧开始采集")
    end
end

local function cmdStop()
    if sendCommand("STOP") then
        print("[OptiScaler Capture] 已发送 STOP")
    end
end

local function cmdStatus()
    sendCommand("STATUS")
    printStatus()
end

local function cmdWatch()
    watchEnabled = not watchEnabled
    watchTimer = 0.0
    if watchEnabled then
        print("[OptiScaler Capture] 自动状态打印已开启")
    else
        print("[OptiScaler Capture] 自动状态打印已关闭")
    end
end

-- ---------------------------------------------------------------------------
-- A) 热键注册
-- 必须在 init.lua「加载期间」调用：CET 会在 init.lua 执行完后把
-- registerForEvent / registerHotkey / registerInput 置为 nil。
-- 绑定位置：CET 覆盖层 → Bindings 页，找 id 以 OptiCapture 开头的项。
-- ---------------------------------------------------------------------------
if registerHotkey then
    registerHotkey("OptiCaptureStart",  "OptiScaler Capture: 开始采集", cmdStart)
    registerHotkey("OptiCaptureStop",   "OptiScaler Capture: 停止采集", cmdStop)
    registerHotkey("OptiCaptureStatus", "OptiScaler Capture: 查询状态", cmdStatus)
    registerHotkey("OptiCaptureWatch",  "OptiScaler Capture: 开关自动状态打印", cmdWatch)
end

-- ---------------------------------------------------------------------------
-- B) 尽力把命令注入控制台
-- CET 每个沙箱有自己的 env，env 的 metatable.__index 指向一张共享回退表；
-- 控制台沙箱也走同一张表。把函数 rawset 进去，控制台就有机会看到。
-- 依赖 CET 内部实现，可能失效；失败则退化为「只用热键 / 手写 command.txt」。
-- ---------------------------------------------------------------------------
local function exposeToConsole()
    local probe = _ENV
    if type(probe) ~= "table" then probe = _G end
    if type(probe) ~= "table" then return false end

    local mtOk, mt = pcall(getmetatable, probe)
    if not mtOk or type(mt) ~= "table" then return false end

    local shared = mt.__index
    if type(shared) ~= "table" then return false end

    rawset(shared, "OptiCaptureStart",  cmdStart)
    rawset(shared, "OptiCaptureStop",   cmdStop)
    rawset(shared, "OptiCaptureStatus", cmdStatus)
    rawset(shared, "OptiCaptureWatch",  cmdWatch)
    return true
end

local consoleExposed = false
pcall(function() consoleExposed = exposeToConsole() end)

registerForEvent("onInit", function()
    print("[OptiScaler Capture] CET 端已加载。")
    if consoleExposed then
        print("[OptiScaler Capture] 控制台可用（记得带括号）：OptiCaptureStart() / OptiCaptureStop() / OptiCaptureStatus() / OptiCaptureWatch()")
    else
        print("[OptiScaler Capture] 控制台注入不可用；请用热键（Bindings 页绑定 OptiCapture*），或直接手写 command.txt")
    end
end)

-- ---------------------------------------------------------------------------
-- 可选的自动状态轮询
-- ---------------------------------------------------------------------------
registerForEvent("onUpdate", function(deltaTime)
    if not watchEnabled then
        return
    end

    watchTimer = watchTimer + deltaTime
    if watchTimer < WATCH_INTERVAL then
        return
    end

    watchTimer = 0.0
    printStatus()
end)

-- init.lua 的返回值会成为本 mod 的对象；控制台里可尝试：
--   GetMod("optiscaler_capture"):Status()
return {
    Start  = cmdStart,
    Stop   = cmdStop,
    Status = cmdStatus,
    Watch  = cmdWatch,
}
