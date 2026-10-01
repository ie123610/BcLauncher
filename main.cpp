#include <iostream>
#include <string>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#include <shlobj.h> // 用于 SHGetFolderPathW

// 引入 nlohmann/json 单头文件
#include "json.hpp"
using json = nlohmann::json;

// 全局版本号定义
const wchar_t* HOST_VERSION = L"1.1.2";

// 日志功能控制开关（true 开，false 关）
const bool ENABLE_LOGGING = false;

// 官方预设默认扩展名列表（当 XML 节点缺失或文件不存在时使用）
const std::string DEFAULT_EXT_LIST = ".torrent;.metalink;.mp4;.mkv;.asf;.avi;.mpg;.rm;.rmvb;.wmv;.mp3;.wma;.zip;.rar;.7z;.iso;.exe;.msi;.msix";

// 错误码定义
enum LaunchResult {
    LAUNCH_SUCCESS = 0,
    LAUNCH_ERR_REGISTRY = 1,
    LAUNCH_ERR_CREATE_PROCESS = 2
};

// 辅助函数：将 wstring 转换为 string (UTF-8/ANSI)
std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_ACP, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(sizeNeeded, 0);
    WideCharToMultiByte(CP_ACP, 0, &wstr[0], (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);
    return strTo;
}

// 日志记录函数
void WriteLog(const std::string& logMsg) {
    if (!ENABLE_LOGGING) return;

    std::ofstream logFile("native_host_debug.log", std::ios::app);
    if (logFile.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        struct tm timeinfo;
        localtime_s(&timeinfo, &in_time_t);

        logFile << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "] "
            << logMsg << std::endl;
        logFile.close();
    }
}

// =========================================================
// 定位 BitComet.xml 并提取扩展名列表
// =========================================================
std::string GetBitCometExtensionList() {
    WriteLog("--- 开始查找 BitComet 配置文件 ---");
    std::wstring xmlPath = L"";

    // 1. 尝试检查便携模式路径（代理程序位于 \BitComet\tools\ 下，上一级目录即为主程序目录）
    wchar_t modulePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) > 0) {
        std::wstring currentExeDir(modulePath);
        size_t lastSlash = currentExeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            std::wstring toolsDir = currentExeDir.substr(0, lastSlash); // ...\BitComet\tools
            size_t secondLastSlash = toolsDir.find_last_of(L"\\/");
            if (secondLastSlash != std::wstring::npos) {
                std::wstring portableXml = toolsDir.substr(0, secondLastSlash + 1) + L"BitComet.xml"; // ...\BitComet\BitComet.xml
                WriteLog("检测便携版配置文件路径: " + WStringToString(portableXml));
                if (GetFileAttributesW(portableXml.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    xmlPath = portableXml;
                    WriteLog("找到便携版配置文件！");
                }
            }
        }
    }

    // 2. 若便携版未找到，尝试检查安装版 AppData 路径 (%APPDATA%\BitComet\BitComet.xml)
    if (xmlPath.empty()) {
        wchar_t appDataPath[MAX_PATH] = { 0 };
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
            std::wstring installedXml = std::wstring(appDataPath) + L"\\BitComet\\BitComet.xml";
            WriteLog("检测安装版配置文件路径: " + WStringToString(installedXml));
            if (GetFileAttributesW(installedXml.c_str()) != INVALID_FILE_ATTRIBUTES) {
                xmlPath = installedXml;
                WriteLog("找到安装版配置文件！");
            }
        }
    }

    // 3. 配置文件均未找到，回退到默认列表
    if (xmlPath.empty()) {
        WriteLog("[警告] 未能找到任何 BitComet.xml 配置文件，退回默认扩展名列表。");
        return DEFAULT_EXT_LIST;
    }

    // 4. 打开并解析 XML 文件中的 <HttpDownloadIEMonitorFileType> 节点
    std::ifstream xmlFile(xmlPath);
    if (!xmlFile.is_open()) {
        WriteLog("[错误] 无法打开配置文件: " + WStringToString(xmlPath) + "，退回默认扩展名列表。");
        return DEFAULT_EXT_LIST;
    }

    std::string content((std::istreambuf_iterator<char>(xmlFile)), std::istreambuf_iterator<char>());
    xmlFile.close();

    std::string startTag = "<HttpDownloadIEMonitorFileType>";
    std::string endTag = "</HttpDownloadIEMonitorFileType>";

    size_t startPos = content.find(startTag);
    if (startPos == std::string::npos) {
        WriteLog("[提示] XML 文件中缺少 <HttpDownloadIEMonitorFileType> 节点（用户未曾修改过），退回默认扩展名列表。");
        return DEFAULT_EXT_LIST;
    }

    startPos += startTag.length();
    size_t endPos = content.find(endTag, startPos);

    if (endPos == std::string::npos) {
        WriteLog("[错误] XML 格式异常，未找到匹配的闭合标签 </HttpDownloadIEMonitorFileType>，退回默认扩展名列表。");
        return DEFAULT_EXT_LIST;
    }

    std::string extList = content.substr(startPos, endPos - startPos);

    // 去除两端可能包含的空白字符
    extList.erase(0, extList.find_first_not_of(" \t\r\n"));
    extList.erase(extList.find_last_not_of(" \t\r\n") + 1);

    if (extList.empty()) {
        WriteLog("[提示] <HttpDownloadIEMonitorFileType> 节点内容为空，退回默认扩展名列表。");
        return DEFAULT_EXT_LIST;
    }

    WriteLog("成功获取扩展名列表: " + extList);
    return extList;
}

