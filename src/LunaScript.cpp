#include "LunaScript.hpp"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <unordered_set>

namespace {
enum class TokenType { Identifier, String, Number, Color, LeftBrace, RightBrace, Semicolon, Equal, End, Invalid };

struct Token {
    TokenType type = TokenType::Invalid;
    std::wstring text;
    int line = 1;
    int column = 1;
};

bool IsIdentifierStart(wchar_t value) {
    return std::iswalpha(value) || value == L'_';
}

bool IsIdentifierPart(wchar_t value) {
    return std::iswalnum(value) || value == L'_' || value == L'-' || value == L'.';
}

class Lexer {
public:
    Lexer(const std::wstring& source, std::vector<LunaDiagnostic>& diagnostics)
        : source_(source), diagnostics_(diagnostics) {}

    Token Next() {
        SkipTrivia();
        Token token;
        token.line = line_;
        token.column = column_;
        if (position_ >= source_.size()) { token.type = TokenType::End; return token; }
        const wchar_t value = source_[position_];
        if (value == L'{') { Advance(); token.type = TokenType::LeftBrace; token.text = L"{"; return token; }
        if (value == L'}') { Advance(); token.type = TokenType::RightBrace; token.text = L"}"; return token; }
        if (value == L';') { Advance(); token.type = TokenType::Semicolon; token.text = L";"; return token; }
        if (value == L'=') { Advance(); token.type = TokenType::Equal; token.text = L"="; return token; }
        if (value == L'"') return ReadString();
        if (value == L'#') return ReadColor();
        if (std::iswdigit(value)) return ReadNumber();
        if (IsIdentifierStart(value)) return ReadIdentifier();
        diagnostics_.push_back({line_, column_, L"Unexpected character in LunaScript"});
        token.text.assign(1, value);
        token.type = TokenType::Invalid;
        Advance();
        return token;
    }

private:
    void Advance() {
        if (position_ >= source_.size()) return;
        if (source_[position_] == L'\n') { ++line_; column_ = 1; }
        else ++column_;
        ++position_;
    }

    void SkipTrivia() {
        for (;;) {
            while (position_ < source_.size() && std::iswspace(source_[position_])) Advance();
            if (position_ + 1 < source_.size() && source_[position_] == L'/' && source_[position_ + 1] == L'/') {
                while (position_ < source_.size() && source_[position_] != L'\n') Advance();
                continue;
            }
            if (position_ + 1 < source_.size() && source_[position_] == L'/' && source_[position_ + 1] == L'*') {
                const int startLine = line_;
                const int startColumn = column_;
                Advance(); Advance();
                while (position_ + 1 < source_.size() && !(source_[position_] == L'*' && source_[position_ + 1] == L'/')) Advance();
                if (position_ + 1 >= source_.size()) {
                    diagnostics_.push_back({startLine, startColumn, L"Unterminated block comment"});
                    return;
                }
                Advance(); Advance();
                continue;
            }
            return;
        }
    }

    Token ReadIdentifier() {
        Token token{TokenType::Identifier, {}, line_, column_};
        const std::size_t start = position_;
        while (position_ < source_.size() && IsIdentifierPart(source_[position_])) Advance();
        token.text = source_.substr(start, position_ - start);
        return token;
    }

    Token ReadNumber() {
        Token token{TokenType::Number, {}, line_, column_};
        const std::size_t start = position_;
        while (position_ < source_.size() && std::iswdigit(source_[position_])) Advance();
        token.text = source_.substr(start, position_ - start);
        return token;
    }

    Token ReadColor() {
        Token token{TokenType::Color, {}, line_, column_};
        Advance();
        const std::size_t start = position_;
        while (position_ < source_.size() && std::iswxdigit(source_[position_])) Advance();
        token.text = source_.substr(start, position_ - start);
        if (token.text.size() != 6) {
            diagnostics_.push_back({token.line, token.column, L"Colors must use #RRGGBB"});
            token.type = TokenType::Invalid;
        }
        return token;
    }

    Token ReadString() {
        Token token{TokenType::String, {}, line_, column_};
        Advance();
        while (position_ < source_.size() && source_[position_] != L'"') {
            wchar_t value = source_[position_];
            if (value == L'\n' || value == L'\r') {
                diagnostics_.push_back({token.line, token.column, L"Strings cannot contain an unescaped newline"});
                token.type = TokenType::Invalid;
                return token;
            }
            if (value == L'\\') {
                Advance();
                if (position_ >= source_.size()) break;
                value = source_[position_];
                switch (value) {
                    case L'n': token.text.push_back(L'\n'); break;
                    case L't': token.text.push_back(L'\t'); break;
                    case L'\\': token.text.push_back(L'\\'); break;
                    case L'"': token.text.push_back(L'"'); break;
                    default:
                        diagnostics_.push_back({line_, column_, L"Unknown string escape"});
                        token.text.push_back(value);
                        break;
                }
                Advance();
                continue;
            }
            token.text.push_back(value);
            Advance();
        }
        if (position_ >= source_.size()) {
            diagnostics_.push_back({token.line, token.column, L"Unterminated string"});
            token.type = TokenType::Invalid;
            return token;
        }
        Advance();
        return token;
    }

