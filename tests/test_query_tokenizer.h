#pragma once
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

// ── Part 54: Query Tokenizer Tests ──

static void runQueryTokenizerTests() {
    std::cout << "── Part 54: Query Tokenizer ──\n";
    int localPassed = 0, localFailed = 0;

    auto check = [&](bool cond, const std::string& msg) {
        if (cond) { localPassed++; passed++; }
        else { localFailed++; failed++; std::cout << "  FAIL: " << msg << "\n"; }
    };

    // 54.1 Simple word
    {
        auto tokens = QueryTokenizer::tokenize("hello");
        check(tokens.size() == 2, "54.1 simple word token count");
        check(tokens[0].type == TokenType::WORD, "54.1 type is WORD");
        check(tokens[0].value == "hello", "54.1 value is hello");
        check(tokens[1].type == TokenType::END, "54.1 ends with END");
    }

    // 54.2 Two words (implicit AND)
    {
        auto tokens = QueryTokenizer::tokenize("foo bar");
        check(tokens.size() == 3, "54.2 two words token count");
        check(tokens[0].type == TokenType::WORD && tokens[0].value == "foo", "54.2 first word");
        check(tokens[1].type == TokenType::WORD && tokens[1].value == "bar", "54.2 second word");
    }

    // 54.3 OR operator
    {
        auto tokens = QueryTokenizer::tokenize("foo | bar");
        check(tokens.size() == 4, "54.3 OR token count");
        check(tokens[0].type == TokenType::WORD, "54.3 first is WORD");
        check(tokens[1].type == TokenType::PIPE, "54.3 pipe token");
        check(tokens[2].type == TokenType::WORD, "54.3 second is WORD");
    }

    // 54.4 NOT operator
    {
        auto tokens = QueryTokenizer::tokenize("!foo");
        check(tokens.size() == 3, "54.4 NOT token count");
        check(tokens[0].type == TokenType::BANG, "54.4 bang token");
        check(tokens[1].type == TokenType::WORD, "54.4 word after bang");
    }

    // 54.5 Grouping with < >
    {
        auto tokens = QueryTokenizer::tokenize("<foo | bar>");
        check(tokens.size() == 6, "54.5 grouping token count");
        check(tokens[0].type == TokenType::LANGLE, "54.5 open angle");
        check(tokens[1].type == TokenType::WORD, "54.5 first word");
        check(tokens[2].type == TokenType::PIPE, "54.5 pipe");
        check(tokens[3].type == TokenType::WORD, "54.5 second word");
        check(tokens[4].type == TokenType::RANGLE, "54.5 close angle");
    }

    // 54.6 Quoted string
    {
        auto tokens = QueryTokenizer::tokenize("\"hello world\"");
        check(tokens.size() == 2, "54.6 quoted token count");
        check(tokens[0].type == TokenType::QUOTED, "54.6 type is QUOTED");
        check(tokens[0].value == "hello world", "54.6 quoted value");
    }

    // 54.7 Filter token
    {
        auto tokens = QueryTokenizer::tokenize("ext:cpp");
        check(tokens.size() == 2, "54.7 filter token count");
        check(tokens[0].type == TokenType::FILTER, "54.7 type is FILTER");
        check(tokens[0].filterName == "ext", "54.7 filter name");
        check(tokens[0].filterArg == "cpp", "54.7 filter arg");
    }

    // 54.8 Unknown prefix:value is WORD, not FILTER
    {
        auto tokens = QueryTokenizer::tokenize("unknown:value");
        check(tokens.size() == 2, "54.8 unknown filter token count");
        check(tokens[0].type == TokenType::WORD, "54.8 unknown prefix is WORD");
        check(tokens[0].value == "unknown:value", "54.8 full value preserved");
    }

    // 54.9 Complex query
    {
        auto tokens = QueryTokenizer::tokenize("foo !bar | <baz \"hello\"> ext:h");
        // foo ! bar | < baz "hello" > ext:h END
        check(tokens.size() == 10, "54.9 complex token count = " + std::to_string(tokens.size()));
        check(tokens[0].type == TokenType::WORD, "54.9 foo");
        check(tokens[1].type == TokenType::BANG, "54.9 bang");
        check(tokens[2].type == TokenType::WORD, "54.9 bar");
        check(tokens[3].type == TokenType::PIPE, "54.9 pipe");
        check(tokens[4].type == TokenType::LANGLE, "54.9 langle");
        check(tokens[5].type == TokenType::WORD, "54.9 baz");
        check(tokens[6].type == TokenType::QUOTED, "54.9 quoted");
        check(tokens[7].type == TokenType::RANGLE, "54.9 rangle");
        check(tokens[8].type == TokenType::FILTER, "54.9 filter");
    }

    // 54.10 hasAdvancedSyntax
    {
        check(!QueryTokenizer::hasAdvancedSyntax("hello"), "54.10 simple no advanced");
        check(!QueryTokenizer::hasAdvancedSyntax("foo bar"), "54.10 two words no advanced");
        check(QueryTokenizer::hasAdvancedSyntax("foo | bar"), "54.10 pipe is advanced");
        check(QueryTokenizer::hasAdvancedSyntax("!foo"), "54.10 bang is advanced");
        check(QueryTokenizer::hasAdvancedSyntax("<foo>"), "54.10 angle is advanced");
        check(QueryTokenizer::hasAdvancedSyntax("\"hello\""), "54.10 quote is advanced");
        check(QueryTokenizer::hasAdvancedSyntax("ext:cpp"), "54.10 known filter is advanced");
        check(!QueryTokenizer::hasAdvancedSyntax("unknown:value"), "54.10 unknown filter not advanced");
    }

    // 54.11 Empty input
    {
        auto tokens = QueryTokenizer::tokenize("");
        check(tokens.size() == 1 && tokens[0].type == TokenType::END, "54.11 empty input");
    }

    // 54.12 Whitespace only
    {
        auto tokens = QueryTokenizer::tokenize("   \t  ");
        check(tokens.size() == 1 && tokens[0].type == TokenType::END, "54.12 whitespace only");
    }

    // 54.13 Multiple filters
    {
        auto tokens = QueryTokenizer::tokenize("ext:cpp size:>100kb");
        check(tokens.size() == 3, "54.13 two filters");
        check(tokens[0].type == TokenType::FILTER && tokens[0].filterName == "ext", "54.13 first filter");
        check(tokens[1].type == TokenType::FILTER && tokens[1].filterName == "size", "54.13 second filter");
        check(tokens[1].filterArg == ">100kb", "54.13 size arg preserved");
    }

    // 54.14 Case-insensitive filter names
    {
        auto tokens = QueryTokenizer::tokenize("EXT:cpp");
        check(tokens[0].type == TokenType::FILTER, "54.14 uppercase filter name recognized");
        check(tokens[0].filterName == "ext", "54.14 filter name lowered");
    }

    // 54.15 Unclosed quote
    {
        auto tokens = QueryTokenizer::tokenize("\"unclosed");
        check(tokens.size() == 2, "54.15 unclosed quote count");
        check(tokens[0].type == TokenType::QUOTED, "54.15 still produces QUOTED");
        check(tokens[0].value == "unclosed", "54.15 value captured");
    }

    // 54.16 Pasted absolute path with spaces stays a single token
    {
        const std::string path =
            "/Users/test/Documents/New project/xiaohongshu/design.md";
        auto tokens = QueryTokenizer::tokenize(path);
        check(tokens.size() == 2, "54.16 absolute path token count");
        check(tokens[0].type == TokenType::WORD, "54.16 absolute path is WORD");
        check(tokens[0].value == path, "54.16 spaces preserved inside path");
    }

    // 54.17 Codex/editor line and column suffixes are removed
    {
        auto tokens = QueryTokenizer::tokenize(
            "/Users/test/New project/file.swift:42:7");
        check(tokens.size() == 2, "54.17 location path token count");
        check(tokens[0].value == "/Users/test/New project/file.swift",
              "54.17 line and column suffix removed");

        tokens = QueryTokenizer::tokenize("/Users/test/New project/file.md:1");
        check(tokens[0].value == "/Users/test/New project/file.md",
              "54.17 line-only suffix removed");
    }

    // 54.18 Explicit filters after a path retain normal query semantics
    {
        auto tokens = QueryTokenizer::tokenize("/tmp/file ext:md");
        check(tokens.size() == 3, "54.18 path plus filter token count");
        check(tokens[0].type == TokenType::WORD && tokens[0].value == "/tmp/file",
              "54.18 path token preserved");
        check(tokens[1].type == TokenType::FILTER && tokens[1].filterName == "ext",
              "54.18 filter remains separate");
    }

    std::cout << "  Passed: " << localPassed << "  Failed: " << localFailed << "\n\n";
}