// =========================================================
// 读取注册表并使用 CreateProcessW 直接唤起 URL Protocol
// =========================================================
LaunchResult LaunchProtocolDirectly(const std::string& bcUrl) {
    WriteLog("开始尝试唤起协议: " + (bcUrl.empty() ? "(普通直接启动)" : bcUrl));

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"bc\\shell\\open\\command", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        WriteLog("[错误] 打开注册表项 HKEY_CLASSES_ROOT\\bc\\shell\\open\\command 失败！");
        return LAUNCH_ERR_REGISTRY;
    }

    wchar_t cmdTemplate[1024] = { 0 };
    DWORD bufSize = sizeof(cmdTemplate);
    DWORD type = REG_SZ;

    LONG res = RegQueryValueExW(hKey, NULL, NULL, &type, (LPBYTE)cmdTemplate, &bufSize);
    RegCloseKey(hKey);

    // 严谨校验：如果读取失败，或者读取出的模板为空，均视为注册表项无效
    if (res != ERROR_SUCCESS || bufSize <= sizeof(wchar_t) || cmdTemplate[0] == L'\0') {
        WriteLog("[错误] 读取注册表默认键值失败或读取结果为空！res: " + std::to_string(res) + ", bufSize: " + std::to_string(bufSize));
        return LAUNCH_ERR_REGISTRY;
    }

    WriteLog("成功读取注册表命令模板: " + WStringToString(cmdTemplate));

    std::wstring templateStr(cmdTemplate);

    // 从命令模板中提取真正的 Exe 路径
    std::wstring exePath;
    if (!templateStr.empty() && templateStr[0] == L'\"') {
        size_t secondQuote = templateStr.find(L'\"', 1);
        if (secondQuote != std::wstring::npos) {
            exePath = templateStr.substr(1, secondQuote - 1);
        }
    }
    else {
        size_t spacePos = templateStr.find(L' ');
        if (spacePos != std::wstring::npos) {
            exePath = templateStr.substr(0, spacePos);
        }
        else {
            exePath = templateStr;
        }
    }

    WriteLog("解析出的比特彗星 Exe 路径: " + WStringToString(exePath));

    // 预先校验 Exe 文件是否存在
    if (!exePath.empty() && GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteLog("[错误] 指定的 Exe 文件不存在或无法访问！路径: " + WStringToString(exePath));
        return LAUNCH_ERR_CREATE_PROCESS;
    }

    std::wstring fullCommandLine;

    // 判读是“普通启动”还是“带下载参数启动”
    if (bcUrl.empty() || bcUrl == "bc://") {
        // 普通启动：仅运行主程序路径，不传递 /url 参数
        fullCommandLine = L"\"" + exePath + L"\"";
    }
    else {
        // 带参数启动：使用输入的 URL 替换模板中的 %1
        int urlLen = MultiByteToWideChar(CP_ACP, 0, bcUrl.c_str(), -1, NULL, 0);
        std::wstring wBcUrl(urlLen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, bcUrl.c_str(), -1, &wBcUrl[0], urlLen);

        size_t pos = templateStr.find(L"%1");
        if (pos != std::wstring::npos) {
            fullCommandLine = templateStr.replace(pos, 2, wBcUrl.c_str());
        }
        else {
            fullCommandLine = templateStr + L" \"" + wBcUrl.c_str() + L"\"";
        }
    }

    WriteLog("拼接后的最终命令行: " + WStringToString(fullCommandLine));

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWDEFAULT;

    wchar_t* cmdBuffer = &fullCommandLine[0];
    const wchar_t* appName = exePath.empty() ? NULL : exePath.c_str();

    // 正确传入 ExePath 与 完整命令行
    BOOL success = CreateProcessW(
        appName,
        cmdBuffer,
        NULL, NULL,
        FALSE,
        0,
        NULL, NULL,
        &si, &pi
    );

    if (success != FALSE) {
        WriteLog("[成功] CreateProcessW 调用成功，进程已启动！PID: " + std::to_string(pi.dwProcessId));
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return LAUNCH_SUCCESS;
    }

    DWORD errCode = GetLastError();
    WriteLog("[错误] CreateProcessW 调用失败！GetLastError 错误码: " + std::to_string(errCode));

    return LAUNCH_ERR_CREATE_PROCESS;
}

