#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <string_view>
#include <unordered_set>
#include "StringUtils.h"

/// Token types produced by the query tokenizer.
enum class TokenType {
    WORD,       // plain text token
    QUOTED,     // text inside double quotes
    PIPE,       // | (OR operator)
    BANG,       // ! (NOT operator)
    LANGLE,     // < (group open)
    RANGLE,     // > (group close)
    FILTER,     // known_filter:value (e.g., ext:cpp)
    END,        // end of input
};

/// A single token from the query string.
struct Token {
    TokenType type;
    std::string value;       // text content (for WORD, QUOTED, FILTER)
    std::string filterName;  // filter name portion (for FILTER type only)
    std::string filterArg;   // filter argument portion (for FILTER type only)
};

/// Lexer for Everything-style query syntax.
/// Produces a flat list of tokens from a query string.
class QueryTokenizer {
public:
    static std::vector<Token> tokenize(const std::string& input) {
        std::vector<Token> tokens;

        // Treat a pasted absolute path as one token, even when directory names
        // contain spaces. Also accept editor/Codex references such as
        // /path/to/file.swift:42:7 by dropping the trailing line/column suffix.
        std::string literalPath;
        if (normalizeStandalonePathQuery(input, literalPath)) {
            tokens.push_back({TokenType::WORD, std::move(literalPath)});
            tokens.push_back({TokenType::END, ""});
            return tokens;
        }

        size_t i = 0;
        size_t len = input.size();

        while (i < len) {
            char c = input[i];

            // Skip whitespace
            if (c == ' ' || c == '\t') {
                i++;
                continue;
            }

            // Single-character operators
            if (c == '|') {
                tokens.push_back({TokenType::PIPE, "|"});
                i++;
                continue;
            }
            if (c == '!') {
                tokens.push_back({TokenType::BANG, "!"});
                i++;
                continue;
            }
            if (c == '<') {
                tokens.push_back({TokenType::LANGLE, "<"});
                i++;
                continue;
            }
            if (c == '>') {
                tokens.push_back({TokenType::RANGLE, ">"});
                i++;
                continue;
            }

            // Quoted string
            if (c == '"') {
                i++; // skip opening quote
                std::string text;
                while (i < len && input[i] != '"') {
                    text += input[i];
                    i++;
                }
                if (i < len) i++; // skip closing quote
                tokens.push_back({TokenType::QUOTED, text});
                continue;
            }

            // Word or filter: collect until whitespace or operator.
            // First pass: collect the initial word segment (stop at operators).
            std::string word;
            size_t wordStart = i;
            while (i < len) {
                char ch = input[i];
                if (ch == ' ' || ch == '\t' || ch == '|' || ch == '!'
                    || ch == '<' || ch == '>' || ch == '"') {
                    break;
                }
                word += ch;
                i++;
            }

            if (!word.empty()) {
                // Check if this is a filter: name:value
                auto colonPos = word.find(':');
                if (colonPos != std::string::npos && colonPos > 0) {
                    std::string name = word.substr(0, colonPos);
                    std::string lowerName = me::toLower(name);

                    if (isKnownFilter(lowerName)) {
                        // For filters, re-collect arg allowing < > chars
                        // (e.g., size:>100kb, depth:<3)
                        size_t argStart = wordStart + colonPos + 1;
                        i = argStart;
                        std::string arg;
                        while (i < len) {
                            char ch = input[i];
                            if (ch == ' ' || ch == '\t' || ch == '|' || ch == '!'
                                || ch == '"') {
                                break;
                            }
                            arg += ch;
                            i++;
                        }
                        Token t;
                        t.type = TokenType::FILTER;
                        t.value = lowerName + ":" + arg;
                        t.filterName = lowerName;
                        t.filterArg = arg;
                        tokens.push_back(std::move(t));
                        continue;
                    }
                }
                tokens.push_back({TokenType::WORD, word});
            }
        }

        tokens.push_back({TokenType::END, ""});
        return tokens;
    }