    const std::wstring& source_;
    std::vector<LunaDiagnostic>& diagnostics_;
    std::size_t position_ = 0;
    int line_ = 1;
    int column_ = 1;
};

bool ParseColor(const Token& token, COLORREF& color) {
    if (token.type != TokenType::Color || token.text.size() != 6) return false;
    unsigned long value = 0;
    try { value = std::stoul(token.text, nullptr, 16); }
    catch (...) { return false; }
    color = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}

std::uint16_t LocalizationPlaceholderMask(const std::wstring& value) {
    std::uint16_t mask = 0;
    for (std::size_t position = 0; position + 2 < value.size(); ++position) {
        if (value[position] == L'{' && value[position + 1] >= L'0' && value[position + 1] <= L'9' &&
            value[position + 2] == L'}') {
            mask |= static_cast<std::uint16_t>(1u << (value[position + 1] - L'0'));
            position += 2;
        }
    }
    return mask;
}

bool HasAdjacentLocalizationPlaceholders(const std::wstring& value) {
    for (std::size_t position = 0; position + 5 < value.size(); ++position) {
        const bool first = value[position] == L'{' && value[position + 1] >= L'0' &&
            value[position + 1] <= L'9' && value[position + 2] == L'}';
        const bool second = value[position + 3] == L'{' && value[position + 4] >= L'0' &&
            value[position + 4] <= L'9' && value[position + 5] == L'}';
        if (first && second) return true;
    }
    return false;
}

void ApplyThemeAppearanceDefaults(LunaThemeDefinition& theme, LunaThemeAppearance appearance) {
    theme.appearance = appearance;
    if (appearance == LunaThemeAppearance::Light) {
        theme.background = RGB(245, 247, 251);
        theme.backgroundSecondary = RGB(224, 232, 246);
        theme.sidebar = RGB(237, 241, 247);
        theme.topbar = RGB(250, 251, 253);
        theme.surface = RGB(255, 255, 255);
        theme.surfaceRaised = RGB(246, 248, 252);
        theme.surfaceHover = RGB(235, 240, 248);
        theme.surfacePressed = RGB(224, 232, 244);
        theme.surfaceSelected = RGB(218, 232, 252);
        theme.input = RGB(250, 251, 253);
        theme.border = RGB(205, 213, 226);
        theme.borderHover = RGB(156, 171, 194);
        theme.focus = RGB(49, 112, 213);
        theme.text = RGB(25, 31, 42);
        theme.textSecondary = RGB(67, 77, 94);
        theme.muted = RGB(102, 113, 132);
        theme.disabled = RGB(151, 160, 175);
        theme.accent = RGB(48, 111, 214);
        theme.accentHover = RGB(36, 91, 181);
        theme.accentPressed = RGB(27, 73, 151);
        theme.accentSecondary = RGB(126, 78, 207);
        theme.textOnAccent = RGB(255, 255, 255);
        theme.info = RGB(25, 132, 187);
        theme.success = RGB(25, 143, 86);
        theme.warning = RGB(188, 119, 20);
        theme.danger = RGB(205, 55, 72);
        theme.shadow = RGB(46, 55, 70);
    } else if (appearance == LunaThemeAppearance::Amoled) {
        theme.background = RGB(0, 0, 0);
        theme.backgroundSecondary = RGB(8, 8, 12);
        theme.sidebar = RGB(4, 4, 6);
        theme.topbar = RGB(6, 6, 9);
        theme.surface = RGB(10, 10, 13);
        theme.surfaceRaised = RGB(18, 18, 23);
        theme.surfaceHover = RGB(27, 27, 34);
        theme.surfacePressed = RGB(14, 14, 18);
        theme.surfaceSelected = RGB(18, 35, 60);
        theme.input = RGB(7, 7, 10);
        theme.border = RGB(33, 33, 42);
        theme.borderHover = RGB(70, 73, 88);
        theme.focus = RGB(117, 178, 255);
        theme.text = RGB(250, 250, 252);
        theme.textSecondary = RGB(196, 198, 207);
        theme.muted = RGB(130, 133, 145);
        theme.disabled = RGB(76, 78, 89);
        theme.accent = RGB(85, 154, 255);
        theme.accentHover = RGB(118, 177, 255);
        theme.accentPressed = RGB(53, 120, 224);
        theme.accentSecondary = RGB(180, 108, 255);
        theme.textOnAccent = RGB(255, 255, 255);
        theme.info = RGB(67, 190, 242);
        theme.success = RGB(67, 218, 139);
        theme.warning = RGB(255, 188, 64);
        theme.danger = RGB(255, 92, 116);
        theme.shadow = RGB(0, 0, 0);
    } else {
        theme.background = RGB(14, 16, 20);
        theme.backgroundSecondary = RGB(24, 31, 48);
        theme.sidebar = RGB(16, 19, 24);
        theme.topbar = RGB(18, 21, 27);
        theme.surface = RGB(20, 23, 29);
        theme.surfaceRaised = RGB(29, 35, 44);
        theme.surfaceHover = RGB(35, 42, 53);
        theme.surfacePressed = RGB(24, 29, 38);
        theme.surfaceSelected = RGB(32, 47, 70);
        theme.input = RGB(18, 22, 28);
        theme.border = RGB(39, 44, 54);
        theme.borderHover = RGB(63, 74, 91);
        theme.focus = RGB(123, 177, 255);
        theme.text = RGB(244, 246, 251);
        theme.textSecondary = RGB(190, 197, 210);
        theme.muted = RGB(137, 144, 158);
        theme.disabled = RGB(87, 94, 107);
        theme.accent = RGB(96, 149, 244);
        theme.accentHover = RGB(119, 171, 250);
        theme.accentPressed = RGB(75, 126, 219);
        theme.accentSecondary = RGB(164, 118, 247);
        theme.textOnAccent = RGB(255, 255, 255);
        theme.info = RGB(83, 180, 235);
        theme.success = RGB(78, 205, 139);
        theme.warning = RGB(246, 180, 72);
        theme.danger = RGB(243, 112, 126);
        theme.shadow = RGB(0, 0, 0);
    }
    theme.backdropColor = theme.accent;
    theme.backdropColorSecondary = theme.accentSecondary;
    theme.backgroundImageTint = theme.accent;
}

class Parser {
public:
    Parser(const std::wstring& source, std::vector<LunaDiagnostic>& diagnostics)
        : lexer_(source, diagnostics), diagnostics_(diagnostics) { Advance(); }

    bool Parse(LunaPluginDefinition& plugin) {
        plugin = {};
        if (!TakeIdentifier(L"luna")) return FailHere(L"A LunaScript file must start with: luna 1;");
        int languageVersion = 0;
        if (!TakeNumber(languageVersion)) return FailHere(L"Expected LunaScript language version");
        plugin.languageVersion = languageVersion;
        OptionalSemicolon();
        if (languageVersion != 1) AddError(L"This Luna Capture build supports LunaScript language version 1");
        if (!TakeIdentifier(L"plugin")) return FailHere(L"Expected plugin declaration");
        if (!TakeString(plugin.id)) return FailHere(L"Expected quoted plugin id");
        if (!Take(TokenType::LeftBrace)) return FailHere(L"Expected '{' after plugin id");
        while (current_.type != TokenType::RightBrace && current_.type != TokenType::End) {
            if (!ParsePluginMember(plugin)) RecoverMember();
            if (plugin.commands.size() > 64 || plugin.events.size() > 64) {
                AddError(L"A plugin may define at most 64 commands and 64 event handlers");
                break;
            }
        }
        if (!Take(TokenType::RightBrace)) AddError(L"Expected '}' at the end of the plugin");
        if (current_.type != TokenType::End) AddError(L"Unexpected content after plugin block");
        Validate(plugin);
        return diagnostics_.empty();
    }

private:
    void Advance() { current_ = lexer_.Next(); }
    bool Take(TokenType type) { if (current_.type != type) return false; Advance(); return true; }
    bool TakeIdentifier(const wchar_t* value) {
        if (current_.type != TokenType::Identifier || current_.text != value) return false;
        Advance(); return true;
    }
    bool TakeString(std::wstring& output) {
        if (current_.type != TokenType::String) return false;
        output = current_.text; Advance(); return true;
    }
    bool TakeNumber(int& output) {
        if (current_.type != TokenType::Number) return false;
        try {
            const unsigned long parsed = std::stoul(current_.text);
            if (parsed > static_cast<unsigned long>((std::numeric_limits<int>::max)())) return false;
            output = static_cast<int>(parsed);
        } catch (...) { return false; }
        Advance(); return true;
    }
    bool TakeToggle(int& output) {
        if (current_.type != TokenType::Identifier || (current_.text != L"on" && current_.text != L"off")) return false;
        output = current_.text == L"on" ? 1 : 0;
        Advance();
        return true;
    }
    void OptionalSemicolon() { if (current_.type == TokenType::Semicolon) Advance(); }
    void AddError(const std::wstring& message) { diagnostics_.push_back({current_.line, current_.column, message}); }
    bool FailHere(const std::wstring& message) { AddError(message); return false; }
    void MarkHostApi(int version, int line) {
        if (version <= minimumHostApiUsed_) return;
        minimumHostApiUsed_ = version;
        minimumHostApiLine_ = line;
    }

    bool ParsePluginMember(LunaPluginDefinition& plugin) {
        if (current_.type != TokenType::Identifier) return FailHere(L"Expected a plugin property, permission, theme, locale, command, or event");
        const Token member = current_;
        Advance();
        if (member.text == L"name") return ParseStringProperty(plugin.name);
        if (member.text == L"version") return ParseStringProperty(plugin.version);
        if (member.text == L"author") return ParseStringProperty(plugin.author);
        if (member.text == L"description") return ParseStringProperty(plugin.description);
        if (member.text == L"homepage") return ParseStringProperty(plugin.homepage);
        if (member.text == L"requires") return ParseRequirement(plugin);
        if (member.text == L"permission") return ParsePermission(plugin);
        if (member.text == L"theme") return ParseTheme(plugin);
        if (member.text == L"locale") return ParseLocale(plugin);
        if (member.text == L"command") return ParseCommand(plugin);
        if (member.text == L"on") return ParseEvent(plugin);
        return FailHere(L"Unknown plugin member: " + member.text);
    }

    bool ParseStringProperty(std::wstring& destination) {
        if (current_.type == TokenType::Equal) Advance();
        if (!TakeString(destination)) return FailHere(L"Expected a quoted string value");
        OptionalSemicolon();
        return true;
    }

    bool ParseRequirement(LunaPluginDefinition& plugin) {
        if (plugin.hostApiRequirementPresent) return FailHere(L"A plugin may declare requires host_api only once");
        if (!TakeIdentifier(L"host_api")) return FailHere(L"requires currently supports only: requires host_api <version>;");
        if (current_.type == TokenType::Equal) Advance();
        int version = 0;
        if (!TakeNumber(version) || version < 1) return FailHere(L"requires host_api needs a positive integer version");
        plugin.requiredHostApi = version;
        plugin.hostApiRequirementPresent = true;
        OptionalSemicolon();
        return true;
    }

