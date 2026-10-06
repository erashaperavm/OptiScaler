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
-- ── 本 mod 提供的入口 ──────────────────────────────────────────────────────
--   A) 屏幕 HUD（ImGui）
--      常驻屏幕中轴线顶端（距顶 60 px），鲜绿色长方形底、黑字，显示：
--        · 状态：捕获中 / 捕获结束 / 待命
--        · 已捕获秒数、帧数、体积；触顶时显示原因
--   B) Page Up 键：开始 / 结束采集（在 onDraw 里用 ImGui 检测；若你已在
--      CET Bindings 页把 OptiCaptureToggle 绑了键，则改由绑定回调触发，避免双触发）
--   C) 热键（Bindings 页绑定，id 以 OptiCapture 开头）
--      OptiCaptureToggle / OptiCaptureStart / OptiCaptureStop /
--      OptiCaptureStatus / OptiCaptureWatch
--   D) 控制台（尽力而为，取决于 CET 版本，注意带括号）
--      OptiCaptureToggle() / OptiCaptureStart() / OptiCaptureStop() / ...
--   E) 完全不用 CET：直接在通信目录手写 command.txt（START / STOP / STATUS）
--
-- ── 单次录制上限（本脚本强制）─────────────────────────────────────────────
--   最长 5 分钟，或落盘 50 GiB，先到先停（自动发 STOP）。
--
-- ── 关于「控制台命令」──────────────────────────────────────────────────────
-- CET 并没有 registerConsoleCommand 这类 API。控制台本质上只是一个 Lua REPL：
--   · Console.cpp      —— 没有命令表，把输入原样交给 ExecuteLua()
--   · ScriptContext.cpp —— 只给 mod 注入 registerForEvent / registerHotkey /
--                          registerInput，且 init.lua 跑完就置 nil
--   · LuaSandbox.cpp   —— 每个 mod 独立沙箱，控制台是独立的 sandbox 0
-- 所以「裸敲 OptiCaptureStatus」在 CET 里永远是语法错误，必须带括号。
--
-- ── 兼容性（重要）─────────────────────────────────────────────────────────
-- CET 的 Lua 运行时是 5.2（其全局白名单里含 bit32 可为证）。以下 5.3+ 才有的
-- 语法在本文件里会**直接导致语法错误、整个 mod 加载失败**，务必避免：
--   · 位运算符  |  &  ~  <<  >>      → 用 bit32.bor/band/bxor/lshift/rshift 代替
--   · 整除运算符  //                 → 用 math.floor(a / b)
-- 组合 ImGui flag 请用上面的 OR()（内部走 bit32.bor，退化为加法）。
-- ============================================================================