    /// Quick check: does the query string contain any advanced syntax?
    /// Used to decide whether to invoke the parser or use the fast path.
    static bool hasAdvancedSyntax(const std::string& q) {
        for (size_t i = 0; i < q.size(); i++) {
            char c = q[i];
            if (c == '|' || c == '!' || c == '<' || c == '>' || c == '"') {
                return true;
            }
            // Check for known filter prefix: word followed by colon
            if (c == ':' && i > 0) {
                // Extract the word before the colon
                size_t start = i;
                while (start > 0 && !std::isspace(static_cast<unsigned char>(q[start - 1]))
                       && q[start - 1] != '|' && q[start - 1] != '!'
                       && q[start - 1] != '<' && q[start - 1] != '>') {
                    start--;
                }
                std::string name;
                for (size_t j = start; j < i; j++) {
                    name += static_cast<char>(std::tolower(static_cast<unsigned char>(q[j])));
                }
                if (isKnownFilter(name)) return true;
            }
        }
        return false;
    }

private:
    static bool normalizeStandalonePathQuery(const std::string& input,
                                             std::string& normalized) {
        size_t start = input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return false;
        size_t end = input.find_last_not_of(" \t\r\n");
        std::string path = input.substr(start, end - start + 1);

        if (path.rfind("file://", 0) == 0) {
            path.erase(0, 7);
        }

        if (path.empty() || (path[0] != '/' && path.rfind("~/", 0) != 0)) {
            return false;
        }

        // Preserve existing glob and directory-list query semantics.
        if (path.find('*') != std::string::npos ||
            path.find('?') != std::string::npos) {
            return false;
        }

        // Keep explicit advanced expressions such as "/tmp/file ext:md"
        // on the normal tokenizer path instead of swallowing the filter.
        if (hasExplicitAdvancedSuffix(path)) return false;

        stripEditorLocationSuffix(path);
        if (path.empty()) return false;

        normalized = std::move(path);
        return true;
    }

    static bool hasExplicitAdvancedSuffix(const std::string& path) {
        size_t i = 0;
        while (i < path.size()) {
            if (!std::isspace(static_cast<unsigned char>(path[i]))) {
                i++;
                continue;
            }

            while (i < path.size() &&
                   std::isspace(static_cast<unsigned char>(path[i]))) {
                i++;
            }
            if (i >= path.size()) break;

            char c = path[i];
            if (c == '|' || c == '!' || c == '<' || c == '>' || c == '"') {
                return true;
            }

            size_t tokenEnd = i;
            while (tokenEnd < path.size() &&
                   !std::isspace(static_cast<unsigned char>(path[tokenEnd]))) {
                tokenEnd++;
            }
            std::string_view token(path.data() + i, tokenEnd - i);
            size_t colonPos = token.find(':');
            size_t slashPos = token.find('/');
            if (colonPos != std::string_view::npos && colonPos > 0 &&
                slashPos == std::string_view::npos) {
                std::string filterName(token.substr(0, colonPos));
                if (isKnownFilter(me::toLower(filterName))) return true;
            }
            i = tokenEnd;
        }
        return false;
    }

    static void stripEditorLocationSuffix(std::string& path) {
        size_t lastSlash = path.find_last_of('/');
        for (int component = 0; component < 2; component++) {
            size_t colon = path.find_last_of(':');
            if (colon == std::string::npos ||
                (lastSlash != std::string::npos && colon < lastSlash) ||
                colon + 1 >= path.size()) {
                return;
            }

            bool digitsOnly = true;
            for (size_t i = colon + 1; i < path.size(); i++) {
                if (!std::isdigit(static_cast<unsigned char>(path[i]))) {
                    digitsOnly = false;
                    break;
                }
            }
            if (!digitsOnly) return;
            path.erase(colon);
        }
    }

    static bool isKnownFilter(const std::string& name) {
        static const std::unordered_set<std::string> filters = {
            // Phase 2 filters
            "ext", "size", "file", "folder",
            "path", "nopath", "parent", "depth", "len",
            // Phase 3 filters
            "dm", "dc", "da",
            "datemodified", "datecreated", "dateaccessed",
            // Phase 4 modifiers (treated as filters by tokenizer)
            "case", "nocase", "regex", "ww", "wfn",
            "wholeword", "wholefilename",
            // Phase 4 macros
            "audio", "video", "pic", "doc", "exe", "zip",
            // Content
            "content",
            // Type shorthand
            "type",
        };
        return filters.count(name) > 0;
    }
};