    bool ParsePermission(LunaPluginDefinition& plugin) {
        if (current_.type != TokenType::Identifier) return FailHere(L"Expected a permission name");
        const std::wstring name = current_.text;
        const int permissionLine = current_.line;
        Advance();
        OptionalSemicolon();
        LunaPermission permission{};
        if (name == L"theme") permission = LunaPermission::Theme;
        else if (name == L"notifications") permission = LunaPermission::Notifications;
        else if (name == L"capture") permission = LunaPermission::Capture;
        else if (name == L"recording") permission = LunaPermission::Recording;
        else if (name == L"replay") permission = LunaPermission::Replay;
        else if (name == L"audio") permission = LunaPermission::Audio;
        else if (name == L"settings") permission = LunaPermission::Settings;
        else if (name == L"localization") permission = LunaPermission::Localization;
        else if (name == L"open_folders") permission = LunaPermission::OpenFolders;
        else if (name == L"diagnostics") permission = LunaPermission::Diagnostics;
        else if (name == L"ui") {
            permission = LunaPermission::Ui;
            MarkHostApi(5, permissionLine);
        }
        else return FailHere(L"Unknown permission: " + name);
        if (std::find(plugin.permissions.begin(), plugin.permissions.end(), permission) == plugin.permissions.end()) {
            plugin.permissions.push_back(permission);
        }
        return true;
    }

