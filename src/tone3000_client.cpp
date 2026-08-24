#include "tone3000_client.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace ntc::tone3000 {
namespace {
constexpr wchar_t kHost[] = L"tone3000.com";
constexpr wchar_t kUserAgent[] = L"NamToClo-Tone3000/2.8";
constexpr unsigned short kCallbackPort = 17836;
constexpr char kRedirectUri[] = "http://127.0.0.1:17836/callback";

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string urlEncode(const std::string& s) {
    std::ostringstream o;
    o << std::uppercase << std::hex;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o << c;
        else o << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return o.str();
}

std::string base64Url(const std::vector<unsigned char>& in) {
    if (in.empty()) return {};
    DWORD chars = 0;
    CryptBinaryToStringA(in.data(), static_cast<DWORD>(in.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars);
    std::string out(chars, '\0');
    CryptBinaryToStringA(in.data(), static_cast<DWORD>(in.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &chars);
    while (!out.empty() && (out.back() == '\0' || out.back() == '=')) out.pop_back();
    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    return out;
}

bool randomBytes(std::vector<unsigned char>& bytes) {
    return BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string randomBase64Url(size_t count) {
    std::vector<unsigned char> b(count);
    return randomBytes(b) ? base64Url(b) : std::string{};
}

std::string sha256Base64Url(const std::string& text) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLen = 0, result = 0, hashLen = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLen), sizeof(objectLen), &result, 0) != 0 ||
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &result, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0); return {};
    }
    object.resize(objectLen); digest.resize(hashLen);
    if (BCryptCreateHash(alg, &hash, object.data(), objectLen, nullptr, 0, 0) != 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), 0) != 0 ||
        BCryptFinishHash(hash, digest.data(), hashLen, 0) != 0) {
        if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); return {};
    }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    return base64Url(digest);
}

struct HttpResponse { DWORD status = 0; std::string body; };

bool http(const std::wstring& method, const std::wstring& path, const std::string& body,
          const std::wstring& contentType, const std::string& bearer, HttpResponse& out, std::string& error) {
    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) { error = "WinHttpOpen failed"; return false; }
    HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); error = "WinHttpConnect failed"; return false; }
    HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); error = "WinHttpOpenRequest failed"; return false; }
    std::wstring headers;
    if (!contentType.empty()) headers += L"Content-Type: " + contentType + L"\r\n";
    if (!bearer.empty()) headers += L"Authorization: Bearer " + utf8ToWide(bearer) + L"\r\n";
    const BOOL sent = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                         static_cast<DWORD>(headers.size()),
                                         body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        error = "HTTPS request failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false;
    }
    DWORD statusSize = sizeof(out.status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &out.status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        const size_t old = out.body.size(); out.body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, out.body.data() + old, available, &read)) break;
        out.body.resize(old + read);
    }
    WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
    return true;
}

std::string queryParam(const std::string& target, const std::string& key) {
    const auto q = target.find('?'); if (q == std::string::npos) return {};
    std::string needle = key + "=";
    size_t p = q + 1;
    while (p < target.size()) {
        size_t end = target.find('&', p); if (end == std::string::npos) end = target.size();
        if (target.compare(p, needle.size(), needle) == 0) {
            std::string v = target.substr(p + needle.size(), end - p - needle.size());
            std::string decoded;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '%' && i + 2 < v.size()) {
                    const std::string h = v.substr(i + 1, 2); decoded.push_back(static_cast<char>(std::strtoul(h.c_str(), nullptr, 16))); i += 2;
                } else decoded.push_back(v[i] == '+' ? ' ' : v[i]);
            }
            return decoded;
        }
        p = end + 1;
    }
    return {};
}