-- ⚠️ CET 沙箱的 io 把「相对路径」解析到**本 mod 自己的目录**，且只允许访问该目录树
-- （绝对路径、`..\` 逃逸一律拒绝）。而 OptiScaler 也正是把 command.txt / status.json
-- 写在本 mod 目录里，所以这里用相对 mod 根目录的**短文件名**即可。
-- 若写成 plugins/cyber_engine_tweaks/mods/optiscaler_capture/command.txt，会被解析成
-- mod 目录下的嵌套子路径（目录不存在）→ io.open 返回 nil，表现为"无法写入 command.txt"。
local commandFile = "command.txt"
local statusFile  = "status.json"

-- ── 单次录制上限 ───────────────────────────────────────────────────────────
local MAX_SECONDS = 300                              -- 5 分钟
local MAX_BYTES   = 50 * 1024 * 1024 * 1024          -- 50 GiB

-- ── HUD 文案 ───────────────────────────────────────────────────────────────
-- CET 自带 fonts/NotoSansSC-Regular.otf（简体中文），HUD 可直接显示中文。
-- 若你的 CET 版本较老、中文显示成方块，把下面三项换成 ASCII：
--   capturing = "REC" / ended = "DONE" / idle = "READY"
local TEXT = {
    capturing = "捕获中",
    ended     = "捕获结束",
    idle      = "待命",
}

local watchEnabled = false
local watchTimer = 0.0
local WATCH_INTERVAL = 5.0

-- ── HUD 状态 ───────────────────────────────────────────────────────────────
local hudVisible = true
local hudErrorLogged = false
local hudFlagsCache = nil
local hudProbeDone = false
local fps = 0.0

-- 从 status.json 读到的捕获状态
local st = {
    online = false,
    state = nil,          -- "capturing" / "idle" / "stopped" / nil
    frames = 0,
    dropped = 0,
    bytes = 0,
    elapsed = 0.0,        -- 已捕获秒数（本脚本按墙钟累计）
    everCaptured = false, -- 本进程内是否采集过（用于区分「待命」与「捕获结束」）
    limitHit = false,
    limitMessage = nil,
    prevCapturing = false,
}
local intent = false      -- 用户意图：true = 希望正在采集（让 Page Up 即时响应）
local pollTimer = 0.0
local POLL_INTERVAL = 0.25

-- ---------------------------------------------------------------------------
-- 底层：发命令 / 读状态
-- ---------------------------------------------------------------------------
local function sendCommand(cmd)
    local f = io.open(commandFile, "w")
    if not f then
        print("[OptiScaler Capture] io.open 失败: " .. commandFile ..
              "（CET 沙箱只允许访问本 mod 目录；也可能是 OptiScaler 未启用 [Capture] Enabled）")
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
-- 状态轮询 + 上限强制
-- ---------------------------------------------------------------------------
local function refreshStatus()
    local raw = readStatus()
    if not raw then
        st.online = false
        return
    end
    st.online = true
    st.state = jsonField(raw, "state")
    st.frames = tonumber(jsonField(raw, "captured_frames")) or 0
    st.dropped = tonumber(jsonField(raw, "dropped_frames")) or 0
    st.bytes = tonumber(jsonField(raw, "bytes_written")) or 0
end

local function updateCaptureState(dt)
    pollTimer = pollTimer + dt
    if pollTimer >= POLL_INTERVAL then
        pollTimer = 0.0
        refreshStatus()
    end

    local nowCapturing = (st.state == "capturing")

    -- 新会话开始：重置计时
    if nowCapturing and not st.prevCapturing then
        st.elapsed = 0.0
        st.everCaptured = true
        st.limitHit = false
        st.limitMessage = nil
    end

    -- 用真实状态校正用户意图
    if nowCapturing then
        intent = true
    elseif st.prevCapturing then
        intent = false
    end
    st.prevCapturing = nowCapturing

    if nowCapturing then
        st.elapsed = st.elapsed + dt

        if not st.limitHit then
            if st.elapsed >= MAX_SECONDS then
                st.limitHit = true
                st.limitMessage = string.format("已达 %.0f 分钟上限，自动停止", MAX_SECONDS / 60.0)
                sendCommand("STOP")
                print("[OptiScaler Capture] " .. st.limitMessage)
            elseif st.bytes >= MAX_BYTES then
                st.limitHit = true
                st.limitMessage = string.format("已达 %.0f GB 上限，自动停止", MAX_BYTES / (1024.0 * 1024.0 * 1024.0))
                sendCommand("STOP")
                print("[OptiScaler Capture] " .. st.limitMessage)
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- 动作实现
-- ---------------------------------------------------------------------------
local function cmdStart()
    intent = true
    if sendCommand("START") then
        print("[OptiScaler Capture] 已发送 START，将在下一个 DLSS 求值帧开始采集")
    end
end

local function cmdStop()
    intent = false
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

-- 开始 / 结束（Page Up 与 OptiCaptureToggle 都走这里）
local function toggleCapture()
    intent = not intent
    if intent then
        sendCommand("START")
        print("[OptiScaler Capture] 开始采集")
    else
        sendCommand("STOP")
        print("[OptiScaler Capture] 停止采集")
    end
    refreshStatus()
end

-- ---------------------------------------------------------------------------
-- ImGui HUD
-- ---------------------------------------------------------------------------
-- 组合两个 ImGui flag。注意：CET 的 Lua 是 5.2，没有位运算符（| 是 5.3+ 语法，
-- 在 5.2 里会直接语法报错、整个 mod 加载失败），所以这里用 bit32.bor，退化为加法。
-- ImGui 各 flag 互不重叠，加法等价于按位或。
local function OR(a, b)
    if a == nil then return b end
    if b == nil then return a end
    if bit32 ~= nil and bit32.bor ~= nil then
        local ok, v = pcall(bit32.bor, a, b)
        if ok and v ~= nil then return v end
    end
    return a + b
end

local function getHudFlags()
    if hudFlagsCache ~= nil then
        return hudFlagsCache
    end
    if ImGuiWindowFlags == nil then
        hudFlagsCache = 0
        return hudFlagsCache
    end
    local flags = {
        ImGuiWindowFlags.NoTitleBar,
        ImGuiWindowFlags.NoResize,
        ImGuiWindowFlags.NoMove,
        ImGuiWindowFlags.NoScrollbar,
        ImGuiWindowFlags.NoSavedSettings,
        ImGuiWindowFlags.NoInputs,
        ImGuiWindowFlags.AlwaysAutoResize,
        ImGuiWindowFlags.NoFocusOnAppearing,
        ImGuiWindowFlags.NoNav,
    }
    local acc = nil
    for _, v in ipairs(flags) do
        acc = OR(acc, v)
    end
    hudFlagsCache = acc or 0
    return hudFlagsCache
end

-- PushStyleColor / PushStyleVar / SetNextWindowPos 的参数形式随 sol_ImGui 版本而异
-- （4 个 float 还是 ImVec4；ImVec2 还是散装 float）。这里两种都试，并记录实际压栈数量，
-- 保证 Pop 数量匹配，避免 ImGui 断言。
local pushedStyleVars = 0
local pushedStyleColors = 0

local function pushStyleColorSafe(idx, r, g, b, a)
    local ok = pcall(function() ImGui.PushStyleColor(idx, r, g, b, a) end)
    if not ok then
        ok = pcall(function() ImGui.PushStyleColor(idx, ImVec4(r, g, b, a)) end)
    end
    if ok then pushedStyleColors = pushedStyleColors + 1 end
end

local function pushStyleVarSafe(idx, a, b)
    local ok
    if b ~= nil then
        ok = pcall(function() ImGui.PushStyleVar(idx, ImVec2(a, b)) end)
        if not ok then ok = pcall(function() ImGui.PushStyleVar(idx, a, b) end) end
    else
        ok = pcall(function() ImGui.PushStyleVar(idx, a) end)
    end
    if ok then pushedStyleVars = pushedStyleVars + 1 end
end

-- 官方 wiki 用的是散装数字形式（SetNextWindowPos(x, y, cond)），优先它，ImVec2 兜底。
local function setNextWindowPosSafe(x, y, px, py)
    if not pcall(function() ImGui.SetNextWindowPos(x, y, ImGuiCond.Always, px, py) end) then
        pcall(function() ImGui.SetNextWindowPos(ImVec2(x, y), ImGuiCond.Always, ImVec2(px, py)) end)
    end
end

local function setNextWindowSizeSafe(w, h)
    if not pcall(function() ImGui.SetNextWindowSize(w, h, ImGuiCond.Always) end) then
        pcall(function() ImGui.SetNextWindowSize(ImVec2(w, h), ImGuiCond.Always) end)
    end
end

-- 屏幕宽度：优先用 CET 自带的 GetDisplayResolution()（白名单里确有它）；
-- ImGui.GetIO().DisplaySize 作为兜底——CET 的 sol_ImGui 不一定绑定 GetIO。
local function getDisplayWidth()
    if GetDisplayResolution ~= nil then
        local ok, w = pcall(GetDisplayResolution)
        if ok and type(w) == "number" and w > 0 then
            return w
        end
    end
    if ImGui ~= nil and ImGui.GetIO ~= nil then
        local ok, w = pcall(function()
            return ImGui.GetIO().DisplaySize.x
        end)
        if ok and type(w) == "number" and w > 0 then
            return w
        end
    end
    return nil
end

local function drawHud()
    if ImGui == nil or ImGui.Begin == nil then
        return
    end

    -- 一次性探测：把可用的 API 打到日志，便于排障
    if not hudProbeDone then
        hudProbeDone = true
        print(string.format(
            "[OptiScaler Capture] HUD 探测: GetDisplayResolution=%s GetIO=%s Begin=%s ImGuiWindowFlags=%s",
            tostring(GetDisplayResolution ~= nil),
            tostring(ImGui.GetIO ~= nil),
            tostring(ImGui.Begin ~= nil),
            tostring(ImGuiWindowFlags ~= nil)))
    end

    local displayW = getDisplayWidth()
    if displayW ~= nil then
        -- 中轴线顶端，距顶 60 px；pivot=(0.5,0) 水平居中
        setNextWindowPosSafe(displayW * 0.5, 60.0, 0.5, 0.0)
    else
        setNextWindowPosSafe(100.0, 100.0, 0.0, 0.0) -- 拿不到分辨率时的兜底位置
    end
    setNextWindowSizeSafe(230.0, 0.0) -- 固定宽度、高度自适应

    -- 鲜绿色底、黑字
    pushedStyleColors = 0
    pushedStyleVars = 0
    pushStyleColorSafe(ImGuiCol.WindowBg, 0.06, 0.92, 0.12, 0.88)
    pushStyleColorSafe(ImGuiCol.Text, 0.02, 0.06, 0.02, 1.0)
    pushStyleVarSafe(ImGuiStyleVar.WindowPadding, 10.0, 6.0)
    pushStyleVarSafe(ImGuiStyleVar.WindowRounding, 6.0)
    pushStyleVarSafe(ImGuiStyleVar.WindowBorderSize, 0.0)

    -- Begin：先试带 flags（去标题栏/边框），失败退回最简形式（官方 wiki 保证可用）。
    -- 只有 Begin 成功才配对调用 End，避免 ImGui 断言。
    local began = pcall(ImGui.Begin, "##opti_capture_hud", getHudFlags())
    if not began then
        began = pcall(ImGui.Begin, "##opti_capture_hud")
    end

    if began then
        local label
        if st.state == "capturing" then
            label = TEXT.capturing
        elseif st.everCaptured then
            label = TEXT.ended
        else
            label = TEXT.idle
        end

        ImGui.Text(string.format("%s   %.1f s", label, st.elapsed))
        ImGui.Text(string.format("%.0f FPS · %d 帧 · %s", fps, st.frames, formatBytes(st.bytes)))
        if st.limitMessage ~= nil then
            ImGui.Text(st.limitMessage)
        elseif not st.online then
            ImGui.Text("status.json 未就绪")
        end

        ImGui.End()
    end

    if pushedStyleVars > 0 then
        ImGui.PopStyleVar(pushedStyleVars)
    end
    if pushedStyleColors > 0 then
        ImGui.PopStyleColor(pushedStyleColors)
    end
end

-- Page Up：若用户已把 OptiCaptureToggle 绑了键，就交给绑定回调（避免一次按键触发两次）
local function checkPageUp()
    if ImGui == nil or ImGuiKey == nil or ImGui.IsKeyPressed == nil then
        return
    end
    if ImGuiKey.PageUp == nil then
        return
    end
    if IsBound ~= nil and IsBound("OptiCaptureToggle") then
        return
    end

    local ok, pressed = pcall(ImGui.IsKeyPressed, ImGuiKey.PageUp, false)
    if not ok then
        ok, pressed = pcall(ImGui.IsKeyPressed, ImGuiKey.PageUp)
    end
    if ok and pressed then
        toggleCapture()
    end
end

-- ---------------------------------------------------------------------------
-- 热键注册
-- 必须在 init.lua「加载期间」调用：CET 会在 init.lua 执行完后把
-- registerForEvent / registerHotkey / registerInput 置为 nil。
-- 绑定位置：CET 覆盖层 → Bindings 页，找 id 以 OptiCapture 开头的项。
-- ---------------------------------------------------------------------------
if registerHotkey then
    registerHotkey("OptiCaptureToggle", "OptiScaler Capture: 开始/结束（Page Up）", toggleCapture)
    registerHotkey("OptiCaptureStart",  "OptiScaler Capture: 开始采集", cmdStart)
    registerHotkey("OptiCaptureStop",   "OptiScaler Capture: 停止采集", cmdStop)
    registerHotkey("OptiCaptureStatus", "OptiScaler Capture: 查询状态", cmdStatus)
    registerHotkey("OptiCaptureWatch",  "OptiScaler Capture: 开关自动状态打印", cmdWatch)
end

-- ---------------------------------------------------------------------------
-- 尽力把命令注入控制台
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

    rawset(shared, "OptiCaptureToggle", toggleCapture)
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
    print(string.format("[OptiScaler Capture] HUD 常驻屏幕顶端；Page Up 开始/结束；单次上限 %d 分钟 / %.0f GB",
        MAX_SECONDS / 60, MAX_BYTES / (1024.0 * 1024.0 * 1024.0)))
    if consoleExposed then
        print("[OptiScaler Capture] 控制台可用（记得带括号）：OptiCaptureToggle() / OptiCaptureStart() / OptiCaptureStop() / OptiCaptureStatus() / OptiCaptureWatch()")
    else
        print("[OptiScaler Capture] 控制台注入不可用；请用 Page Up 或热键（Bindings 页绑定 OptiCapture*），或直接手写 command.txt")
    end
end)

-- ---------------------------------------------------------------------------
-- 每帧：状态轮询 + 上限 + 可选自动状态打印
-- ---------------------------------------------------------------------------
registerForEvent("onUpdate", function(deltaTime)
    local dt = deltaTime or 0.0
    if dt > 0 then
        local inst = 1.0 / dt
        fps = (fps <= 0) and inst or (fps * 0.9 + inst * 0.1) -- 指数平滑
    end
    updateCaptureState(dt)

    if watchEnabled then
        watchTimer = watchTimer + dt
        if watchTimer >= WATCH_INTERVAL then
            watchTimer = 0.0
            printStatus()
        end
    end
end)

-- ---------------------------------------------------------------------------
-- 每帧：HUD + Page Up
-- ImGui 只能在 onDraw 里调用；若某步失败，捕获功能不受影响。
-- ---------------------------------------------------------------------------
registerForEvent("onDraw", function()
    pcall(checkPageUp)

    if hudVisible then
        local ok = pcall(drawHud)
        if not ok and not hudErrorLogged then
            hudErrorLogged = true
            print("[OptiScaler Capture] HUD 绘制失败：CET 的 ImGui API 与预期不符（将不再重复提示）。" ..
                  "捕获功能不受影响，Page Up 仍可用。")
        end
    end
end)

-- init.lua 的返回值会成为本 mod 的对象；控制台里可尝试：
--   GetMod("optiscaler_capture"):Status()
return {
    Toggle = toggleCapture,
    Start  = cmdStart,
    Stop   = cmdStop,
    Status = cmdStatus,
    Watch  = cmdWatch,
}