    bool ParseTheme(LunaPluginDefinition& plugin) {
        if (plugin.theme.present) return FailHere(L"A plugin can define only one theme");
        plugin.theme.present = true;
        if (!TakeString(plugin.theme.name)) return FailHere(L"Expected a quoted theme name");
        if (!Take(TokenType::LeftBrace)) return FailHere(L"Expected '{' after theme name");
        std::unordered_set<std::wstring> properties;
        while (current_.type != TokenType::RightBrace && current_.type != TokenType::End) {
            if (current_.type != TokenType::Identifier) { AddError(L"Expected a theme property"); RecoverMember(); continue; }
            const std::wstring key = current_.text;
            const int propertyLine = current_.line;
            Advance();
            if (!properties.insert(key).second) AddError(L"Duplicate theme property: " + key);
            if (current_.type == TokenType::Equal) Advance();

            static const std::unordered_set<std::wstring> apiFiveProperties{
                L"appearance", L"sidebar", L"topbar", L"surface_hover", L"surface_pressed",
                L"surface_selected", L"input", L"border_hover", L"focus", L"text_secondary",
                L"disabled", L"accent_pressed", L"accent_secondary", L"text_on_accent", L"info",
                L"shadow", L"border_scale", L"font_scale", L"font_weight", L"font_italic",
                L"font_family", L"saturation", L"contrast", L"brightness", L"motion_scale",
                L"gradient_style", L"backdrop_scope",
                L"backdrop_color", L"backdrop_color_secondary", L"backdrop_thickness",
                L"backdrop_speed", L"backdrop_seed", L"gradient_angle", L"gradient_center_x",
                L"gradient_center_y", L"gradient_spread", L"vignette_color", L"vignette_opacity",
                L"noise_opacity", L"background_image_position_x", L"background_image_position_y",
                L"background_image_scale", L"background_image_tint", L"background_image_tint_opacity",
                L"background_image_brightness", L"background_image_saturation",
            };
            if (apiFiveProperties.contains(key)) MarkHostApi(5, propertyLine);

            if (key == L"appearance") {
                if (properties.size() != 1) AddError(L"appearance must be the first theme property so later colors can override its defaults");
                if (current_.type != TokenType::Identifier) {
                    AddError(L"appearance requires dark, light, or amoled");
                } else {
                    if (current_.text == L"dark") ApplyThemeAppearanceDefaults(plugin.theme, LunaThemeAppearance::Dark);
                    else if (current_.text == L"light") ApplyThemeAppearanceDefaults(plugin.theme, LunaThemeAppearance::Light);
                    else if (current_.text == L"amoled") ApplyThemeAppearanceDefaults(plugin.theme, LunaThemeAppearance::Amoled);
                    else AddError(L"appearance must be dark, light, or amoled");
                    Advance();
                }
                OptionalSemicolon();
                continue;
            }

            if (key == L"backdrop" || key == L"gradient_style" || key == L"backdrop_scope" ||
                key == L"background_image_mode") {
                if (current_.type != TokenType::Identifier) {
                    AddError(key + L" requires an identifier value");
                } else if (key == L"backdrop") {
                    static const std::unordered_set<std::wstring> apiFivePatterns{
                        L"diagonal", L"waves", L"rings", L"checker", L"hexagons", L"stars",
                        L"circuit", L"scanlines", L"noise", L"mesh",
                    };
                    if (apiFivePatterns.contains(current_.text)) MarkHostApi(5, propertyLine);
                    if (current_.text == L"solid") plugin.theme.backdropPattern = LunaBackdropPattern::Solid;
                    else if (current_.text == L"gradient") plugin.theme.backdropPattern = LunaBackdropPattern::Gradient;
                    else if (current_.text == L"grid") plugin.theme.backdropPattern = LunaBackdropPattern::Grid;
                    else if (current_.text == L"dots") plugin.theme.backdropPattern = LunaBackdropPattern::Dots;
                    else if (current_.text == L"aurora") plugin.theme.backdropPattern = LunaBackdropPattern::Aurora;
                    else if (current_.text == L"diagonal") plugin.theme.backdropPattern = LunaBackdropPattern::Diagonal;
                    else if (current_.text == L"waves") plugin.theme.backdropPattern = LunaBackdropPattern::Waves;
                    else if (current_.text == L"rings") plugin.theme.backdropPattern = LunaBackdropPattern::Rings;
                    else if (current_.text == L"checker") plugin.theme.backdropPattern = LunaBackdropPattern::Checker;
                    else if (current_.text == L"hexagons") plugin.theme.backdropPattern = LunaBackdropPattern::Hexagons;
                    else if (current_.text == L"stars") plugin.theme.backdropPattern = LunaBackdropPattern::Stars;
                    else if (current_.text == L"circuit") plugin.theme.backdropPattern = LunaBackdropPattern::Circuit;
                    else if (current_.text == L"scanlines") plugin.theme.backdropPattern = LunaBackdropPattern::Scanlines;
                    else if (current_.text == L"noise") plugin.theme.backdropPattern = LunaBackdropPattern::Noise;
                    else if (current_.text == L"mesh") plugin.theme.backdropPattern = LunaBackdropPattern::Mesh;
                    else AddError(L"Unknown backdrop; use solid, gradient, grid, dots, aurora, diagonal, waves, rings, checker, hexagons, stars, circuit, scanlines, noise, or mesh");
                    Advance();
                } else if (key == L"gradient_style") {
                    if (current_.text == L"linear") plugin.theme.gradientStyle = LunaGradientStyle::Linear;
                    else if (current_.text == L"radial") plugin.theme.gradientStyle = LunaGradientStyle::Radial;
                    else AddError(L"gradient_style must be linear or radial");
                    Advance();
                } else if (key == L"backdrop_scope") {
                    if (current_.text == L"main") plugin.theme.backdropScope = LunaBackdropScope::Main;
                    else if (current_.text == L"panels") plugin.theme.backdropScope = LunaBackdropScope::Panels;
                    else if (current_.text == L"all") plugin.theme.backdropScope = LunaBackdropScope::All;
                    else AddError(L"backdrop_scope must be main, panels, or all");
                    Advance();
                } else {
                    if (current_.text == L"stretch" || current_.text == L"center") {
                        MarkHostApi(5, propertyLine);
                    }
                    if (current_.text == L"cover") plugin.theme.backgroundImageMode = LunaBackgroundImageMode::Cover;
                    else if (current_.text == L"contain") plugin.theme.backgroundImageMode = LunaBackgroundImageMode::Contain;
                    else if (current_.text == L"tile") plugin.theme.backgroundImageMode = LunaBackgroundImageMode::Tile;
                    else if (current_.text == L"stretch") plugin.theme.backgroundImageMode = LunaBackgroundImageMode::Stretch;
                    else if (current_.text == L"center") plugin.theme.backgroundImageMode = LunaBackgroundImageMode::Center;
                    else AddError(L"background_image_mode must be cover, contain, tile, stretch, or center");
                    Advance();
                }
                OptionalSemicolon();
                continue;
            }

            if (key == L"font_italic") {
                int enabled = 0;
                if (!TakeToggle(enabled)) AddError(L"font_italic requires on or off");
                else plugin.theme.fontItalic = enabled != 0;
                OptionalSemicolon();
                continue;
            }

            if (key == L"font_family" || key == L"background_image") {
                std::wstring value;
                if (!TakeString(value)) {
                    AddError(key + L" requires a quoted string");
                } else if (key == L"font_family") {
                    if (value.empty() || value.size() > 31) AddError(L"font_family must contain 1 to 31 characters");
                    else plugin.theme.fontFamily = std::move(value);
                } else {
                    if (value.empty() || value.size() > 260) AddError(L"background_image path must contain 1 to 260 characters");
                    else plugin.theme.backgroundImage = std::move(value);
                }
                OptionalSemicolon();
                continue;
            }

            int* numberDestination = nullptr;
            int minimum = 0;
            int maximum = 0;
            if (key == L"corner_scale") { numberDestination = &plugin.theme.cornerScalePercent; minimum = 50; maximum = 160; }
            else if (key == L"border_scale") { numberDestination = &plugin.theme.borderScalePercent; minimum = 50; maximum = 250; }
            else if (key == L"font_scale") { numberDestination = &plugin.theme.fontScalePercent; minimum = 75; maximum = 150; }
            else if (key == L"font_weight") { numberDestination = &plugin.theme.fontWeight; minimum = 100; maximum = 900; }
            else if (key == L"saturation") { numberDestination = &plugin.theme.saturationPercent; minimum = 0; maximum = 200; }
            else if (key == L"contrast") { numberDestination = &plugin.theme.contrastPercent; minimum = 50; maximum = 200; }
            else if (key == L"brightness") { numberDestination = &plugin.theme.brightnessPercent; minimum = 25; maximum = 200; }
            else if (key == L"motion_scale") { numberDestination = &plugin.theme.motionScalePercent; minimum = 0; maximum = 200; }
            else if (key == L"backdrop_opacity") { numberDestination = &plugin.theme.backdropOpacityPercent; minimum = 0; maximum = 100; }
            else if (key == L"backdrop_scale") { numberDestination = &plugin.theme.backdropScale; minimum = 8; maximum = 192; }
            else if (key == L"backdrop_thickness") { numberDestination = &plugin.theme.backdropThickness; minimum = 1; maximum = 8; }
            else if (key == L"backdrop_speed") { numberDestination = &plugin.theme.backdropSpeedPercent; minimum = 0; maximum = 200; }
            else if (key == L"backdrop_seed") { numberDestination = &plugin.theme.backdropSeed; minimum = 0; maximum = 65535; }
            else if (key == L"gradient_angle") { numberDestination = &plugin.theme.gradientAngle; minimum = 0; maximum = 359; }
            else if (key == L"gradient_center_x") { numberDestination = &plugin.theme.gradientCenterXPercent; minimum = 0; maximum = 100; }
            else if (key == L"gradient_center_y") { numberDestination = &plugin.theme.gradientCenterYPercent; minimum = 0; maximum = 100; }
            else if (key == L"gradient_spread") { numberDestination = &plugin.theme.gradientSpreadPercent; minimum = 25; maximum = 200; }
            else if (key == L"vignette_opacity") { numberDestination = &plugin.theme.vignetteOpacityPercent; minimum = 0; maximum = 90; }
            else if (key == L"noise_opacity") { numberDestination = &plugin.theme.noiseOpacityPercent; minimum = 0; maximum = 40; }
            else if (key == L"background_image_opacity") { numberDestination = &plugin.theme.backgroundImageOpacityPercent; minimum = 0; maximum = 100; }
            else if (key == L"background_image_position_x") { numberDestination = &plugin.theme.backgroundImagePositionXPercent; minimum = 0; maximum = 100; }
            else if (key == L"background_image_position_y") { numberDestination = &plugin.theme.backgroundImagePositionYPercent; minimum = 0; maximum = 100; }
            else if (key == L"background_image_scale") { numberDestination = &plugin.theme.backgroundImageScalePercent; minimum = 25; maximum = 400; }
            else if (key == L"background_image_tint_opacity") { numberDestination = &plugin.theme.backgroundImageTintOpacityPercent; minimum = 0; maximum = 100; }
            else if (key == L"background_image_brightness") { numberDestination = &plugin.theme.backgroundImageBrightnessPercent; minimum = 25; maximum = 200; }
            else if (key == L"background_image_saturation") { numberDestination = &plugin.theme.backgroundImageSaturationPercent; minimum = 0; maximum = 200; }
            if (numberDestination) {
                int value = 0;
                if (!TakeNumber(value)) AddError(key + L" requires an integer from " + std::to_wstring(minimum) + L" to " + std::to_wstring(maximum));
                else if (value < minimum || value > maximum) AddError(key + L" must be from " + std::to_wstring(minimum) + L" to " + std::to_wstring(maximum));
                else *numberDestination = value;
                OptionalSemicolon();
                continue;
            }

            COLORREF* destination = nullptr;
            if (key == L"background") destination = &plugin.theme.background;
            else if (key == L"background_secondary") destination = &plugin.theme.backgroundSecondary;
            else if (key == L"sidebar") destination = &plugin.theme.sidebar;
            else if (key == L"topbar") destination = &plugin.theme.topbar;
            else if (key == L"surface") destination = &plugin.theme.surface;
            else if (key == L"surface_raised") destination = &plugin.theme.surfaceRaised;
            else if (key == L"surface_hover") destination = &plugin.theme.surfaceHover;
            else if (key == L"surface_pressed") destination = &plugin.theme.surfacePressed;
            else if (key == L"surface_selected") destination = &plugin.theme.surfaceSelected;
            else if (key == L"input") destination = &plugin.theme.input;
            else if (key == L"border") destination = &plugin.theme.border;
            else if (key == L"border_hover") destination = &plugin.theme.borderHover;
            else if (key == L"focus") destination = &plugin.theme.focus;
            else if (key == L"text") destination = &plugin.theme.text;
            else if (key == L"text_secondary") destination = &plugin.theme.textSecondary;
            else if (key == L"muted") destination = &plugin.theme.muted;
            else if (key == L"disabled") destination = &plugin.theme.disabled;
            else if (key == L"accent") destination = &plugin.theme.accent;
            else if (key == L"accent_hover") destination = &plugin.theme.accentHover;
            else if (key == L"accent_pressed") destination = &plugin.theme.accentPressed;
            else if (key == L"accent_secondary") destination = &plugin.theme.accentSecondary;
            else if (key == L"text_on_accent") destination = &plugin.theme.textOnAccent;
            else if (key == L"info") destination = &plugin.theme.info;
            else if (key == L"success") destination = &plugin.theme.success;
            else if (key == L"warning") destination = &plugin.theme.warning;
            else if (key == L"danger") destination = &plugin.theme.danger;
            else if (key == L"shadow") destination = &plugin.theme.shadow;
            else if (key == L"backdrop_color") destination = &plugin.theme.backdropColor;
            else if (key == L"backdrop_color_secondary") destination = &plugin.theme.backdropColorSecondary;
            else if (key == L"vignette_color") destination = &plugin.theme.vignetteColor;
            else if (key == L"background_image_tint") destination = &plugin.theme.backgroundImageTint;
            if (!destination) {
                AddError(L"Unknown theme property: " + key);
                RecoverMember();
                continue;
            }
            if (!ParseColor(current_, *destination)) AddError(key + L" requires a #RRGGBB color");
            if (current_.type != TokenType::End) Advance();
            OptionalSemicolon();
        }
        if (!Take(TokenType::RightBrace)) return FailHere(L"Expected '}' after theme");
        return true;
    }

