#pragma once

#include <optional>
#include <string>

class ExpressionParser
{
public:
    explicit ExpressionParser(std::wstring text) : text_(std::move(text)) {}

    std::optional<double> parse();

private:
    void skipSpaces();
    bool consume(wchar_t ch);
    std::optional<double> parseExpression();
    std::optional<double> parseTerm();
    std::optional<double> parsePower();
    std::optional<double> parseUnary();
    std::optional<double> parsePrimary();

    std::wstring text_;
    size_t pos_ = 0;
};

bool looksLikeMath(const std::wstring& query);
std::wstring formatDouble(double value);
