#include <iostream>
#include <string>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <vector>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#include <shlobj.h> // 用于 SHGetFolderPathW

// 引入 nlohmann/json 单头文件
#include "json.hpp"
using json = nlohmann::json;

// 全局版本号定义
const wchar_t* HOST_VERSION = L"1.2.7";

// 官方预设默认扩展名列表（当 XML 节点缺失或文件不存在时使用）
const std::string DEFAULT_EXT_LIST = ".mp4;.mkv;.asf;.avi;.mpg;.rm;.rmvb;.wmv;.mp3;.wma;.zip;.rar;.7z;.iso;.exe;.msi;.msix";

// 错误码定义
enum LaunchResult {
    LAUNCH_SUCCESS = 0,
    LAUNCH_ERR_REGISTRY = 1,
    LAUNCH_ERR_CREATE_PROCESS = 2
};

// RAII 结构：用于安全管理 WOW64 文件系统重定向的禁用与恢复
class ScopedWow64DisableRedirection {
public:
    ScopedWow64DisableRedirection() : disabled(false), oldVal(NULL) {
        HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
        if (hKernel) {
            typedef BOOL(WINAPI* LPFN_Disable)(PVOID*);
            LPFN_Disable pfnDisable = (LPFN_Disable)GetProcAddress(hKernel, "Wow64DisableWow64FsRedirection");
            if (pfnDisable) {
                disabled = pfnDisable(&oldVal);
            }
        }
    }

    ~ScopedWow64DisableRedirection() {
        if (disabled) {
            HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
            if (hKernel) {
                typedef BOOL(WINAPI* LPFN_Revert)(PVOID);
                LPFN_Revert pfnRevert = (LPFN_Revert)GetProcAddress(hKernel, "Wow64RevertWow64FsRedirection");
                if (pfnRevert) {
                    pfnRevert(oldVal);
                }
            }
        }
    }

private:
    BOOL disabled;
    PVOID oldVal;
};

// 辅助函数：将 wstring 转换为 string (UTF-8/ANSI)
std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_ACP, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(sizeNeeded, 0);
    WideCharToMultiByte(CP_ACP, 0, &wstr[0], (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);
    return strTo;
}

// 动态读取 exe 同级目录下的 ini 控制日志开关
bool IsLoggingEnabled() {
    wchar_t modulePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) > 0) {
        std::wstring exeDir(modulePath);
        size_t lastSlash = exeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            std::wstring iniPath = exeDir.substr(0, lastSlash + 1) + L"BcLauncher.ini";
            UINT enableLog = GetPrivateProfileIntW(L"Logging", L"EnableLog", 0, iniPath.c_str());
            return enableLog != 0;
        }
    }
    return false;
}