    bool ParseLocale(LunaPluginDefinition& plugin) {
        if (plugin.locale.present) return FailHere(L"A plugin can define only one locale block");
        plugin.locale.present = true;
        if (!TakeString(plugin.locale.name)) return FailHere(L"Expected a quoted locale name, for example ru-RU");
        if (!Take(TokenType::LeftBrace)) return FailHere(L"Expected '{' after locale name");
        while (current_.type != TokenType::RightBrace && current_.type != TokenType::End) {
            LunaTranslation translation;
            if (!TakeString(translation.source)) { AddError(L"Locale entries start with the exact quoted host text"); RecoverMember(); continue; }
            if (current_.type == TokenType::Equal) Advance();
            if (!TakeString(translation.replacement)) { AddError(L"Locale entries require a quoted replacement"); RecoverMember(); continue; }
            OptionalSemicolon();
            if (translation.source.size() > 512 || translation.replacement.size() > 512) {
                AddError(L"Locale source and replacement strings are limited to 512 characters");
            } else {
                plugin.locale.translations.push_back(std::move(translation));
            }
            if (plugin.locale.translations.size() > 1024) {
                AddError(L"A locale may contain at most 1024 translated strings");
                break;
            }
        }
        if (!Take(TokenType::RightBrace)) return FailHere(L"Expected '}' after locale");
        return true;
    }

    bool ParseCommand(LunaPluginDefinition& plugin) {
        LunaCommandDefinition command;
        if (!TakeString(command.id)) return FailHere(L"Expected quoted command id");
        if (!Take(TokenType::LeftBrace)) return FailHere(L"Expected '{' after command id");
        std::unordered_set<std::wstring> properties;
        while (current_.type != TokenType::RightBrace && current_.type != TokenType::End) {
            if (current_.type != TokenType::Identifier) { AddError(L"Expected a command property or run block"); RecoverMember(); continue; }
            const std::wstring key = current_.text;
            Advance();
            if (key != L"surface" && !properties.insert(key).second) AddError(L"Duplicate command property: " + key);
            if (key == L"title") ParseStringProperty(command.title);
            else if (key == L"description") ParseStringProperty(command.description);
            else if (key == L"icon") ParseStringProperty(command.icon);
            else if (key == L"surface") {
                if (current_.type != TokenType::Identifier) {
                    AddError(L"surface requires quick_panel or tray");
                    RecoverMember();
                    continue;
                }
                LunaCommandSurface surface{};
                if (current_.text == L"quick_panel") surface = LunaCommandSurface::QuickPanel;
                else if (current_.text == L"tray") surface = LunaCommandSurface::TrayMenu;
                else {
                    AddError(L"Unknown command surface: " + current_.text);
                    Advance();
                    OptionalSemicolon();
                    continue;
                }
                Advance();
                OptionalSemicolon();
                if (std::find(command.surfaces.begin(), command.surfaces.end(), surface) == command.surfaces.end()) {
                    command.surfaces.push_back(surface);
                }
            }
            else if (key == L"order") {
                if (current_.type == TokenType::Equal) Advance();
                int order = 0;
                if (!TakeNumber(order) || order < 0 || order > 1000) AddError(L"Command order must be an integer from 0 to 1000");
                else command.order = order;
                OptionalSemicolon();
            }
            else if (key == L"run") ParseInstructionBlock(command.instructions);
            else { AddError(L"Unknown command property: " + key); RecoverMember(); }
        }
        if (!Take(TokenType::RightBrace)) return FailHere(L"Expected '}' after command");
        if (command.instructions.empty()) AddError(L"Every command requires one non-empty run block");
        plugin.commands.push_back(std::move(command));
        return true;
    }

    bool ParseEvent(LunaPluginDefinition& plugin) {
        if (current_.type != TokenType::Identifier) return FailHere(L"Expected event name after 'on'");
        LunaEventDefinition event;
        event.eventName = current_.text;
        const int eventLine = current_.line;
        Advance();
        static const std::unordered_set<std::wstring> apiFiveEvents{
            L"capture_queued", L"capture_cancelled", L"capture_copied", L"recording_stopped",
            L"recording_failed", L"recording_recovering", L"recording_recovered", L"replay_failed",
            L"replay_recovering", L"replay_recovered", L"plugins_reloaded",
        };
        if (apiFiveEvents.contains(event.eventName)) MarkHostApi(5, eventLine);
        if (event.eventName == L"timer") {
            if (!TakeIdentifier(L"every")) return FailHere(L"Timer events require: on timer every <seconds> { ... }");
            int seconds = 0;
            if (!TakeNumber(seconds)) return FailHere(L"on timer every requires an integer number of seconds");
            if (seconds < 5 || seconds > 86400) return FailHere(L"on timer every must be between 5 and 86400 seconds");
            event.intervalSeconds = seconds;
        }
        if (!ParseInstructionBlock(event.instructions)) return false;
        plugin.events.push_back(std::move(event));
        return true;
    }

    bool ParseInstructionBlock(std::vector<LunaInstruction>& instructions) {
        if (!Take(TokenType::LeftBrace)) return FailHere(L"Expected '{' before action block");
        while (current_.type != TokenType::RightBrace && current_.type != TokenType::End) {
            if (instructions.size() >= 128) { AddError(L"An action block may contain at most 128 instructions"); RecoverMember(); continue; }
            LunaInstruction instruction;
            instruction.line = current_.line;
            if (!ParseInstruction(instruction)) { RecoverMember(); continue; }
            instructions.push_back(std::move(instruction));
        }
        return Take(TokenType::RightBrace) || FailHere(L"Expected '}' after action block");
    }