// =========================================================
// Chrome Native Messaging 标准二进制应答函数
// =========================================================
void SendNativeResponse(const std::string& jsonStr) {
    WriteLog("发送响应给 Chrome: " + jsonStr);

    (void)_setmode(_fileno(stdout), _O_BINARY);

    uint32_t length = static_cast<uint32_t>(jsonStr.length());
    std::cout.write(reinterpret_cast<const char*>(&length), sizeof(length));
    std::cout.write(jsonStr.c_str(), length);
    std::cout.flush();
}

// 接收 json 对象并序列化输出给 Chrome
void SendNativeResponseJson(const json& j) {
    SendNativeResponse(j.dump());
}

// 从标准输入 std::cin 读取 Chrome 传过来的 JSON 消息
std::string ReadNativeMessage() {
    (void)_setmode(_fileno(stdin), _O_BINARY);

    uint32_t length = 0;
    // 读取 4 字节 Little-Endian 消息长度
    if (!std::cin.read(reinterpret_cast<char*>(&length), sizeof(length))) {
        return "";
    }

    if (length == 0 || length > 1024 * 1024) { // 限制最大 1MB 消息，防止异常
        return "";
    }

    std::string buffer(length, '\0');
    if (!std::cin.read(&buffer[0], length)) {
        return "";
    }

    return buffer;
}

// URL 编码函数
std::string URLEncode(const std::string& str) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;

    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        }
        else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

