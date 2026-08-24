#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace ntc::tone3000 {

struct Tone {
    std::int64_t id = 0;
    std::string title;
    std::string creator;
    std::string gear;
    std::string license;
    int modelsCount = 0;
    int downloadsCount = 0;
    int favoritesCount = 0;
};

struct Model {
    std::int64_t id = 0;
    std::int64_t toneId = 0;
    std::string name;
    std::string size;
    std::string architectureVersion;
    std::string modelUrl;
};

struct Tokens {
    std::string accessToken;
    std::string refreshToken;
    std::int64_t expiresAtUnixMs = 0;
};

class Client {
public:
    explicit Client(std::string publishableKey = {});
    void setPublishableKey(std::string key);
    const std::string& publishableKey() const noexcept { return publishableKey_; }

    bool connected() const noexcept { return !tokens_.accessToken.empty(); }
    void disconnect() noexcept { tokens_ = {}; }

    // Opens the system browser, receives the OAuth callback on 127.0.0.1:17836,
    // verifies PKCE state and exchanges the authorization code for bearer tokens.
    bool authenticateInteractive(std::string& error);

    bool searchNamTones(const std::string& query, std::vector<Tone>& tones, std::string& error);
    bool listModels(std::int64_t toneId, std::vector<Model>& models, std::string& error);
    bool downloadModel(const Model& model, const std::filesystem::path& destination, std::string& error);

private:
    bool ensureAccessToken(std::string& error);
    bool refresh(std::string& error);

    std::string publishableKey_;
    Tokens tokens_;
};

} // namespace ntc::tone3000