    bool ParseInstruction(LunaInstruction& instruction) {
        if (current_.type != TokenType::Identifier) return FailHere(L"Expected an action instruction");
        const std::wstring action = current_.text;
        Advance();
        if (action == L"notify") {
            instruction.type = LunaInstructionType::Notify;
            if (!TakeString(instruction.first) || !TakeString(instruction.second)) return FailHere(L"notify requires title and detail strings");
        } else if (action == L"log") {
            instruction.type = LunaInstructionType::Log;
            if (!TakeString(instruction.first)) return FailHere(L"log requires a string");
        } else {
            if (current_.type != TokenType::Identifier) return FailHere(L"Expected an action target");
            const std::wstring target = current_.text;
            Advance();
            if (action == L"setting") {
                if (target == L"preview") instruction.type = LunaInstructionType::SettingPreview;
                else if (target == L"cursor") instruction.type = LunaInstructionType::SettingCursor;
                else if (target == L"notifications") instruction.type = LunaInstructionType::SettingNotifications;
                else if (target == L"sound") instruction.type = LunaInstructionType::SettingSound;
                else if (target == L"preview_fps") instruction.type = LunaInstructionType::SettingPreviewFps;
                else if (target == L"replay_seconds") instruction.type = LunaInstructionType::SettingReplaySeconds;
                else if (target == L"recording_fps") instruction.type = LunaInstructionType::SettingRecordingFps;
                else if (target == L"video_bitrate") instruction.type = LunaInstructionType::SettingVideoBitrate;
                else if (target == L"recording_height") instruction.type = LunaInstructionType::SettingRecordingHeight;
                else if (target == L"codec") instruction.type = LunaInstructionType::SettingVideoCodec;
                else if (target == L"image_format") instruction.type = LunaInstructionType::SettingImageFormat;
                else if (target == L"jpeg_quality") instruction.type = LunaInstructionType::SettingJpegQuality;
                else if (target == L"output_width") instruction.type = LunaInstructionType::SettingOutputWidth;
                else if (target == L"output_height") instruction.type = LunaInstructionType::SettingOutputHeight;
                else if (target == L"copy_region") instruction.type = LunaInstructionType::SettingCopyRegion;
                else if (target == L"overlay_duration") instruction.type = LunaInstructionType::SettingOverlayDuration;
                else if (target == L"replay_preview") instruction.type = LunaInstructionType::SettingReplayPreview;
                else if (target == L"system_audio") instruction.type = LunaInstructionType::SettingSystemAudio;
                else if (target == L"system_audio_volume") instruction.type = LunaInstructionType::SettingSystemAudioVolume;
                else if (target == L"application_audio") instruction.type = LunaInstructionType::SettingApplicationAudio;
                else if (target == L"application_audio_volume") instruction.type = LunaInstructionType::SettingApplicationAudioVolume;
                else if (target == L"audio_bitrate") instruction.type = LunaInstructionType::SettingAudioBitrate;
                else return FailHere(L"Unknown setting target: " + target);
                if (target == L"preview" || target == L"cursor" || target == L"notifications" || target == L"sound" ||
                    target == L"copy_region" || target == L"replay_preview" || target == L"system_audio" ||
                    target == L"application_audio") {
                    if (!TakeToggle(instruction.value)) return FailHere(L"This setting requires on or off");
                } else if (target == L"codec") {
                    // Keep in step with VideoCodec in Types.hpp; the value is the
                    // enum ordinal the host clamps against kVideoCodecCount.
                    static const std::pair<const wchar_t*, int> kCodecNames[] = {
                        {L"h264", 0},  {L"hevc", 1},            {L"av1", 2},
                        {L"vp9", 3},   {L"h264_fragmented", 4}, {L"hevc_fragmented", 5},
                        {L"mpeg4", 6}, {L"h263", 7},            {L"wmv9", 8},
                        {L"vc1", 9},   {L"mjpeg", 10},          {L"h264_avi", 11},
                    };
                    const std::wstring expected =
                        L"codec requires h264, hevc, av1, vp9, h264_fragmented, hevc_fragmented, "
                        L"mpeg4, h263, wmv9, vc1, mjpeg or h264_avi";
                    if (current_.type != TokenType::Identifier) return FailHere(expected);
                    int codecValue = -1;
                    for (const auto& [name, value] : kCodecNames) {
                        if (current_.text == name) { codecValue = value; break; }
                    }
                    if (codecValue < 0) return FailHere(expected);
                    instruction.value = codecValue;
                    Advance();
                } else if (target == L"image_format") {
                    if (current_.type != TokenType::Identifier) return FailHere(L"image_format requires png or jpeg");
                    if (current_.text == L"png") instruction.value = 0;
                    else if (current_.text == L"jpeg" || current_.text == L"jpg") instruction.value = 1;
                    else return FailHere(L"image_format requires png or jpeg");
                    Advance();
                } else if (!TakeNumber(instruction.value)) return FailHere(L"This setting requires an integer value");
            } else if (action == L"audio") {
                if (target == L"add_microphone") instruction.type = LunaInstructionType::AudioAddMicrophone;
                else if (target == L"remove_microphone") instruction.type = LunaInstructionType::AudioRemoveMicrophone;
                else if (target == L"microphone_enabled") instruction.type = LunaInstructionType::AudioMicrophoneEnable;
                else if (target == L"microphone_gain") instruction.type = LunaInstructionType::AudioMicrophoneGain;
                else if (target == L"noise_suppression") instruction.type = LunaInstructionType::AudioNoiseSuppression;
                else if (target == L"noise_gate") instruction.type = LunaInstructionType::AudioNoiseGate;
                else return FailHere(L"Unknown audio target: " + target);
                if (!TakeString(instruction.first)) return FailHere(L"Audio microphone actions require a quoted device name or endpoint ID");
                if (target == L"microphone_enabled") {
                    if (!TakeToggle(instruction.value)) return FailHere(L"microphone_enabled requires on or off");
                } else if (target != L"add_microphone" && target != L"remove_microphone" && !TakeNumber(instruction.value)) {
                    return FailHere(L"This audio action requires an integer value");
                }
            } else if (action == L"capture" && target == L"active") instruction.type = LunaInstructionType::CaptureActive;
            else if (action == L"capture" && target == L"all") instruction.type = LunaInstructionType::CaptureAll;
            else if (action == L"capture" && target == L"region") instruction.type = LunaInstructionType::CaptureRegion;
            else if (action == L"capture" && target == L"window") instruction.type = LunaInstructionType::CaptureWindow;
            else if (action == L"capture" && target == L"monitor") {
                instruction.type = LunaInstructionType::CaptureMonitor;
                if (!TakeNumber(instruction.value) || instruction.value < 0 || instruction.value >= 8) {
                    return FailHere(L"capture monitor requires a monitor index from 0 to 7");
                }
            }
            else if (action == L"recording" && target == L"toggle") instruction.type = LunaInstructionType::RecordingToggle;
            else if (action == L"recording" && target == L"start") instruction.type = LunaInstructionType::RecordingStart;
            else if (action == L"recording" && target == L"stop") instruction.type = LunaInstructionType::RecordingStop;
            else if (action == L"replay" && target == L"toggle") instruction.type = LunaInstructionType::ReplayToggle;
            else if (action == L"replay" && target == L"start") instruction.type = LunaInstructionType::ReplayStart;
            else if (action == L"replay" && target == L"stop") instruction.type = LunaInstructionType::ReplayStop;
            else if (action == L"replay" && target == L"save_all") instruction.type = LunaInstructionType::ReplaySaveAll;
            else if (action == L"replay" && target == L"save_monitor") {
                instruction.type = LunaInstructionType::ReplaySaveMonitor;
                if (!TakeNumber(instruction.value) || instruction.value < 0 || instruction.value >= 8) {
                    return FailHere(L"replay save_monitor requires a monitor index from 0 to 7");
                }
            }
            else if (action == L"open" && target == L"screenshots") instruction.type = LunaInstructionType::OpenScreenshots;
            else if (action == L"open" && target == L"recordings") instruction.type = LunaInstructionType::OpenRecordings;
            else if (action == L"open" && target == L"plugins") instruction.type = LunaInstructionType::OpenPlugins;
            else if (action == L"open" && target == L"replay_cache") instruction.type = LunaInstructionType::OpenReplayCache;
            else if (action == L"open" && target == L"logs") instruction.type = LunaInstructionType::OpenLogs;
            else if (action == L"ui" && target == L"show") instruction.type = LunaInstructionType::UiShow;
            else if (action == L"ui" && target == L"hide") instruction.type = LunaInstructionType::UiHide;
            else if (action == L"ui" && target == L"toggle") instruction.type = LunaInstructionType::UiToggle;
            else if (action == L"ui" && target == L"page") {
                instruction.type = LunaInstructionType::UiNavigate;
                if (current_.type != TokenType::Identifier) return FailHere(L"ui page requires home, capture, photos, videos, record, replay, encoder, audio, plugins, or settings");
                static const std::unordered_set<std::wstring> pages{L"home", L"capture", L"photos", L"videos", L"record", L"replay", L"encoder", L"audio", L"plugins", L"settings"};
                if (!pages.contains(current_.text)) return FailHere(L"Unknown ui page: " + current_.text);
                instruction.first = current_.text;
                Advance();
            }
            else if (action == L"quick_panel" && target == L"show") instruction.type = LunaInstructionType::QuickPanelShow;
            else if (action == L"quick_panel" && target == L"hide") instruction.type = LunaInstructionType::QuickPanelHide;
            else if (action == L"quick_panel" && target == L"toggle") instruction.type = LunaInstructionType::QuickPanelToggle;
            else if (action == L"theme" && target == L"activate") instruction.type = LunaInstructionType::ThemeActivate;
            else if (action == L"theme" && target == L"reset") instruction.type = LunaInstructionType::ThemeReset;
            else if (action == L"locale" && target == L"activate") instruction.type = LunaInstructionType::LocaleActivate;
            else if (action == L"locale" && target == L"reset") instruction.type = LunaInstructionType::LocaleReset;
            else return FailHere(L"Unknown action: " + action + L" " + target);
        }

        const auto inRange = [&instruction, this](int minimum, int maximum, const wchar_t* label) {
            if (instruction.value >= minimum && instruction.value <= maximum) return true;
            return FailHere(std::wstring(label) + L" must be from " + std::to_wstring(minimum) + L" to " + std::to_wstring(maximum));
        };
        switch (instruction.type) {
            case LunaInstructionType::SettingPreviewFps: if (!inRange(1, 60, L"preview_fps")) return false; break;
            case LunaInstructionType::SettingReplaySeconds: if (!inRange(5, 3600, L"replay_seconds")) return false; break;
            case LunaInstructionType::SettingRecordingFps: if (!inRange(1, 240, L"recording_fps")) return false; break;
            case LunaInstructionType::SettingVideoBitrate: if (!inRange(1, 200, L"video_bitrate")) return false; break;
            case LunaInstructionType::SettingRecordingHeight: if (!inRange(0, 4320, L"recording_height")) return false; break;
            case LunaInstructionType::SettingJpegQuality: if (!inRange(1, 100, L"jpeg_quality")) return false; break;
            case LunaInstructionType::SettingOutputWidth: if (!inRange(0, 7680, L"output_width")) return false; break;
            case LunaInstructionType::SettingOutputHeight: if (!inRange(0, 4320, L"output_height")) return false; break;
            case LunaInstructionType::SettingOverlayDuration: if (!inRange(250, 10000, L"overlay_duration")) return false; break;
            case LunaInstructionType::SettingSystemAudioVolume:
            case LunaInstructionType::SettingApplicationAudioVolume:
                if (!inRange(0, 200, L"audio volume")) return false; break;
            case LunaInstructionType::SettingAudioBitrate:
                if (instruction.value != 96 && instruction.value != 128 && instruction.value != 160 && instruction.value != 192) {
                    return FailHere(L"audio_bitrate must be 96, 128, 160, or 192");
                }
                break;
            case LunaInstructionType::AudioMicrophoneGain: if (!inRange(0, 200, L"microphone_gain")) return false; break;
            case LunaInstructionType::AudioNoiseSuppression: if (!inRange(0, 100, L"noise_suppression")) return false; break;
            case LunaInstructionType::AudioNoiseGate: if (!inRange(20, 80, L"noise_gate")) return false; break;
            default: break;
        }
        switch (instruction.type) {
            case LunaInstructionType::CaptureMonitor:
            case LunaInstructionType::SettingImageFormat:
            case LunaInstructionType::SettingJpegQuality:
            case LunaInstructionType::SettingOutputWidth:
            case LunaInstructionType::SettingOutputHeight:
            case LunaInstructionType::SettingCopyRegion:
            case LunaInstructionType::SettingOverlayDuration:
            case LunaInstructionType::SettingReplayPreview:
            case LunaInstructionType::SettingSystemAudio:
            case LunaInstructionType::SettingSystemAudioVolume:
            case LunaInstructionType::SettingApplicationAudio:
            case LunaInstructionType::SettingApplicationAudioVolume:
            case LunaInstructionType::SettingAudioBitrate:
            case LunaInstructionType::OpenReplayCache:
            case LunaInstructionType::OpenLogs:
            case LunaInstructionType::UiShow:
            case LunaInstructionType::UiHide:
            case LunaInstructionType::UiToggle:
            case LunaInstructionType::UiNavigate:
            case LunaInstructionType::QuickPanelShow:
            case LunaInstructionType::QuickPanelHide:
            case LunaInstructionType::QuickPanelToggle:
            case LunaInstructionType::ThemeActivate:
            case LunaInstructionType::ThemeReset:
            case LunaInstructionType::LocaleActivate:
            case LunaInstructionType::LocaleReset:
                MarkHostApi(5, instruction.line);
                break;
            default: break;
        }
        OptionalSemicolon();
        return true;
    }