int main(int argc, char* argv[]) {
    // 判断是否由非 Chrome 唤醒（没有传入参数）
    if (argc <= 1) {
        std::wstring msg = L"This is a BitComet Native Messaging Host and cannot be run directly.\nVersion: ";
        msg += HOST_VERSION;

        MessageBoxW(
            NULL,
            msg.c_str(),
            L"BitComet Native Host",
            MB_OK | MB_ICONINFORMATION | MB_TOPMOST
        );
        return 0;
    }

    WriteLog("================ Host 进程启动 ================");

    // 强制设置标准流模式
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);

    // 1. 读取来自 Chrome 的 Native Message JSON 消息
    std::string jsonInput = ReadNativeMessage();

    if (jsonInput.empty()) {
        WriteLog("[警告] 收到空的 JSON 消息或读取失败！");
        return 0;
    }

    WriteLog("收到来自 Chrome 的原始 JSON: " + jsonInput);

    // 2. 解析 JSON 数据结构
    json jInput;
    try {
        jInput = json::parse(jsonInput);
    }
    catch (const json::parse_error& e) {
        WriteLog("[错误] JSON 解析失败: " + std::string(e.what()));
        SendNativeResponseJson({
            {"status", "error"},
            {"message", "invalid_json"}
            });
        return 0;
    }

    // 提取字段 (使用 safe value 访问，支持默认值)
    std::string action = jInput.value("action", "");
    std::string fileUrl = jInput.value("link", "");
    std::string referer = jInput.value("referurl", jInput.value("referer", ""));
    std::string cookie = jInput.value("cookie", "");
    std::string userAgent = jInput.value("user_agent", "");
    std::string taskName = jInput.value("name", "");

    // 3. 处理预检请求 GetConfig
    if (action == "GetConfig") {
        WriteLog("响应动作: GetConfig");
        std::string extList = GetBitCometExtensionList();
        SendNativeResponseJson({
            {"ext_list", extList}
            });
        return 0;
    }

    // 4. 处理打开客户端请求 Launch_Program
    if (action == "Launch_Program") {
        WriteLog("响应动作: Launch_Program");
        // 传入空字符串，触发纯启动模式
        LaunchResult result = LaunchProtocolDirectly("");
        if (result == LAUNCH_ERR_REGISTRY) {
            SendNativeResponseJson({
                {"status", "error"},
                {"message", "registry_read_error: BC protocol may not be registered"}
                });
        }
        else if (result == LAUNCH_ERR_CREATE_PROCESS) {
            SendNativeResponseJson({
                {"status", "error"},
                {"message", "launch_failed: BitComet executable may have been moved"}
                });
        }
        else {
            SendNativeResponseJson({
                {"status", "ok"}
                });
        }
        return 0;
    }

    // 5. 校验核心 URL 参数
    if (fileUrl.empty()) {
        WriteLog("[错误] 参数缺失: missing_url");
        SendNativeResponseJson({
            {"status", "error"},
            {"message", "missing_url"}
            });
        return 0;
    }

    // 6. 执行 URL 编码
    std::string encName = URLEncode(taskName);
    std::string encUrl = URLEncode(fileUrl);
    std::string encRef = URLEncode(referer);
    std::string encCook = URLEncode(cookie);
    std::string encUA = URLEncode(userAgent);

    // 7. 拼接 bc:// 唤起协议
    std::string bcUrl = "bc://http2/" + encName + "/?url=" + encUrl;
    if (!encRef.empty())  bcUrl += "&refer=" + encRef;
    if (!encCook.empty()) bcUrl += "&cookie=" + encCook;
    if (!encUA.empty())   bcUrl += "&user_agent=" + encUA;

    // 8. 唤起比特彗星 (带下载任务参数)
    LaunchResult result = LaunchProtocolDirectly(bcUrl);
    if (result == LAUNCH_ERR_REGISTRY) {
        SendNativeResponseJson({
            {"status", "error"},
            {"message", "registry_read_error: BC protocol may not be registered"}
            });
    }
    else if (result == LAUNCH_ERR_CREATE_PROCESS) {
        SendNativeResponseJson({
            {"status", "error"},
            {"message", "launch_failed: BitComet executable may have been moved"}
            });
    }
    else {
        // 9. 给 Chrome 返回成功响应
        SendNativeResponseJson({
            {"status", "ok"}
            });
    }

    return 0;
}