// 日志记录函数（基于 INI 配置动态开辟）
void WriteLog(const std::string& logMsg) {
    if (!IsLoggingEnabled()) return;

    std::ofstream logFile("BcLauncher_debug.log", std::ios::app);
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

// 辅助函数：安全获取系统环境变量值，未找到则返回默认值
std::wstring GetSystemEnvVar(const wchar_t* varName, const wchar_t* defaultValue) {
    wchar_t buf[32767] = { 0 };
    DWORD ret = GetEnvironmentVariableW(varName, buf, 32767);
    if (ret > 0 && ret < 32767) {
        return std::wstring(buf);
    }
    return std::wstring(defaultValue);
}

// 构造独立且完全隔绝父进程污染的 Unicode 环境变量块
std::vector<wchar_t> CreateCleanEnvironmentBlock() {
    std::vector<std::wstring> envVars;

    std::wstring sysRoot = GetSystemEnvVar(L"SystemRoot", L"C:\\WINDOWS");
    std::wstring sysDrive = GetSystemEnvVar(L"SystemDrive", L"C:");
    std::wstring comSpec = GetSystemEnvVar(L"ComSpec", L"C:\\WINDOWS\\system32\\cmd.exe");

    envVars.push_back(L"SystemRoot=" + sysRoot);
    envVars.push_back(L"SystemDrive=" + sysDrive);
    envVars.push_back(L"ComSpec=" + comSpec);
    envVars.push_back(L"PROCESSOR_ARCHITECTURE=AMD64");

    std::wstring cleanPath = sysRoot + L"\\system32;" + sysRoot + L";" + sysRoot + L"\\System32\\Wbem;" + sysRoot + L"\\System32\\WindowsPowerShell\\v1.0\\";
    envVars.push_back(L"Path=" + cleanPath);
    envVars.push_back(L"PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");

    std::wstring appData = GetSystemEnvVar(L"APPDATA", L"");
    std::wstring localAppData = GetSystemEnvVar(L"LOCALAPPDATA", L"");
    std::wstring userProfile = GetSystemEnvVar(L"USERPROFILE", L"");
    std::wstring programData = GetSystemEnvVar(L"ProgramData", L"C:\\ProgramData");
    std::wstring tempDir = GetSystemEnvVar(L"TEMP", L"");
    std::wstring tmpDir = GetSystemEnvVar(L"TMP", L"");

    if (!appData.empty())      envVars.push_back(L"APPDATA=" + appData);
    if (!localAppData.empty()) envVars.push_back(L"LOCALAPPDATA=" + localAppData);
    if (!userProfile.empty())  envVars.push_back(L"USERPROFILE=" + userProfile);
    if (!programData.empty())  envVars.push_back(L"ProgramData=" + programData);
    if (!tempDir.empty())      envVars.push_back(L"TEMP=" + tempDir);
    if (!tmpDir.empty())       envVars.push_back(L"TMP=" + tmpDir);

    envVars.push_back(L"OS=Windows_NT");

    std::vector<wchar_t> block;
    for (const auto& var : envVars) {
        block.insert(block.end(), var.begin(), var.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');

    return block;
}

// =========================================================
// 定位 BitComet.xml 并提取扩展名列表
// =========================================================
std::string GetBitCometExtensionList() {
    WriteLog("--- 开始查找 BitComet 配置文件 ---");
    std::wstring xmlPath = L"";

    // 1. 检查便携模式路径
    wchar_t modulePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) > 0) {
        std::wstring currentExeDir(modulePath);
        size_t lastSlash = currentExeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            std::wstring toolsDir = currentExeDir.substr(0, lastSlash);
            size_t secondLastSlash = toolsDir.find_last_of(L"\\/");
            if (secondLastSlash != std::wstring::npos) {
                std::wstring portableXml = toolsDir.substr(0, secondLastSlash + 1) + L"BitComet.xml";
                WriteLog("检测便携版配置文件路径: " + WStringToString(portableXml));
                if (GetFileAttributesW(portableXml.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    xmlPath = portableXml;
                    WriteLog("找到便携版配置文件！");
                }
            }
        }
    }

    // 2. 检查安装版 AppData 路径
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

    // 3. 退回默认列表
    if (xmlPath.empty()) {
        WriteLog("[警告] 未能找到任何 BitComet.xml 配置文件，退回默认扩展名列表。");
        return DEFAULT_EXT_LIST;
    }

    // 4. 读取 XML 文件内容
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
        WriteLog("[提示] XML 文件中缺少 <HttpDownloadIEMonitorFileType> 节点，退回默认扩展名列表。");
        return DEFAULT_EXT_LIST;
    }

    startPos += startTag.length();
    size_t endPos = content.find(endTag, startPos);

    if (endPos == std::string::npos) {
        WriteLog("[错误] XML 格式异常，未找到闭合标签，退回默认扩展名列表。");
        return DEFAULT_EXT_LIST;
    }

    std::string extList = content.substr(startPos, endPos - startPos);
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

    if (res != ERROR_SUCCESS || bufSize <= sizeof(wchar_t) || cmdTemplate[0] == L'\0') {
        WriteLog("[错误] 读取注册表默认键值失败或读取结果为空！");
        return LAUNCH_ERR_REGISTRY;
    }

    WriteLog("成功读取注册表命令模板: " + WStringToString(cmdTemplate));

    std::wstring templateStr(cmdTemplate);
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

    if (!exePath.empty() && GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteLog("[错误] 指定的 Exe 文件不存在或无法访问！路径: " + WStringToString(exePath));
        return LAUNCH_ERR_CREATE_PROCESS;
    }

    // 提取比特彗星 Exe 所在的主目录作为工作目录
    std::wstring workingDir;
    if (!exePath.empty()) {
        size_t lastDirSlash = exePath.find_last_of(L"\\/");
        if (lastDirSlash != std::wstring::npos) {
            workingDir = exePath.substr(0, lastDirSlash);
        }
    }

    std::wstring fullCommandLine;
    if (bcUrl.empty() || bcUrl == "bc://") {
        fullCommandLine = L"\"" + exePath + L"\"";
    }
    else {
        int urlLen = MultiByteToWideChar(CP_ACP, 0, bcUrl.c_str(), -1, NULL, 0);
        std::wstring wBcUrl(urlLen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, bcUrl.c_str(), -1, &wBcUrl[0], urlLen);

        if (!wBcUrl.empty() && wBcUrl.back() == L'\0') {
            wBcUrl.pop_back();
        }

        fullCommandLine = L"\"" + exePath + L"\" --url=\"" + wBcUrl + L"\" --tray";
    }

    WriteLog("拼接后的最终命令行: " + WStringToString(fullCommandLine));

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWDEFAULT;

    wchar_t* cmdBuffer = &fullCommandLine[0];
    const wchar_t* appName = exePath.empty() ? NULL : exePath.c_str();
    const wchar_t* curDir = workingDir.empty() ? NULL : workingDir.c_str();

    // 构造隔离的环境变量块，并增加脱离 Job Object 限制的标志
    std::vector<wchar_t> cleanEnvBlock = CreateCleanEnvironmentBlock();
    DWORD creationFlags = DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;

    BOOL success = FALSE;

    // 唤起进程时临时解除 WOW64 重定向
    {
        ScopedWow64DisableRedirection wow64Scope;

        success = CreateProcessW(
            appName,
            cmdBuffer,
            NULL, NULL,
            FALSE,                 // 禁用句柄继承
            creationFlags,         // 脱离父进程管道/控制台/Job树，并使用 Unicode 环境
            cleanEnvBlock.data(),  // 传入干净的系统环境变量
            curDir,                // 设置正确的根工作目录
            &si, &pi
        );
    }

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
// Native Messaging 标准二进制应答函数（沿用旧版）
// =========================================================
void SendNativeResponse(const std::string& jsonStr) {
    WriteLog("发送响应给浏览器: " + jsonStr);

    (void)_setmode(_fileno(stdout), _O_BINARY);

    uint32_t length = static_cast<uint32_t>(jsonStr.length());
    std::cout.write(reinterpret_cast<const char*>(&length), sizeof(length));
    std::cout.write(jsonStr.c_str(), length);
    std::cout.flush();
}

void SendNativeResponseJson(const json& j) {
    SendNativeResponse(j.dump());
}

std::string ReadNativeMessage() {
    (void)_setmode(_fileno(stdin), _O_BINARY);

    uint32_t length = 0;
    if (!std::cin.read(reinterpret_cast<char*>(&length), sizeof(length))) {
        return "";
    }

    if (length == 0 || length > 1024 * 1024) {
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
    if (argc <= 1) {
        std::wstring msg = L"This is a BitComet Native Messaging Host and cannot be run directly.\nVersion: ";
        msg += HOST_VERSION;

        // 若判断配置开启了日志功能，动态追加 Logging enabled. 提示信息
        if (IsLoggingEnabled()) {
            msg += L"\n\nLogging enabled.";
        }

        MessageBoxW(
            NULL,
            msg.c_str(),
            L"BitComet Native Host",
            MB_OK | MB_ICONINFORMATION | MB_TOPMOST
        );
        return 0;
    }

    WriteLog("================ Host 进程启动 ================");

    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);

    // 1. 读取 Native Message
    std::string jsonInput = ReadNativeMessage();

    if (jsonInput.empty()) {
        WriteLog("[警告] 收到空的 JSON 消息或读取失败！");
        return 0;
    }

    WriteLog("收到来自浏览器的原始 JSON: " + jsonInput);

    // 2. 解析 JSON
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

    std::string action = jInput.value("action", "");
    std::string fileUrl = jInput.value("link", "");
    std::string referer = jInput.value("referurl", jInput.value("referer", ""));
    std::string cookie = jInput.value("cookie", "");
    std::string userAgent = jInput.value("user_agent", "");
    std::string taskName = jInput.value("name", "");

    // 3. 处理 GetConfig 动作
    if (action == "GetConfig") {
        WriteLog("响应动作: GetConfig");
        std::string extList = GetBitCometExtensionList();
        SendNativeResponseJson({
            {"ext_list", extList}
            });
        return 0;
    }

    // 4. 处理 Launch_Program 动作
    if (action == "Launch_Program") {
        WriteLog("响应动作: Launch_Program");
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

    // 8. 唤起比特彗星
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
        SendNativeResponseJson({
            {"status", "ok"}
            });
    }

    return 0;
}