    void RecoverMember() {
        int depth = 0;
        while (current_.type != TokenType::End) {
            if (current_.type == TokenType::LeftBrace) ++depth;
            else if (current_.type == TokenType::RightBrace) {
                if (depth == 0) return;
                --depth;
            } else if (current_.type == TokenType::Semicolon && depth == 0) { Advance(); return; }
            Advance();
        }
    }

    void Validate(LunaPluginDefinition& plugin) {
        if (plugin.id.empty()) AddError(L"Plugin id cannot be empty");
        if (plugin.id.size() > 96) AddError(L"Plugin id cannot exceed 96 characters");
        for (wchar_t value : plugin.id) {
            if (!(std::iswalnum(value) || value == L'.' || value == L'_' || value == L'-')) {
                AddError(L"Plugin id may contain only letters, numbers, '.', '_' and '-'");
                break;
            }
        }
        if (plugin.name.empty()) AddError(L"Plugin requires a name property");
        if (plugin.version.empty()) AddError(L"Plugin requires a version property");
        if (plugin.author.empty()) plugin.author = L"Unknown author";
        if (plugin.description.empty()) plugin.description = L"No description provided";
        if (plugin.requiredHostApi > LunaScript::kSupportedHostApi) {
            AddError(L"Plugin requires Host API " + std::to_wstring(plugin.requiredHostApi) +
                     L", but this build supports Host API " + std::to_wstring(LunaScript::kSupportedHostApi));
        }
        if (plugin.requiredHostApi < minimumHostApiUsed_) {
            diagnostics_.push_back({minimumHostApiLine_, 1,
                L"This plugin uses Host API " + std::to_wstring(minimumHostApiUsed_) +
                L" syntax; add `requires host_api " + std::to_wstring(minimumHostApiUsed_) + L";` near the top of the plugin block"});
        }
        if (plugin.theme.present && !LunaScript::HasPermission(plugin, LunaPermission::Theme)) {
            AddError(L"A theme block requires: permission theme;");
        }
        if (plugin.locale.present && !LunaScript::HasPermission(plugin, LunaPermission::Localization)) {
            AddError(L"A locale block requires: permission localization;");
        }
        if (plugin.locale.present && plugin.locale.translations.empty()) AddError(L"A locale block must contain at least one translation");
        std::unordered_set<std::wstring> localeSources;
        for (const auto& translation : plugin.locale.translations) {
            if (translation.source.empty() || translation.replacement.empty()) {
                AddError(L"Locale source and replacement strings cannot be empty");
            }
            if (!localeSources.insert(translation.source).second) {
                AddError(L"Duplicate locale source: " + translation.source);
            }
            const std::uint16_t sourcePlaceholders = LocalizationPlaceholderMask(translation.source);
            const std::uint16_t replacementPlaceholders = LocalizationPlaceholderMask(translation.replacement);
            if (HasAdjacentLocalizationPlaceholders(translation.source)) {
                AddError(L"Locale source placeholders must be separated by literal text");
            }
            if ((replacementPlaceholders & ~sourcePlaceholders) != 0) {
                AddError(L"A locale replacement may only use placeholders declared by its source string");
            }
        }
        std::unordered_set<std::wstring> commandIds;
        for (const auto& command : plugin.commands) {
            if (command.id.empty() || command.title.empty()) AddError(L"Every command needs a non-empty id and title");
            if (command.id.size() > 96) AddError(L"Command ids cannot exceed 96 characters");
            for (wchar_t value : command.id) {
                if (!(std::iswalnum(value) || value == L'.' || value == L'_' || value == L'-')) {
                    AddError(L"Command ids may contain only letters, numbers, '.', '_' and '-'");
                    break;
                }
            }
            if (command.title.size() > 128) AddError(L"Command titles cannot exceed 128 characters");
            if (command.description.size() > 256) AddError(L"Command descriptions cannot exceed 256 characters");
            if (command.icon.size() > 32) AddError(L"Command icon names cannot exceed 32 characters");
            if (!commandIds.insert(command.id).second) AddError(L"Duplicate command id: " + command.id);
            ValidateInstructions(plugin, command.instructions);
        }
        static const std::unordered_set<std::wstring> events{
            L"app_started", L"capture_saved", L"recording_started", L"recording_saved",
            L"capture_failed", L"replay_started", L"replay_saved", L"replay_stopped", L"plugin_enabled",
            L"quick_panel_opened", L"quick_panel_closed", L"quick_panel_command", L"timer",
            L"capture_queued", L"capture_cancelled", L"capture_copied", L"recording_stopped",
            L"recording_failed", L"recording_recovering", L"recording_recovered", L"replay_failed",
            L"replay_recovering", L"replay_recovered", L"plugins_reloaded"};
        for (const auto& event : plugin.events) {
            if (!events.contains(event.eventName)) AddError(L"Unknown event: " + event.eventName);
            if (event.eventName == L"timer" && (event.intervalSeconds < 5 || event.intervalSeconds > 86400)) {
                AddError(L"on timer requires: every <seconds> (5..86400)");
            }
            ValidateInstructions(plugin, event.instructions);
        }
    }