bool waitForCallback(std::string& target, std::string& error) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { error = "WSAStartup failed"; return false; }
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) { WSACleanup(); error = "Could not create OAuth callback socket"; return false; }
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(kCallbackPort); inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    BOOL exclusive = TRUE; setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(server, 1) == SOCKET_ERROR) {
        closesocket(server); WSACleanup(); error = "Port 17836 is unavailable for the Tone3000 OAuth callback"; return false;
    }
    fd_set set; FD_ZERO(&set); FD_SET(server, &set); timeval tv{180,0};
    if (select(0, &set, nullptr, nullptr, &tv) <= 0) { closesocket(server); WSACleanup(); error = "Tone3000 authorization timed out"; return false; }
    SOCKET client = accept(server, nullptr, nullptr);
    char buf[8192]{}; const int n = recv(client, buf, static_cast<int>(sizeof(buf)-1), 0);
    if (n > 0) {
        std::string req(buf, buf+n); size_t a = req.find(' '), b = a == std::string::npos ? std::string::npos : req.find(' ', a+1);
        if (a != std::string::npos && b != std::string::npos) target = req.substr(a+1, b-a-1);
    }
    const char reply[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n<html><body style='font-family:Segoe UI'><h2>Tone3000 connected.</h2><p>You can close this browser tab and return to NAM to CLO.</p></body></html>";
    send(client, reply, static_cast<int>(sizeof(reply)-1), 0);
    closesocket(client); closesocket(server); WSACleanup();
    if (target.empty()) { error = "Invalid OAuth callback"; return false; }
    return true;
}

std::wstring apiPathFromModelUrl(const std::string& url) {
    const std::string base = "https://tone3000.com";
    if (url.rfind(base, 0) == 0) return utf8ToWide(url.substr(base.size()));
    return utf8ToWide(url);
}

} // namespace

Client::Client(std::string publishableKey) : publishableKey_(std::move(publishableKey)) {}
void Client::setPublishableKey(std::string key) { publishableKey_ = std::move(key); if (publishableKey_.empty()) disconnect(); }

bool Client::authenticateInteractive(std::string& error) {
    if (publishableKey_.rfind("t3k_pub_", 0) != 0) { error = "Enter a valid Tone3000 publishable key (t3k_pub_...)"; return false; }
    const std::string verifier = randomBase64Url(32), state = randomBase64Url(16), challenge = sha256Base64Url(verifier);
    if (verifier.empty() || state.empty() || challenge.empty()) { error = "Could not generate PKCE values"; return false; }
    const std::string url = "https://tone3000.com/api/v1/oauth/authorize?client_id=" + urlEncode(publishableKey_) +
        "&redirect_uri=" + urlEncode(kRedirectUri) + "&response_type=code&code_challenge=" + urlEncode(challenge) +
        "&code_challenge_method=S256&state=" + urlEncode(state) + "&format=nam";

    // Bind/listen must happen before the browser is opened; waitForCallback owns that socket,
    // so launch a tiny listener thread and then open the browser.
    std::string target, listenerError;
    std::thread listener([&]{ waitForCallback(target, listenerError); });
    Sleep(120);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        listener.detach(); error = "Could not open the system browser"; return false;
    }
    listener.join();
    if (!listenerError.empty()) { error = listenerError; return false; }
    if (queryParam(target, "state") != state) { error = "OAuth state mismatch"; return false; }
    if (const auto oauthError = queryParam(target, "error"); !oauthError.empty()) { error = "Tone3000 OAuth error: " + oauthError; return false; }
    const std::string code = queryParam(target, "code");
    if (code.empty()) { error = "Tone3000 did not return an authorization code"; return false; }

    const std::string body = "grant_type=authorization_code&code=" + urlEncode(code) + "&code_verifier=" + urlEncode(verifier) +
        "&redirect_uri=" + urlEncode(kRedirectUri) + "&client_id=" + urlEncode(publishableKey_);
    HttpResponse r;
    if (!http(L"POST", L"/api/v1/oauth/token", body, L"application/x-www-form-urlencoded", {}, r, error)) return false;
    if (r.status < 200 || r.status >= 300) { error = "Token exchange failed: HTTP " + std::to_string(r.status) + " " + r.body; return false; }
    try {
        const auto j = nlohmann::json::parse(r.body);
        tokens_.accessToken = j.value("access_token", ""); tokens_.refreshToken = j.value("refresh_token", "");
        tokens_.expiresAtUnixMs = nowMs() + static_cast<std::int64_t>(j.value("expires_in", 3600)) * 1000;
    } catch (const std::exception& e) { error = std::string("Invalid token response: ") + e.what(); return false; }
    if (tokens_.accessToken.empty()) { error = "Token response contained no access_token"; return false; }
    return true;
}

