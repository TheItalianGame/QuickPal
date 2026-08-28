#include "expression.h"

#include "util.h"

#include <cmath>
#include <cwctype>
#include <sstream>

std::optional<double> ExpressionParser::parse()
{
    pos_ = 0;
    auto value = parseExpression();
    skipSpaces();
    if (!value || pos_ != text_.size())
    {
        return std::nullopt;
    }
    return value;
}

void ExpressionParser::skipSpaces()
{
    while (pos_ < text_.size() && std::iswspace(text_[pos_]))
    {
        ++pos_;
    }
}

bool ExpressionParser::consume(wchar_t ch)
{
    skipSpaces();
    if (pos_ < text_.size() && text_[pos_] == ch)
    {
        ++pos_;
        return true;
    }
    return false;
}

std::optional<double> ExpressionParser::parseExpression()
{
    auto value = parseTerm();
    while (value)
    {
        if (consume(L'+'))
        {
            auto rhs = parseTerm();
            if (!rhs)
            {
                return std::nullopt;
            }
            *value += *rhs;
        }
        else if (consume(L'-'))
        {
            auto rhs = parseTerm();
            if (!rhs)
            {
                return std::nullopt;
            }
            *value -= *rhs;
        }
        else
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<double> ExpressionParser::parseTerm()
{
    auto value = parsePower();
    while (value)
    {
        if (consume(L'*'))
        {
            auto rhs = parsePower();
            if (!rhs)
            {
                return std::nullopt;
            }
            *value *= *rhs;
        }
        else if (consume(L'/'))
        {
            auto rhs = parsePower();
            if (!rhs || *rhs == 0.0)
            {
                return std::nullopt;
            }
            *value /= *rhs;
        }
        else if (consume(L'%'))
        {
            auto rhs = parsePower();
            if (!rhs || *rhs == 0.0)
            {
                return std::nullopt;
            }
            *value = std::fmod(*value, *rhs);
        }
        else
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<double> ExpressionParser::parsePower()
{
    auto value = parseUnary();
    if (!value)
    {
        return std::nullopt;
    }
    if (consume(L'^'))
    {
        auto rhs = parsePower();
        if (!rhs)
        {
            return std::nullopt;
        }
        *value = std::pow(*value, *rhs);
    }
    return value;
}

std::optional<double> ExpressionParser::parseUnary()
{
    if (consume(L'+'))
    {
        return parseUnary();
    }
    if (consume(L'-'))
    {
        auto value = parseUnary();
        if (!value)
        {
            return std::nullopt;
        }
        return -*value;
    }
    return parsePrimary();
}

std::optional<double> ExpressionParser::parsePrimary()
{
    skipSpaces();
    if (consume(L'('))
    {
        auto value = parseExpression();
        if (!value || !consume(L')'))
        {
            return std::nullopt;
        }
        return value;
    }

    if (startsWith(lowerCopy(text_.substr(pos_, 4)), L"sqrt"))
    {
        pos_ += 4;
        if (!consume(L'('))
        {
            return std::nullopt;
        }
        auto value = parseExpression();
        if (!value || !consume(L')') || *value < 0)
        {
            return std::nullopt;
        }
        return std::sqrt(*value);
    }

    const size_t start = pos_;
    bool sawDigit = false;
    while (pos_ < text_.size() && (std::iswdigit(text_[pos_]) || text_[pos_] == L'.'))
    {
        sawDigit = sawDigit || std::iswdigit(text_[pos_]) != 0;
        ++pos_;
    }
    if (!sawDigit)
    {
        return std::nullopt;
    }

    try
    {
        return std::stod(text_.substr(start, pos_ - start));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool looksLikeMath(const std::wstring& query)
{
    bool hasDigit = false;
    bool hasOperator = false;
    for (wchar_t ch : query)
    {
        hasDigit = hasDigit || std::iswdigit(ch) != 0;
        hasOperator = hasOperator || ch == L'+' || ch == L'-' || ch == L'*' || ch == L'/' || ch == L'%' || ch == L'^' || ch == L'(';
    }
    return hasDigit && hasOperator;
}

std::wstring formatDouble(double value)
{
    std::wstringstream stream;
    stream.setf(std::ios::fixed, std::ios::floatfield);
    stream.precision(10);
    stream << value;
    std::wstring out = stream.str();
    while (out.size() > 1 && out.back() == L'0')
    {
        out.pop_back();
    }
    if (!out.empty() && out.back() == L'.')
    {
        out.pop_back();
    }
    if (out == L"-0")
    {
        out = L"0";
    }
    return out;
}