    void ValidateInstructions(const LunaPluginDefinition& plugin, const std::vector<LunaInstruction>& instructions) {
        for (const auto& instruction : instructions) {
            LunaPermission required = LunaPermission::Diagnostics;
            switch (instruction.type) {
                case LunaInstructionType::Notify: required = LunaPermission::Notifications; break;
                case LunaInstructionType::CaptureActive:
                case LunaInstructionType::CaptureAll:
                case LunaInstructionType::CaptureRegion:
                case LunaInstructionType::CaptureWindow:
                case LunaInstructionType::CaptureMonitor: required = LunaPermission::Capture; break;
                case LunaInstructionType::RecordingToggle:
                case LunaInstructionType::RecordingStart:
                case LunaInstructionType::RecordingStop: required = LunaPermission::Recording; break;
                case LunaInstructionType::ReplayToggle:
                case LunaInstructionType::ReplayStart:
                case LunaInstructionType::ReplayStop:
                case LunaInstructionType::ReplaySaveAll:
                case LunaInstructionType::ReplaySaveMonitor: required = LunaPermission::Replay; break;
                case LunaInstructionType::SettingPreview:
                case LunaInstructionType::SettingPreviewFps:
                case LunaInstructionType::SettingCursor:
                case LunaInstructionType::SettingNotifications:
                case LunaInstructionType::SettingSound:
                case LunaInstructionType::SettingReplaySeconds:
                case LunaInstructionType::SettingRecordingFps:
                case LunaInstructionType::SettingVideoBitrate:
                case LunaInstructionType::SettingRecordingHeight:
                case LunaInstructionType::SettingVideoCodec:
                case LunaInstructionType::SettingImageFormat:
                case LunaInstructionType::SettingJpegQuality:
                case LunaInstructionType::SettingOutputWidth:
                case LunaInstructionType::SettingOutputHeight:
                case LunaInstructionType::SettingCopyRegion:
                case LunaInstructionType::SettingOverlayDuration:
                case LunaInstructionType::SettingReplayPreview:
                case LunaInstructionType::SettingSystemAudio:
                case LunaInstructionType::SettingSystemAudioVolume:
                case LunaInstructionType::SettingApplicationAudio:
                case LunaInstructionType::SettingApplicationAudioVolume:
                case LunaInstructionType::SettingAudioBitrate: required = LunaPermission::Settings; break;
                case LunaInstructionType::AudioAddMicrophone:
                case LunaInstructionType::AudioRemoveMicrophone:
                case LunaInstructionType::AudioMicrophoneEnable:
                case LunaInstructionType::AudioMicrophoneGain:
                case LunaInstructionType::AudioNoiseSuppression:
                case LunaInstructionType::AudioNoiseGate: required = LunaPermission::Audio; break;
                case LunaInstructionType::OpenScreenshots:
                case LunaInstructionType::OpenRecordings:
                case LunaInstructionType::OpenPlugins:
                case LunaInstructionType::OpenReplayCache:
                case LunaInstructionType::OpenLogs: required = LunaPermission::OpenFolders; break;
                case LunaInstructionType::UiShow:
                case LunaInstructionType::UiHide:
                case LunaInstructionType::UiToggle:
                case LunaInstructionType::UiNavigate:
                case LunaInstructionType::QuickPanelShow:
                case LunaInstructionType::QuickPanelHide:
                case LunaInstructionType::QuickPanelToggle: required = LunaPermission::Ui; break;
                case LunaInstructionType::ThemeActivate:
                case LunaInstructionType::ThemeReset: required = LunaPermission::Theme; break;
                case LunaInstructionType::LocaleActivate:
                case LunaInstructionType::LocaleReset: required = LunaPermission::Localization; break;
                case LunaInstructionType::Log: required = LunaPermission::Diagnostics; break;
            }
            if (instruction.type == LunaInstructionType::Notify &&
                (instruction.first.size() > 128 || instruction.second.size() > 512)) {
                diagnostics_.push_back({instruction.line, 1, L"notify title/detail are limited to 128/512 characters"});
            }
            if (instruction.type == LunaInstructionType::Log && instruction.first.size() > 1024) {
                diagnostics_.push_back({instruction.line, 1, L"log messages are limited to 1024 characters"});
            }
            if (instruction.type == LunaInstructionType::Log &&
                std::any_of(instruction.first.begin(), instruction.first.end(), [](wchar_t value) {
                    return value == L'\r' || value == L'\n' || (value < L' ' && value != L'\t');
                })) {
                diagnostics_.push_back({instruction.line, 1, L"log messages must stay on one line and cannot contain control characters"});
            }
            if ((instruction.type == LunaInstructionType::AudioAddMicrophone ||
                 instruction.type == LunaInstructionType::AudioRemoveMicrophone ||
                 instruction.type == LunaInstructionType::AudioMicrophoneEnable ||
                 instruction.type == LunaInstructionType::AudioMicrophoneGain ||
                 instruction.type == LunaInstructionType::AudioNoiseSuppression ||
                 instruction.type == LunaInstructionType::AudioNoiseGate) && instruction.first.size() > 256) {
                diagnostics_.push_back({instruction.line, 1, L"Audio target names are limited to 256 characters"});
            }
            if (!LunaScript::HasPermission(plugin, required)) {
                diagnostics_.push_back({instruction.line, 1,
                    L"Instruction requires permission " + std::wstring(LunaScript::PermissionName(required))});
            }
        }
    }

    Lexer lexer_;
    std::vector<LunaDiagnostic>& diagnostics_;
    Token current_;
    int minimumHostApiUsed_ = 1;
    int minimumHostApiLine_ = 1;
};
}  // namespace

bool LunaScript::Parse(const std::wstring& source, LunaPluginDefinition& plugin,
                       std::vector<LunaDiagnostic>& diagnostics) {
    diagnostics.clear();
    if (source.size() > 1024 * 1024) {
        diagnostics.push_back({1, 1, L"A LunaScript source file cannot exceed 1 MiB"});
        return false;
    }
    Parser parser(source, diagnostics);
    return parser.Parse(plugin);
}

const wchar_t* LunaScript::PermissionName(LunaPermission permission) {
    switch (permission) {
        case LunaPermission::Theme: return L"theme";
        case LunaPermission::Notifications: return L"notifications";
        case LunaPermission::Capture: return L"capture";
        case LunaPermission::Recording: return L"recording";
        case LunaPermission::Replay: return L"replay";
        case LunaPermission::Audio: return L"audio";
        case LunaPermission::Settings: return L"settings";
        case LunaPermission::Localization: return L"localization";
        case LunaPermission::OpenFolders: return L"open_folders";
        case LunaPermission::Diagnostics: return L"diagnostics";
        case LunaPermission::Ui: return L"ui";
        default: return L"unknown";
    }
}

bool LunaScript::HasPermission(const LunaPluginDefinition& plugin, LunaPermission permission) {
    return std::find(plugin.permissions.begin(), plugin.permissions.end(), permission) != plugin.permissions.end();
}

const wchar_t* LunaScript::CommandSurfaceName(LunaCommandSurface surface) {
    switch (surface) {
        case LunaCommandSurface::QuickPanel: return L"quick_panel";
        case LunaCommandSurface::TrayMenu: return L"tray";
        default: return L"unknown";
    }
}

bool LunaScript::HasCommandSurface(const LunaCommandDefinition& command, LunaCommandSurface surface) {
    return std::find(command.surfaces.begin(), command.surfaces.end(), surface) != command.surfaces.end();
}
