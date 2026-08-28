#include "editor.h"

#include <algorithm>
#include <cwctype>

namespace
{
bool isWordChar(wchar_t ch)
{
    return std::iswalnum(ch) != 0 || ch == L'_';
}
}

std::pair<size_t, size_t> TextEditor::selection() const
{
    return caret_ <= anchor_ ? std::make_pair(caret_, anchor_) : std::make_pair(anchor_, caret_);
}

std::wstring TextEditor::selectedText() const
{
    const auto [begin, end] = selection();
    return text_.substr(begin, end - begin);
}

void TextEditor::setText(std::wstring value)
{
    text_ = std::move(value);
    caret_ = text_.size();
    anchor_ = caret_;
}

void TextEditor::clear()
{
    text_.clear();
    caret_ = 0;
    anchor_ = 0;
}

bool TextEditor::deleteSelection()
{
    if (!hasSelection())
    {
        return false;
    }
    const auto [begin, end] = selection();
    text_.erase(begin, end - begin);
    caret_ = begin;
    anchor_ = begin;
    return true;
}

bool TextEditor::insert(const std::wstring& value)
{
    if (value.empty())
    {
        return deleteSelection();
    }
    deleteSelection();
    text_.insert(caret_, value);
    caret_ += value.size();
    anchor_ = caret_;
    return true;
}

bool TextEditor::insertChar(wchar_t ch)
{
    return insert(std::wstring(1, ch));
}

bool TextEditor::backspace(bool wholeWord)
{
    if (deleteSelection())
    {
        return true;
    }
    if (caret_ == 0)
    {
        return false;
    }
    const size_t target = wholeWord ? previousWordBoundary(caret_) : caret_ - 1;
    text_.erase(target, caret_ - target);
    caret_ = target;
    anchor_ = target;
    return true;
}

bool TextEditor::deleteForward(bool wholeWord)
{
    if (deleteSelection())
    {
        return true;
    }
    if (caret_ >= text_.size())
    {
        return false;
    }
    const size_t target = wholeWord ? nextWordBoundary(caret_) : caret_ + 1;
    text_.erase(caret_, target - caret_);
    anchor_ = caret_;
    return true;
}

void TextEditor::collapseTo(size_t position, bool extend)
{
    caret_ = std::min(position, text_.size());
    if (!extend)
    {
        anchor_ = caret_;
    }
}

void TextEditor::moveLeft(bool wholeWord, bool extend)
{
    if (!extend && hasSelection() && !wholeWord)
    {
        const auto [begin, _] = selection();
        caret_ = begin;
        anchor_ = begin;
        return;
    }
    const size_t target = wholeWord ? previousWordBoundary(caret_) : (caret_ > 0 ? caret_ - 1 : 0);
    collapseTo(target, extend);
}

void TextEditor::moveRight(bool wholeWord, bool extend)
{
    if (!extend && hasSelection() && !wholeWord)
    {
        const auto [_, end] = selection();
        caret_ = end;
        anchor_ = end;
        return;
    }
    const size_t target = wholeWord ? nextWordBoundary(caret_) : std::min(caret_ + 1, text_.size());
    collapseTo(target, extend);
}

void TextEditor::moveHome(bool extend)
{
    collapseTo(0, extend);
}

void TextEditor::moveEnd(bool extend)
{
    collapseTo(text_.size(), extend);
}

void TextEditor::selectAll()
{
    anchor_ = 0;
    caret_ = text_.size();
}

void TextEditor::placeCaret(size_t position, bool extend)
{
    collapseTo(position, extend);
}

void TextEditor::selectWordAt(size_t position)
{
    if (text_.empty())
    {
        return;
    }
    const size_t clamped = std::min(position, text_.size() - 1);
    if (!isWordChar(text_[clamped]))
    {
        anchor_ = clamped;
        caret_ = std::min(clamped + 1, text_.size());
        return;
    }

    size_t begin = clamped;
    while (begin > 0 && isWordChar(text_[begin - 1]))
    {
        --begin;
    }
    size_t end = clamped;
    while (end < text_.size() && isWordChar(text_[end]))
    {
        ++end;
    }
    anchor_ = begin;
    caret_ = end;
}

size_t TextEditor::previousWordBoundary(size_t from) const
{
    size_t index = std::min(from, text_.size());
    while (index > 0 && !isWordChar(text_[index - 1]))
    {
        --index;
    }
    while (index > 0 && isWordChar(text_[index - 1]))
    {
        --index;
    }
    return index;
}

size_t TextEditor::nextWordBoundary(size_t from) const
{
    size_t index = std::min(from, text_.size());
    while (index < text_.size() && isWordChar(text_[index]))
    {
        ++index;
    }
    while (index < text_.size() && !isWordChar(text_[index]))
    {
        ++index;
    }
    return index;
}