bool Client::refresh(std::string& error) {
    if (tokens_.refreshToken.empty()) { error = "No refresh token available"; return false; }
    const std::string body = "grant_type=refresh_token&refresh_token=" + urlEncode(tokens_.refreshToken) + "&client_id=" + urlEncode(publishableKey_);
    HttpResponse r; if (!http(L"POST", L"/api/v1/oauth/token", body, L"application/x-www-form-urlencoded", {}, r, error)) return false;
    if (r.status < 200 || r.status >= 300) { error = "Token refresh failed: HTTP " + std::to_string(r.status); disconnect(); return false; }
    try { auto j=nlohmann::json::parse(r.body); tokens_.accessToken=j.value("access_token",""); tokens_.refreshToken=j.value("refresh_token",tokens_.refreshToken); tokens_.expiresAtUnixMs=nowMs()+static_cast<std::int64_t>(j.value("expires_in",3600))*1000; }
    catch (...) { error="Invalid refresh response"; return false; }
    return !tokens_.accessToken.empty();
}

bool Client::ensureAccessToken(std::string& error) {
    if (!connected()) { error = "Connect to Tone3000 first"; return false; }
    if (nowMs() > tokens_.expiresAtUnixMs - 60000) return refresh(error);
    return true;
}

bool Client::searchNamTones(const std::string& query, std::vector<Tone>& tones, std::string& error) {
    if (!ensureAccessToken(error)) return false;
    const std::wstring path = utf8ToWide("/api/v1/tones/search?format=nam&page_size=30&sort=best-match&query=" + urlEncode(query));
    HttpResponse r; if (!http(L"GET", path, {}, {}, tokens_.accessToken, r, error)) return false;
    if (r.status == 401 && refresh(error)) return searchNamTones(query, tones, error);
    if (r.status < 200 || r.status >= 300) { error="Tone search failed: HTTP "+std::to_string(r.status); return false; }
    try {
        auto j=nlohmann::json::parse(r.body); tones.clear();
        for (const auto& x : j.at("data")) { Tone t; t.id=x.value("id",0LL); t.title=x.value("title",""); t.gear=x.value("gear",""); t.license=x.value("license",""); t.modelsCount=x.value("models_count",0); t.downloadsCount=x.value("downloads_count",0); t.favoritesCount=x.value("favorites_count",0); if (x.contains("user")) t.creator=x["user"].value("username",""); tones.push_back(std::move(t)); }
    } catch (const std::exception& e) { error=std::string("Invalid search response: ")+e.what(); return false; }
    return true;
}

bool Client::listModels(std::int64_t toneId, std::vector<Model>& models, std::string& error) {
    if (!ensureAccessToken(error)) return false;
    const std::wstring path=utf8ToWide("/api/v1/models?tone_id="+std::to_string(toneId)+"&page_size=100"); HttpResponse r;
    if (!http(L"GET",path,{}, {},tokens_.accessToken,r,error)) return false;
    if (r.status<200||r.status>=300){error="Model list failed: HTTP "+std::to_string(r.status);return false;}
    try { auto j=nlohmann::json::parse(r.body); models.clear(); for(const auto& x:j.at("data")){ Model m; m.id=x.value("id",0LL);m.toneId=x.value("tone_id",0LL);m.name=x.value("name","");m.size=x.value("size","");m.architectureVersion=x.value("architecture_version","");m.modelUrl=x.value("model_url",""); if(m.modelUrl.size()>=4 && m.modelUrl.substr(m.modelUrl.size()-4)==".nam") models.push_back(std::move(m)); }} catch(const std::exception&e){error=std::string("Invalid models response: ")+e.what();return false;} return true;
}

bool Client::downloadModel(const Model& model, const std::filesystem::path& destination, std::string& error) {
    if (!ensureAccessToken(error)) return false; HttpResponse r;
    if (!http(L"GET",apiPathFromModelUrl(model.modelUrl),{}, {},tokens_.accessToken,r,error)) return false;
    if(r.status<200||r.status>=300){error="Model download failed: HTTP "+std::to_string(r.status);return false;}
    std::error_code ec; std::filesystem::create_directories(destination.parent_path(),ec);
    std::ofstream f(destination,std::ios::binary); if(!f){error="Could not create downloaded NAM file";return false;} f.write(r.body.data(),static_cast<std::streamsize>(r.body.size())); if(!f){error="Could not write downloaded NAM file";return false;} return true;
}

} // namespace ntc::tone3000
